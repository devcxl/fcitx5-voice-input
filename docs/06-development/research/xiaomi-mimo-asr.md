# 调研：小米 MiMo ASR 模型与接入评估

> 面向 fcitx5-voice-input（C++ Fcitx5 语音输入插件，采集 16kHz mono int16，VAD 分段后提交 ASR）。
> 调研日期：2026-09-25。调研 agent：@researcher。
> 本文件仅含事实与来源标注，方案为决策参考；事实与推断严格区分，推断处标注「推断」。
> 所有 URL 均来自本次 web 搜索/抓取的原文，未凭记忆编造。

---

## 0. 调研结论摘要（带置信度标注）

| # | 结论 | 置信度 | 依据 |
|---|------|--------|------|
| C1 | **确有其专门的 ASR 模型**：官方名为 **MiMo-V2.5-ASR**（`XiaomiMiMo/MiMo-V2.5-ASR`），由小米 MiMo 团队端到端训练。**不存在** `MiMo-ASR-1.0` 之类的命名 | 高 | [官方博客][mimo-blog]、[GitHub][mimo-gh]、[HF][mimo-hf] |
| C2 | **MiMo-Audio 与 MiMo-V2.5-ASR 是两条不同产品线**：`MiMo-Audio-Tokenizer`（1.2B, 25Hz RVQ）与 `MiMo-Audio-7B-Base/Instruct` 是「音频理解 LLM（few-shot）」，**被 MiMo-V2.5-ASR 复用作 tokenizer**；三者名字都真实存在 | 高 | [MiMo-Audio GitHub][mimo-audio-gh]、[arXiv 2512.23808][mimo-audio-paper]、[Tokenizer HF][mimo-tok-hf] |
| C3 | **开源权重可用**：HF + ModelScope，8B decoder + 1.2B tokenizer，`safetensors`/`bfloat16`；HF 模型卡标 **MIT**，GitHub 仓库页标 **Apache-2.0**（两者均宽松、均允许商用，但标注不一致） | 高（存在）/ 中（license 一致性） | [HF 文件树][mimo-hf-tree]、[HF model card][mimo-hf]、[config.json][mimo-config]、[GitHub][mimo-gh] |
| C4 | **官方云 API 存在且 OpenAI 兼容**：`POST https://api.xiaomimimo.com/v1/chat/completions`，`model=mimo-v2.5-asr`，音频走 `messages[].content[].input_audio` 的 base64 Data URL；认证头两种任选：`api-key` 或 `Authorization: Bearer`；无独立 `/v1/audio/transcriptions` | 高 | [API 文档（中文）][mimo-api-zh]、[API 文档（英文）][mimo-api-en]、[官方 FAQ][mimo-faq] |
| C5 | **是「整段音频上传 + SSE 输出流」而非实时音频流**：不支持 WebSocket/realtime 音频推流；`stream=true` 只对**输出 token** 做 SSE，音频必须先完整 base64 提交 | 高（无 WS 端点）/ 中（增量粒度未实测） | [使用指南][mimo-guide]、[API 文档][mimo-api-zh]、[第三方 issue 佐证][hermes-issue] |
| C6 | **音频格式硬约束**：仅支持 `wav`/`mp3`，base64 编码后 **≤ 10 MB**；不接受原始 PCM（需自行封装 WAV）。派生上限：16kHz mono wav 约 **≤ 3.9 分钟** | 高（限制原文）/ 中（时长换算为推断） | [使用指南][mimo-guide] |
| C7 | **计费按音频时长**：¥0.5/小时、$0.074/小时；模型规格 8K 上下文 / 2K 最大输出 / 100 RPM / 10K TPM | 高 | [官方定价页][mimo-price]、[模型页][mimo-models] |
| C8 | **本地推理栈**：需 Python 3.12 + CUDA ≥12 + `flash-attn==2.7.4.post1` + PyTorch（transformers，`model_type: qwen2`）。**无官方 ONNX / llama.cpp / GGUF 路径**；社区有 Apple MLX 4-bit 移植与第三方 OpenASR `q4_k` `.oasr` 包（峰值内存 7.6 GB、约 0.5× 实时） | 中 | [GitHub Getting Started][mimo-gh]、[config.json][mimo-config]、[MLX][mimo-mlx]、[OpenASR][openasr] |
| C9 | **接入本项目成本很低**：现有 OpenAI **`chat` 模式**已经构造 `data:audio/wav;base64,...` 且解析 `choices[0].message.content`，与 MiMo 云 API 请求/响应高度一致；理论上改 BaseUrl/Model 即可接通。存在 2 个细节风险：`asr_options.enable_itn` 额外字段、`language` 因现有代码覆盖 bug 实际未发送；鉴权已确认 Bearer 可用，不再是风险。两项风险的处置见 §9 | 中-高 | 本地源码 [openai_asr.cpp](../../../src/addon/asr/openai_asr.cpp)、[API 文档][mimo-api-zh]、[官方 FAQ][mimo-faq] |
| C10 | **不具备「边说边出」能力**：云 API 为一次性分段提交，无实时增量音频流；对语音输入法场景，延迟/交互不如现有 OpenAI Realtime、火山、Mistral 三个 WS 流式后端，但**中文/方言准确率与成本**有优势 | 高 | C5、C7、[官方博客][mimo-blog] |
| C11 | **截至 2026-09-25，ASR 仍是 `mimo-v2.5-asr`**：2026-09-22 发布的 MiMo-V2.6 系列为语言模型（Pro/Flash/UltraSpeed），不含新 ASR；ASR 定价页仍只有 v2.5-asr | 中-高 | [官方定价页（更新 2026-09-21）][mimo-price]、[模型发布日志][mimo-release]、[V2.6 发布][mimo-v26] |
| C12 | **官方中文基准领先**：AiShell-2 2.52% CER、Fleurs-Zh 2.41%、Wenet Meeting 5.92%、Wenet Net 5.26%；英文 Open ASR 平均 5.73% WER（LibriSpeech-clean 1.45%）；粤语 WeNet-Yue 7.21%、Fleurs-Yue 3.28%；**均为小米自评，缺独立复现** | 高（官方数字）/ 中（可信度） | [官方博客评测表][mimo-blog] |
| C13 | **无实时因子官方数据**；第三方 OpenASR 包标 0.5× 实时（即 RTF≈2，比实时慢一倍），YouTube 实测约 18 GB 显存（bf16）。**本地实时输入不现实** | 中-低 | [OpenASR][openasr]、[YouTube 实测][yt-review] |
| C14 | **隐私**：云 API 会把整段语音上传至小米服务器；想要离线需本地跑 8B+1.2B 模型（≥16 GB 显存）或第三方量化运行时 | 高 | [API 文档][mimo-api-zh]、[HF 文件树][mimo-hf-tree] |

**一句话结论**：MiMo-V2.5-ASR 是真实、已开源、且官方云 API 已开放的**中文/方言强项 ASR 模型**；对本项目而言，接云 API 的**工程量极小**（现有 chat 模式几乎可直接复用），但它是**一次性上传的分段转录**，不具备实时 WS 流式音频输入，因此定位应类似现有 whisper-1 HTTP 后端，而非 Realtime/火山/Mistral 三个流式后端。

---

## 1. 模型本体：MiMo 系列里到底哪个是 ASR

### 1.1 产品线澄清（对应问题 1）

MiMo 家族中与「语音」相关的有三个不同层次，必须区分：

| 名称 | 性质 | 规格 | 与 ASR 的关系 | 来源 |
|------|------|------|--------------|------|
| **MiMo-V2.5-ASR** | **专用端到端 ASR 模型**（本次调研主角） | 8B decoder + 1.2B audio tokenizer | 直接语音转文字 | [GitHub][mimo-gh]、[HF][mimo-hf] |
| **MiMo-Audio-7B-Base / Instruct** | 音频理解 LLM（few-shot，多任务） | 7B LLM，基于 MiMo-7B | 能做 ASR，但定位是通用音频理解，非专用识别 | [MiMo-Audio GitHub][mimo-audio-gh]、[arXiv][mimo-audio-paper] |
| **MiMo-Audio-Tokenizer** | 音频离散 tokenizer | 1.2B，25 Hz，8 层 RVQ，200 token/s | 被 MiMo-V2.5-ASR 复用为前端 | [Tokenizer HF][mimo-tok-hf]、[arXiv][mimo-audio-paper] |
| MiMo-V2.5 / V2.6（Omni） | 原生全模态**语言**大模型，可「听懂」音频 | 310B/1T MoE | 不是专用 ASR，输出推理/对话 | [MiMo-V2.5 页][mimo-v25]、[V2.6][mimo-v26] |

- **没有** `MiMo-ASR-1.0`、`MiMo-ASR-V2` 之类命名；专用 ASR 的唯一官方名是 `mimo-v2.5-asr`（模型 ID）/ `MiMo-V2.5-ASR`（展示名）。置信度：高（官方 API 文档 `model` 字段仅列 `mimo-v2.5-asr`）。
- **MiMo-Audio-7B-Instruct 确实是真实名字**，属于 2025 年 9 月发布的 MiMo-Audio（arXiv 2512.23808）系列；**MiMo-Audio-Tokenizer 也是真实名字**。它们**不是** MiMo-V2.5-ASR 的别名，而是其架构前端来源。置信度：高。

### 1.2 架构与规格（一手）

来自 [config.json][mimo-config]（HF 官方权重）：

- `architectures: ["MiMoV2ASRForCausalLM"]`，`model_type: "qwen2"`，`transformers_version: 4.57.1`，`dtype: bfloat16`。
- Decoder：`num_hidden_layers: 36`、`hidden_size: 4096`、`num_attention_heads: 32`、`num_key_value_heads: 8`、`max_position_embeddings: 8192`。
- 音频前端：`audio_config`（8 通道 RVQ、`group_size: 4` patch、`speech_vocab_size` 为 8 层码本、`audio_segment_size: 6000`）。
- 权重：7 个 `model-0000x-of-00007.safetensors`，HF 显示总大小约 **32 GB（F32 标注，config 为 bfloat16）**；`model size 8B params`。实测 bf16 加载约 18 GB 显存（第三方）。[HF 文件树][mimo-hf-tree]、[YouTube][yt-review]

推理入口为 Python `MimoAudio.asr_sft(path, audio_tag="<chinese>|<english>|Auto")`，**文件级、离线**；官方 demo 仅提供 Gradio 上传/录音，未见 chunk 流式接口。[GitHub][mimo-gh]

### 1.3 语言与场景

官方宣称：中英双语 + 吴语/粤语/闽南语/四川话/河南/东北/陕西等方言、code-switch、歌词、强噪声、远场、多说话人、知识密集内容、原生标点、无需语言标签。[GitHub][mimo-gh]、[官方博客][mimo-blog]

---

## 2. 访问方式

### 2.1 开源权重（自托管）

| 项 | 值 | 来源 |
|----|----|------|
| GitHub | `github.com/XiaomiMiMo/MiMo-V2.5-ASR`（Python，2026-04-23 创建，约 331 stars / 34 forks，10 open issues） | [GitHub][mimo-gh] |
| HuggingFace | `XiaomiMiMo/MiMo-V2.5-ASR`（~106 likes，月下载约 3,244） | [HF][mimo-hf] |
| ModelScope | `XiaomiMiMo/MiMo-V2.5-ASR`（HF README 提供） | [HF][mimo-hf] |
| Tokenizer | `XiaomiMiMo/MiMo-Audio-Tokenizer`（必须一起下载） | [Tokenizer HF][mimo-tok-hf] |
| 框架依赖 | transformers（`qwen2` 架构，`trust_remote_code`）+ `flash-attn==2.7.4.post1` | [GitHub][mimo-gh]、[config.json][mimo-config] |
| 硬件前提 | Linux、Python 3.12、CUDA ≥12.0 | [GitHub][mimo-gh] |

**License 存在标注不一致**（重要，但两者都宽松）：

- HF 模型卡元数据：`License: mit`。[HF][mimo-hf]
- GitHub 仓库页：`Apache-2.0 license`。[GitHub][mimo-gh]
- 小米对 MiMo-V2.5 系列开源公告统一表述为 **MIT**，允许商用推理与二次训练、无需额外授权。[V2.5 开源公告][mimo-v25-oss]

→ 结论：可用于商用/再分发的置信度 **高**；具体适用哪份 license 文本建议以权重仓库实际 `LICENSE` 文件为准（本次未抓取到该文件，标「未证实」）。

### 2.2 官方云 API（OpenAI 兼容 Chat Completions）

- **端点**：`POST https://api.xiaomimimo.com/v1/chat/completions`（中文文档与英文文档一致）。[API 文档][mimo-api-zh]、[API 文档][mimo-api-en]
- **认证**：支持两种，任选其一：`api-key: $MIMO_API_KEY` 或 `Authorization: Bearer $MIMO_API_KEY`（官方 FAQ 明确列出）。官方 Python 示例用 `OpenAI(base_url=..., api_key=...)`（SDK 默认发 `Authorization: Bearer`），第三方集成 issue 亦记 `Authorization: Bearer`。[官方 FAQ][mimo-faq]、[API 文档][mimo-api-zh]、[hermes issue][hermes-issue]
  → 对本项目含义：现有 `Authorization: Bearer` 发送方式**可直接使用**，无需新增鉴权头配置。
- **请求体**：`model=mimo-v2.5-asr`；`messages[0].content[0].type="input_audio"`、`input_audio.data="data:{MIME};base64,..."`；`asr_options.language ∈ {auto,zh,en}`；`stream` bool。
- **响应**：`choices[0].message.content` 为文本；`usage.seconds`（音频时长）、`usage.prompt_tokens_details.audio_tokens`（音频 token 数，示例 4 秒=25 token，即 6.25 token/s，与 patch 率一致）。
- **计费/限额**：¥0.5/小时、$0.074/小时；RPM 100、TPM 10K；8K 上下文、2K 最大输出。[定价页][mimo-price]、[模型页][mimo-models]
- **平台定位**：小米 MiMo 开放平台同时提供 OpenAI 兼容与 Anthropic 兼容 API；2026-06-23 起支持 OpenAI Responses API。[API 平台更新日志][mimo-release]

### 2.3 端侧 / 小米设备

- 官方渠道为「云 API + 开源权重」两条路；未发现面向第三方 App 的端侧 SDK，也未发现「仅限小米设备」的公开 ASR 接口。澎湃 OS 内置能力**未在公开文档中说明**（标「未证实」）。
- 另有第三方「OpenASR」提供 `.oasr` 纯本地运行时（见 §4.3），非小米官方。

---

## 3. 协议与流式语义（对应问题 3）

### 3.1 云 API 的真实流式程度（关键）

- **没有 WebSocket / Realtime 音频端点**。平台文档仅有 Chat Completions；第三方集成 issue 明确指出 ASR 走 `/v1/chat/completions` 而**不是** `/v1/audio/transcriptions`。[API 文档][mimo-api-zh]、[hermes issue][hermes-issue]
- `stream=true` 使用 **SSE**（server-sent events），但**输入音频必须一次性完整提交**（base64 Data URL），因此它是「输出 token 流」，不是「边说边发音频流」。置信度：高（输入侧）；输出是否逐 token 到达：中（未实测）。
- 旁证：同平台 TTS 文档明确写「流式调用当前降级为兼容模式，仅在全部推理完成后以流式形式返回一次结果」——说明该平台的「stream」并不保证真增量。[TTS 文档摘录][mimo-tts-stream]
- 对本项目含义：**无法映射到 realtime 后端的增量语义**；最接近的现有实现是 **whisper-1 HTTP 一次性转录**（`AsrSession::End()` 时整段提交、返回终稿）。

### 3.2 音频格式与限制

| 项 | 约束 | 来源 |
|----|------|------|
| 容器 | 仅 `wav`、`mp3`（不支持裸 PCM） | [使用指南][mimo-guide] |
| MIME | wav→`audio/wav`；mp3→`audio/mpeg`/`audio/mp3` | [使用指南][mimo-guide] |
| 编码后大小 | base64 字符串 **≤ 10 MB** | [使用指南][mimo-guide] |
| 采样率/声道 | 文档未规定（推断：随容器自适应，模型前端做重采样） | —（未证实） |
| 时长上限 | 文档未给秒数；由 10MB 反推 wav@16k mono ≈ **3.9 分钟**；由 8K 上下文与 6.25 token/s 反推 ≈ 21 分钟，故 **10MB 是实际瓶颈** | 推断（基于 [使用指南][mimo-guide]、[API 文档][mimo-api-zh]） |
| 超时行为 | 文档未说明；官方客户端未见显式超时字段 | 未证实 |
| 语言 | `asr_options.language` 仅支持单语种 `auto`/`zh`/`en`；方言/code-switch 靠 `auto` 自动处理 | [API 文档][mimo-api-zh]、[模型页][mimo-models] |

> 对本项目：默认 `maxSpeechMs=30000`（30 秒）对应 wav 约 960 KB、base64 约 1.28 MB，**远低于 10 MB 限制**；即使把上限调到 60 秒（约 2.56 MB）也安全。

### 3.3 端侧运行不是流式

开源仓库的 `asr_sft()` 为文件级调用；官方 issue 中有人询问时间戳输出，仍处 open 状态，说明功能面较基础。[GitHub Issues][mimo-issues]

---

## 4. 本地推理栈（对应问题 4）

### 4.1 官方路径

- PyTorch + transformers（`qwen2` 架构，需 `trust_remote_code`）；CUDA ≥12，Python 3.12，`flash-attn==2.7.4.post1`（官方给出预编译 wheel 兜底）。[GitHub][mimo-gh]
- 需同时加载 `MiMo-Audio-Tokenizer`（1.2B）与 `MiMo-V2.5-ASR`（8B）两个权重。[HF][mimo-hf]
- **无官方 ONNX 导出、无官方 llama.cpp/GGUF、无 vLLM 接入文档**（vLLM/vLLM-Omni 的 MiMo 案例针对的是 MiMo-Audio-7B 与 MiMo-V2.5 语言模型，不是本 ASR）。[vLLM-Omni MiMo-Audio][vllm-omni]、[llama.cpp issue][llama-issue]

### 4.2 重采样

- 模型侧未要求 24kHz（与 OpenAI Realtime/whisper-1 场景不同）；但云 API 只收 wav/mp3，**原始 16kHz PCM 需包一层 WAV 头**（本项目 `openai_asr.cpp` 已有 `FloatPcmToWav`）。是否内部重采样到固定率：文档未说明，**未证实**。
- 本地推理对 16kHz 输入的处理需看 `asr_sft` 实现（本次未抓到 `src/mimo_audio` 源码，标「未证实」）。

### 4.3 社区/第三方路径

| 方案 | 描述 | 数据 | 置信度 |
|------|------|------|--------|
| Apple MLX 4-bit | `mlx-community/MiMo-V2.5-ASR-MLX`，Apple Silicon | 未给体积/速度 | 中（有 HF 页，仅 macOS） |
| OpenASR `.oasr` | 第三方 q4_k 量化，纯本地、无 Python 推理；提供 `openasr serve`（OpenAI 兼容 `audio.transcriptions`） | 5.4 GB、峰值内存 7.6 GB、**0.5× 实时**（RTF≈2）、仅英/粤/普 | 中-低（单一第三方站，未独立验证） |
| GGUF | 仅 `MiMo-V2.5` **文本** LLM 的 GGUF，明确「不含音频输入，需上游 llama.cpp 支持」；**不适用于 ASR** | — | 高（明确说明） |

**结论**：目前**没有可被 C++ 直接 dlopen 的官方推理库**；C++ 接入现实路径只有「调云 API」或「本地跑一个 OpenAI 兼容 sidecar 服务（如 OpenASR）再复用现有 HTTP 后端」。后者性能（RTF≈2）与可信度都不足，暂不建议作为默认。

---

## 5. 接入 fcitx5-voice-input 的具体路径（对应问题 5）

### 5.1 最接近的现有后端：OpenAI `chat` 模式（不是 realtime/volcengine）

本地源码 `src/addon/asr/openai_asr.cpp` 的 `chat` 分支与 MiMo 云 API 对照：

| 环节 | 现有 `chat` 模式 | MiMo 云 API 要求 | 匹配 |
|------|-----------------|-----------------|------|
| 端点 | `{baseUrl}/chat/completions` | `/v1/chat/completions` | ✅ |
| 音频 | `data:audio/wav;base64,<b64>` | 同 | ✅ |
| 消息 | `type=input_audio` + `input_audio.data` | 同 | ✅ |
| 语言 | 设 `asr_options.language`（但见 5.2 bug） | `asr_options.language` | ⚠️ |
| 响应解析 | `choices[0].message.content` | 同 | ✅ |
| 鉴权 | `Authorization: Bearer` | 文档 `api-key`（Bearer 待证实） | ⚠️ |
| 其它字段 | 额外发 `asr_options.enable_itn=true` | 文档未列 `enable_itn` | ⚠️ |

**因此，零/低代码接入的可行性很高**：给用户一个「MiMo」后端（或直接指引在现有 OpenAI 子配置里设 `BaseUrl=https://api.xiaomimimo.com/v1`、`Model=mimo-v2.5-asr`、`ApiMode=chat`）即可发起转录。

### 5.2 需要修复/注意的细节与工作量

1. **`language` 覆盖 bug（必看）**：`openai_asr.cpp` 中先 `body["asr_options"]["language"]=...`，随后 `body["asr_options"] = asrOpts;`（仅含 `enable_itn`）整体覆盖，导致 **language 从未真正发送**。若新增 MiMo 后端需修正；若沿用现有 chat 模式则只能走 `auto`（MiMo 默认且推荐，影响可接受，但中文用户失去 `zh` 强制选项）。
2. **`enable_itn` 额外字段**：MiMo 文档的 `asr_options` 只列 `language`。该字段源自阿里 Bailian 兼容路径，MiMo 可能忽略也可能 400。建议新增专用后端时不发该字段（最小改动：为 MiMo 走一个不带 `enable_itn` 的分支）。
3. **鉴权头（已解决）**：官方 FAQ 明确 `api-key` 与 `Authorization: Bearer` 两种均可，现有实现的 Bearer 头发送方式**无需改动**。[官方 FAQ][mimo-faq]
4. **超时**：现有 `CURLOPT_TIMEOUT=30L`。30 秒语音在云上转录通常够用，但长段 + 排队可能超时，建议 MiMo 后端放宽（如 60–120s），并保留取消回调。
5. **无需新增依赖、无需重采样**：WAV 封装与 base64、libcurl HTTP 均已具备。这是相比 realtime 后端的一大优势。
6. **架构落点**：新增 `AsrEngine`/`AsrSession` 实现（可高度复用 `OpenaiAsrSession` 的一次性提交模型）+ `voiceinput-config.h` 新增 `MimoAsrConfig` 子块 + `AsrBackendAnnotation` 增 `mimo` 枚举项。工作量：**小**（1 个 .cpp/.h + 配置 + CMake 列表 + i18n）。

### 5.3 备选方案

| 方案 | 描述 | 增量体验 | 工作量 | 成本/风险 |
|------|------|---------|--------|-----------|
| **A. 云 API - 复用现有 chat 模式** | 仅改 BaseUrl/Model/ApiMode | 无（一次性） | 极小 | 受 §5.2 细节影响；无语言强制 |
| **B. 云 API - 新增 `mimo` 后端** | 复制 chat 路径，修正 language/鉴权/超时 | 无（一次性） | 小 | 推荐；隔离未来 API 变化 |
| **C. 本地 sidecar（OpenASR）** | 本地 `openasr serve`，复用 whisper HTTP 后端指向 localhost | 无 | 小（项目侧） | 隐私好，但 RTF≈2、第三方可信度低、仅英/粤/普 |
| **D. 官方自托管（8B+1.2B）** | 自建 GPU 服务暴露 OpenAI 兼容接口 | 无 | 大（外部） | 需 ≥16GB 显存，无 CPU/C++ 路径 |

**建议**：若目标是「中文/方言高准确率、可接受一次性返回」，选 **B**（新增 `mimo` 后端），关键前置是修 language/鉴权/超时；若只做快速验证，先用 **A** 打通。**不建议**用 MiMo 替代现有三个 WS 流式后端来追求增量体验。

---

## 6. 横向对比（对应问题 6）

### 6.1 准确率（小米官方自评，[官方博客][mimo-blog]；低 WER/CER 更好）

| 模型 | AiShell-2 | Fleurs-Zh | Wenet Meeting | Wenet Net | 英文 Open ASR 平均 | LS-clean | WeNet-Wu | m4singer |
|------|-----------|-----------|---------------|-----------|-------------------|----------|----------|----------|
| **MiMo-V2.5-ASR** | **2.52** | **2.41** | 5.92 | 5.26 | **5.73** | **1.45** | **19.55** | 3.95 |
| Qwen3-ASR-1.7B | 2.67 | 3.21 | 5.90 | 4.94 | 5.76 | 1.63 | 24.29 | 4.82 |
| FunASR-1.5 | 2.57 | 2.75 | 5.95 | 5.28 | 5.88 | 1.50 | 29.08 | 5.58 |
| Seed-ASR 2.0 | 2.63 | 3.31 | 7.22 | 4.89 | 8.09 | 2.81 | 32.59 | 9.46 |
| Gemini-3.1-Pro | 4.52 | 3.30 | 12.09 | 9.69 | — | — | 64.87 | 4.25 |
| Whisper-large-v3 | — | — | — | — | 7.44 | 2.01 | — | — |

> 注：表中未列 whisper 的中文 CER。全部为**小米公布的评测结果**，未见第三方独立复现（标「中」置信度）；OpenASR 页引用了同样的数字。[OpenASR][openasr]

### 6.2 与本项目现有后端对比

| 维度 | MiMo 云 API | OpenAI whisper-1 HTTP（现状） | OpenAI Realtime / 火山 / Mistral（WS） |
|------|------------|------------------------------|----------------------------------------|
| 传输 | HTTP Chat Completions（整段 base64） | HTTP multipart（整段） | WebSocket 推流 |
| 增量 | 无（SSE 仅输出） | 无 | 有 |
| 首字延迟 | 一次性返回 | 一次性 | 数百 ms 级（各后端不同） |
| 中文/方言 | **强（官方 SOTA 宣称）** | 一般（方言弱） | 取决于后端 |
| 成本 | **$0.074/h ≈ $0.00123/min** | whisper-1 ≈ $0.006/min | realtime ≈ $0.017/min |
| 格式 | 仅 wav/mp3，≤10MB | 多种音频格式 | 常需 24kHz PCM 等 |
| 隐私 | 上传小米 | 上传所选云 | 上传所选云 |
| 实现成本 | **极低**（可复用 chat 模式） | 已有 | 已有 |

> 成本换算：$0.074/小时 ÷ 60 ≈ **$0.00123/分钟**，约为 OpenAI whisper-1 标价的 1/5、Realtime 的 1/14。[定价页][mimo-price]

---

## 7. 风险与不确定性（对应问题 7）

1. **非实时**：无 WS/realtime 音频输入；对本插件「边说边出」的价值有限。属结构性限制，非临时状态。置信度：高。
2. **输出流式是否真增量未知**：`stream=true` 的 SSE 在 TTS 侧已被官方明确降级为「推理完成后一次性返回」，ASR 侧无同类说明，需最小原型实测。置信度：中。
3. ~~鉴权头不确定~~（已解决）：官方 FAQ 明确两种头均可，Bearer 可直接用。置信度：高。[官方 FAQ][mimo-faq]
4. **`enable_itn` 兼容性未知**：现有 chat 模式多发一个 MiMo 未文档化字段，可能被忽略或报错。置信度：中。
5. **License 标注不一致**：HF=MIT、GitHub=Apache-2.0；均为宽松许可，商用风险低，但合规文本需以仓库 `LICENSE` 为准。置信度：中。
6. **服务稳定性/生命周期**：MiMo 平台迭代很快（V2 → V2.5 已下线旧版；V2.6 已发布），存在模型/端点调整风险；但 ASR 当前为唯一 ASR 型号且仍在售。置信度：中-高。
7. **本地路径门槛高**：8B+1.2B、CUDA、flash-attn 固定版本（编译易踩坑）、无 ONNX/llama.cpp。C++ 端无直接推理路径。置信度：高。
8. **社区活跃度有限**：GitHub ~331 stars、10 open issues、仅 1 个公开 issue 且无官方响应；官方迭代节奏偏向语言模型，ASR 维护力度未知。置信度：中。
9. **关于「Muon 优化器 / tokenizer 依赖」**：本次未找到 MiMo-V2.5-ASR 使用 Muon 优化器的官方说明，**标「未证实」**；tokenizer 依赖确认为必须额外下载 1.2B 的 `MiMo-Audio-Tokenizer`（双模型加载是主要工程负担）。[GitHub][mimo-gh]、[config.json][mimo-config]
10. **无公开的独立延迟/RTF 数据**：仅有第三方 OpenASR「0.5× 实时」与 YouTube「18GB 显存」的二手数据。置信度：低-中。

---

## 8. 元评审（剩余不确定与最弱证据）

- **最弱证据**：
  1. C13 的本地性能数据（0.5× 实时、18GB）来自单一第三方站与一个 YouTube 视频，未做基准复现；
  2. OpenASR `.oasr` 运行时的真实性与性能（§4.3）未能独立验证，仅凭其官网自称；
  3. 「Bearer 是否可用」已由官方 FAQ 确认（两种头均可）；「ASR SSE 是否逐 token」仍为文档未明确项，需实测。
- **最强证据**：模型存在性/命名/开源地址（GitHub+HF 官方双源）、云 API 端点与请求/响应结构（中英文官方文档一致）、格式与 10MB 限制、定价（官方定价页 2026-09-21 更新）。这些置信度高。
- **未覆盖**：官方仓库 `src/mimo_audio` 源码（抓取 CRAWL_NOT_FOUND）、权重仓库 `LICENSE` 文件正文、`api.xiaomimimo.com/v1/audio/transcriptions` 是否实际存在（未发探测请求）。因此「无独立 transcription 端点」基于文档与第三方 issue，置信度高但非直接探测。
- **时效提醒**：调研日 2026-09-25；MiMo 平台更新频繁，接入前应复核 [API 文档][mimo-api-zh] 与 [定价页][mimo-price]。
- **对项目决策的关键提醒**：**不要**把 `language` 覆盖 bug 与 `enable_itn` 兼容性当作可忽略项；建议先写一个 20 行的独立 curl/最小 C++ 探针验证「Bearer + 无 enable_itn + zh」是否能 200。

---

## 9. 决策与实施记录（2026-09-25）

最终决策：**采用方案 A+——不新增 `mimo` 独立后端，改为修复现有 OpenAI `chat` 模式使其可直接对接 MiMo 云 API**（用户确认范围；理由：chat 路径已覆盖绝大部分请求/响应形态，新增后端只增加配置面与维护点）。

已实施（本地离线验证，`ctest` 12/12 通过）：
- 新增请求体构造纯函数 `src/addon/asr/utils/chat_asr_request.{h,cpp}`，单元测试 `tests/chat_asr_request_test.cpp` 覆盖 4 种 language/ITN 组合。
- **修复 language 覆盖 bug**：原实现 `body["asr_options"] = asrOpts` 会整体覆盖、丢弃 language；现在两者统一构造，`auto`/空值时省略 language。
- **`enable_itn` 改为可配置**：`OpenAIAsrConfig.EnableItn`（默认 `true`，保持 DashScope 行为）；MiMo 用户设 `false`。
- 用法（无需新后端）：`ActiveBackend=openai`、`BaseUrl=https://api.xiaomimimo.com/v1`、`Model=mimo-v2.5-asr`、`ApiMode=chat`、`Language=zh`、`EnableItn=false`。

未实测项（无 API Key，待首次联调按序确认）：
1. `Authorization: Bearer` 实网可用性（官方 FAQ 称两种头均可）；
2. MiMo 对 `asr_options.enable_itn` 未知字段的容忍度：若忽略，`EnableItn=false` 无副作用；若 400，则必须为 false（本实现已提供开关）；
3. 30s `CURLOPT_TIMEOUT` 对长段/排队是否足够（本次未放宽；实测超时再评估 per-backend 超时）；
4. SSE 增量（`stream=true`）是否逐 token 到达——当前实现只用 `stream=false`，不影响接入。

决策变更触发条件：若 MiMo 推出 WS 实时音频接口，或 chat 路径出现多家供应商参数分歧，再评估方案 B（独立 `mimo` 后端）。

## 参考链接

**小米官方（一手）**
- [MiMo-V2.5-ASR 官方博客（含完整评测表）][mimo-blog] — 架构、场景、全部 benchmark 数字、April 2026
- [MiMo-V2.5-ASR GitHub][mimo-gh] — 开源代码、安装、`asr_sft` API、Apache-2.0 标注、模型下载
- [MiMo-V2.5-ASR HuggingFace 模型卡][mimo-hf] — MIT license、8B、ModelScope、Gradio demo
- [MiMo-V2.5-ASR HF 文件树][mimo-hf-tree] — safetensors 分片与体积
- [MiMo-V2.5-ASR config.json][mimo-config] — `MiMoV2ASRForCausalLM`、qwen2、bfloat16、层数/维度
- [MiMo-Audio-Tokenizer 模型卡][mimo-tok-hf] — 1.2B、25Hz RVQ、200 token/s
- [MiMo-Audio GitHub][mimo-audio-gh] / [arXiv 2512.23808][mimo-audio-paper] — 音频理解 LLM 与 patch 机制
- [MiMo-Audio-7B-Instruct HF][mimo-audio-7b] — Instruct 模型
- [语音识别 API 文档（中文）][mimo-api-zh] — 端点、请求/响应字段、`asr_options`
- [Speech Recognition API（英文）][mimo-api-en] — 同上英文版
- [语音识别使用指南][mimo-guide] — wav/mp3、10MB、stream 示例
- [MiMo-V2.5-ASR 模型页][mimo-models] — 8K/2K、RPM/TPM、示例代码、方言样例
- [官方定价页（2026-09-21 更新）][mimo-price] — ¥0.5/h、$0.074/h
- [模型发布日志][mimo-release] — 2026-06-02 asr 发布、平台更新
- [V2.5 TTS+ASR 发布公告][mimo-news] — ASR 开源、demo/权重地址
- [MiMo-V2.5 系列开源公告（MIT）][mimo-v25-oss] — license 表述
- [MiMo-V2.6 发布][mimo-v26] — 2026-09-22，系列内无新 ASR
- [MiMo-V2.5 语言模型页][mimo-v25] — 全模态 LLM（非 ASR）
- [MiMo-Skills 仓库][mimo-skills] — Agent skills（目前仅 TTS，无 ASR skill）
- [MiMo-V2.5-ASR Issues][mimo-issues] — 时间戳支持 open issue

**第三方 / 社区（二手，辅助）**
- [OpenASR: MiMo-V2.5-ASR 本地包][openasr] — q4_k、7.6GB、0.5× 实时、`.oasr`、本地 OpenAI 兼容服务
- [mlx-community/MiMo-V2.5-ASR-MLX][mimo-mlx] — Apple Silicon MLX 4-bit 移植
- [NousResearch/hermes-agent issue #46257][hermes-issue] — 第三方集成记录：chat/completions、Bearer、非 transcriptions
- [YouTube: MiMo-V2.5-ASR 本地实测][yt-review] — 约 18GB 显存（二手）
- [vLLM-Omni MiMo-Audio serving][vllm-omni] — MiMo-Audio（非本 ASR）在线服务示例
- [AesSedai/MiMo-V2.5-GGUF][llama-gguf] — 文本 LLM GGUF，明确不含音频
- [llama.cpp issue #22469][llama-issue] — MiMo V2.5 支持请求（Closed as not planned）

[mimo-blog]: https://mimo.xiaomi.com/mimo-v2-5-asr
[mimo-gh]: https://github.com/XiaomiMiMo/MiMo-V2.5-ASR
[mimo-hf]: https://huggingface.co/XiaomiMiMo/MiMo-V2.5-ASR
[mimo-hf-tree]: https://huggingface.co/XiaomiMiMo/MiMo-V2.5-ASR/tree/main
[mimo-config]: https://huggingface.co/XiaomiMiMo/MiMo-V2.5-ASR/blob/main/config.json
[mimo-tok-hf]: https://huggingface.co/XiaomiMiMo/MiMo-Audio-Tokenizer
[mimo-audio-gh]: https://github.com/XiaomiMiMo/MiMo-Audio
[mimo-audio-paper]: https://arxiv.org/abs/2512.23808
[mimo-audio-7b]: https://huggingface.co/XiaomiMiMo/MiMo-Audio-7B-Instruct
[mimo-faq]: https://mimo.mi.com/docs/zh-CN/quick-start/faq/api-integration
[mimo-api-zh]: https://mimo.mi.com/docs/zh-CN/api/audio/Speech-Recognition
[mimo-api-en]: https://mimo.mi.com/docs/en-US/api/audio/Speech-Recognition
[mimo-guide]: https://mimo.mi.com/docs/en-US/quick-start/usage-guide/audio/Speech-Recognition
[mimo-models]: https://mimo.mi.com/models/zh-CN/mimo-v2.5-asr
[mimo-price]: https://mimo.mi.com/docs/zh-CN/price/pay-as-you-go
[mimo-release]: https://mimo.mi.com/docs/en-US/updates/model
[mimo-news]: https://mimo.mi.com/docs/zh-CN/news/latest/v2.5-tts-release
[mimo-v25-oss]: https://mimo.mi.com/docs/en-US/news/latest/v2.5-open-sourced
[mimo-v26]: https://mimo.xiaomi.com/mimo-v2-6
[mimo-v25]: https://mimo.xiaomi.com/mimo-v2-5
[mimo-skills]: https://github.com/XiaomiMiMo/MiMo-Skills
[mimo-issues]: https://github.com/XiaomiMiMo/MiMo-V2.5-ASR/issues
[mimo-tts-stream]: https://platform.xiaomimimo.com/docs/usage-guide/speech-synthesis
[openasr]: https://openasr.org/models/mimo-v2.5-asr
[mimo-mlx]: https://huggingface.co/mlx-community/MiMo-V2.5-ASR-MLX
[hermes-issue]: https://github.com/NousResearch/hermes-agent/issues/46257
[yt-review]: https://www.youtube.com/watch?v=3lXBYgXM090
[vllm-omni]: https://docs.vllm.ai/projects/vllm-omni/en/latest/user_guide/examples/online_serving/mimo_audio
[llama-gguf]: https://huggingface.co/AesSedai/MiMo-V2.5-GGUF
[llama-issue]: https://github.com/ggml-org/llama.cpp/issues/22469
