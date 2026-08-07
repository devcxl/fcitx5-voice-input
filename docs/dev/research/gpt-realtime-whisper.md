# 调研：OpenAI gpt-realtime-whisper 模型与 Realtime API 接入评估

> 面向 fcitx5-voice-input（C++ Fcitx5 语音输入插件，当前 OpenAI 后端走 whisper-1 HTTP multipart 一次性转录）。
> 调研日期：2026-08-07。调研 agent：@researcher。
> 本文件仅含事实与来源标注，方案为决策参考。

---

## 0. 调研结论摘要（带置信度标注）

| # | 结论 | 置信度 | 依据 |
|---|------|--------|------|
| C1 | **模型名**：`gpt-realtime-whisper` 是真实存在、已发布的流式 STT 模型，**非 preview/beta**（OpenAI 直连与 Azure 均标 GA）。官方明确称其为 GA 之后的"继续支持"模型，**当前新接入推荐模型已换成 `gpt-live-transcribe`** | 高 | [模型页][gtrw]、[Transcription 总览][transcription]、[Azure][azure-ms] |
| C2 | **计费按音频时长**：`gpt-realtime-whisper` 按 $0.017/音频分钟 计费（非 token），**属实**；`gpt-live-transcribe` 同价 $0.017/min | 高 | [模型页][gtrw]、[TNW 报道][tnw]、[社区公告][community-announce] |
| C3 | **端点命名与用户问题里的不符**：当前文档无 `v1/realtime/transcriptions`，无 `v1/realtime/translation_sessions`。实际为：转录会话 `v1/realtime/transcription_sessions`，翻译会话 `v1/realtime/translations` | 高 | [gpt-realtime 模型页][gtr]、[翻译指南][rt-translation] |
| C4 | **传输协议是 WebSocket（服务端）或 WebRTC（浏览器）**，非纯 HTTP/SSE 流式。音频以 **base64 JSON 文本帧** 通过 `input_audio_buffer.append` 持续推送 | 高 | [Realtime WebSocket 指南][ws-guide]、[转录指南][rt-transcription] |
| C5 | **支持"边说边识别"增量**：事件 `conversation.item.input_audio_transcription.delta`（增量）+ `.completed`（最终）。**但 `gpt-realtime-whisper` 必须手动 commit（`turn_detection` 置空），不支持 server_vad**；其 delta 通常随 commit 批量到达，非逐词实时 | 中-高 | [VAD 指南][rt-vad]、[转录指南][rt-transcription]、[社区实测][community-delta] |
| C6 | **音频格式硬性约束**：Realtime API 要求 **pcm16、mono、24kHz（最低 24000）**，16kHz 会被拒绝（`Expected a value >= 24000`）。本项目现采集 16kHz，**需重采样 16k→24k** 才能接入 | 高 | [Azure 文档][azure-realtime]、[OpenWhispr PR][openwhispr] |
| C7 | **语言**：支持多语言，可传 ISO-639-1 hint（如 `zh`）；`gpt-realtime-whisper`/`gpt-live-transcribe` 不返回语种检测（检测需 `gpt-transcribe`） | 中 | [Azure][azure-ms]、[转录指南][rt-transcription] |
| C8 | **GA / 可用性**：OpenAI 直连所有开发者可用（需付费 Tier，Free 不支持）；Azure 仅限部分区域（Canada Central / France Central / India South，持续扩增） | 中 | [模型页][gtrw]、[Azure 目录][azure-catalog] |
| C9 | **C++ 集成无需新增 WS 库**：libcurl 自 **7.86.0 起内置 WebSocket**（`curl_ws_send`/`curl_ws_recv` + `CURLOPT_CONNECT_ONLY=2L`）。本项目已要求 curl>=7.86.0，**零新依赖可行** | 高 | [curl WS 文档][curl-ws]、[curl_ws_send][curl-ws-send] |

**一句话结论**：接入真实可行，且**无需新增 WebSocket 依赖**（复用 libcurl 7.86+ 的内置 WS 支持）；技术门槛集中在**16k→24k 重采样**与**手动 commit 的分段语义**，而非传输协议本身。若追求最佳实时增量体验，官方当前推荐的是 `gpt-live-transcribe` 而非 `gpt-realtime-whisper`。

---

## 1. 接入端点与协议

### 1.1 端点语义澄清（重要更正）

用户问题中的 `v1/realtime/transcriptions` 与 `v1/realtime/translation_sessions` **在当前文档中不存在**。官方文档各模型页（gpt-realtime / gpt-realtime-whisper / gpt-live-transcribe）统一列出三类实时端点为：

| 会话类型 | 端点 | 语义 | 来源 |
|---------|------|------|------|
| Voice-agent 会话 | `v1/realtime` | 语音对语音，模型作为助理，用 conversation + response 生命周期 | [Realtime 总览][rt-overview] |
| **翻译会话** | `v1/realtime/translations` | 模型作为口译，持续把流入音频翻译为另一种语言，输出音频 + transcript deltas，**不调用 response.create** | [翻译指南][rt-translation] |
| **转录会话** | `v1/realtime/transcription_sessions` | 需要无模型回复的实时 transcript delta | [Realtime 总览][rt-overview]、[转录指南][rt-transcription] |

> 注：`gpt-realtime-whisper` 使用转录会话（`v1/realtime/transcription_sessions`）。翻译会话用的是 `gpt-realtime-translate`，与"翻译成另一种语言"相关，与本项目"中文语音转文字"需求不符，应排除。

### 1.2 传输协议

- **服务端到服务端（本插件场景）→ WebSocket**。连接示例（官方 WebSocket 指南）：
  ```
  wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1
  ```
  对转录会话，URL 形式为 `wss://api.openai.com/v1/realtime?model=<transcription-model>`（`intent=transcription` 为社区旧写法，官方现用 session 内 `type: "transcription"`）。
- **浏览器/移动端 → WebRTC**（本插件为桌面 C++，不需要）。
- **认证**：标准 API key，`Authorization: Bearer <OPENAI_API_KEY>` 头；可选 `OpenAI-Safety-Identifier` 头（隐私标识）。服务端直连用标准 API key 即可（key 仅在服务端）。GA 接口无需 `OpenAI-Beta: realtime=v1` 头（beta 接口才需要）。
- **消息格式**：WebSocket 上客户端与服务器双向发送 **JSON 序列化的文本帧**。

> **无纯 HTTP/SSE 流式替代**。Realtime 会话是持久连接，官方明确区分"实时音频（用 Realtime 会话）"与"已完成文件的流式输出（`v1/audio/transcriptions` + `stream=true`）"。若不想引入持久连接，仍可退回现有 whisper-1 / gpt-transcribe 的 HTTP 方案，但拿不到持续增量。

---

## 2. 流式语义

### 2.1 音频发送方式

- 客户端把 **base64 编码的 PCM 音频块** 封装进 JSON，通过 `input_audio_buffer.append` 事件持续推送（"边说边发"）。
- 每块音频 ≤ **15 MB**（建议 100ms 左右粒度）。格式为 pcm16、mono、**24kHz**。
- 停止时用 `input_audio_buffer.commit`（手动）或由 VAD 自动 commit。

### 2.2 增量返回事件（server → client）

| 事件类型 | 含义 | 字段 |
|---------|------|------|
| `conversation.item.input_audio_transcription.delta` | 增量文本片段 | `item_id`, `content_index`, `delta` |
| `conversation.item.input_audio_transcription.completed` | 该 commit 项的最终转录 | `item_id`, `transcript` |
| `conversation.item.input_audio_transcription.failed` | 转录失败 | — |
| （VAD 开时）`input_audio_buffer.speech_started` / `speech_stopped` / `committed` | 语音起止与缓冲区提交 | — |

### 2.3 关键语义差异（对"真实时"体验影响重大）

- **`gpt-realtime-whisper`：必须手动 commit，不支持 server_vad**。官方 VAD 指南明确："`gpt-realtime-whisper` requires turn detection to be omitted or set to `null`"。
- 官方文档的 delta 示例展示逐词增量，但**社区实测（gpt-4o-transcribe 等）表明：转录在收到 commit 后才开始，delta 与 completed 往往同时批量到达**，并非严格的逐词实时。对不断句的长句（无停顿），需客户端周期性发 commit 才能拿到中间结果（会牺牲部分词准确率）。
- **官方当前推荐模型是 `gpt-live-transcribe`**（可调 `delay` 控制延迟/质量权衡，提供真正连续的 transcript deltas）；`gpt-realtime-whisper` 与 `gpt-4o-transcribe` 被列为"现有集成可继续使用，非新接入推荐"。
- `gpt-transcribe` 可在转录会话中用于"已 commit 的 turn"的转录并可返回语种检测——但那是 turn 之后，非边说边出。

> 对本项目：若核心诉求是"边说边显示增量"，`gpt-realtime-whisper` 需手动 commit 才能拿增量，且可能批量到达；`gpt-live-transcribe` 才是为此设计的模型。这直接影响选型结论。

### 2.4 语言指定

- 转录会话可在 `session.update` 的 `transcription` 对象里设 `language`（ISO-639-1，如 `zh`）作 hint，提高准确率。
- `gpt-realtime-whisper` / `gpt-live-transcribe` **不返回语种检测结果**；需"检测语言"只能用 `gpt-transcribe`（文件 / 已提交 turn）。
- 语言质量依音频质量/口音/领域词变化，官方建议用真实音频验证。

---

## 3. 模型规格

### 3.1 模型名与显式指定

- 必须在 WebSocket URL 或 session 配置中**显式指定模型名**（如 `gpt-realtime-whisper`），通过 `?model=` 参数或 `session.update` 的 `transcription.model`。
- 该模型无 snapshot 差异需求（模型页列出 `gpt-realtime-whisper` 别名与 snapshot，同 id）。
- **上下文窗口 16,000**。

### 3.2 计费

- `gpt-realtime-whisper`：**$0.017 / 音频分钟**（按音频时长，非 token）。**属实**，官方模型页标注 "Realtime audio duration $0.017"。
- 对照：`gpt-realtime-translate` $0.034/min；`gpt-realtime-2` 按 token（$32/1M audio input tokens、$64/1M audio output）。
- **Tier 要求（按分钟/分钟限流）**：
  | Tier | 每分钟音频分钟数 |
  |------|------------------|
  | Free | 不支持 |
  | Tier 1 | 100 |
  | Tier 2 | 350 |
  | Tier 3 | 650 |
  | Tier 4 | 1,000 |
  | Tier 5 | 1,300 |
  即**免费账号不可用，需付费并达一定 Tier**。

### 3.3 与 whisper-1 对比

| 维度 | whisper-1（现状，HTTP multipart） | gpt-realtime-whisper / gpt-live-transcribe（Realtime WS） |
|------|-------------------------------|-----------------------------------------------------------|
| 传输 | POST `/v1/audio/transcriptions`，一次性 | WebSocket 持久会话，持续推流 |
| 延迟 | 秒级（整段上传完才出结果） | 300–800ms 首 token（官方估算） |
| 增量 | 无（一次性最终文本） | delta + completed |
| 计费 | 按文本 token（$0.006/min 级，量级约 $\sim$0.006/min 以下） | **$0.017/音频分钟**（更贵） |
| 语言 | 支持多语言 + 可选 language 参数 | 支持多语言 + language hint；不返回语种检测 |
| 上下文 | 单次请求 | 会话内可携带此前 turn 上下文 |
| 格式 | WAV/MP3/MP4/FLAC/Ogg 等 | 仅原始 PCM（pcm16, mono, 24kHz） |
| 会话限制 | 无严格时长 | 官方称 Realtime 会话约 ≤30 分钟，需重连拼接 |

> 价格上 whisper-1 更便宜（按 token 且更低价）；Realtime 转录贵约数倍但换延迟与增量。对本项目"语音输入法"场景，延迟与增量价值 > 差价。

---

## 4. 可用性

- **GA**：`gpt-realtime-whisper` 于 **2026-05-07** 随 GPT-Realtime-2 等新模型发布（OpenAI 官方公告），OpenAI 直连与 Azure 均标 GA / Generally Available。
- **OpenAI 直连**：所有开发者可用，但**需付费 Tier**（Free 不支持）。
- **Azure 区域**：仅 Canada Central、France Central、India South（"More coming soon"）——**区域受限**。
- **社区/C++ 客户端参考**：
  - Python: `KhalidAbdelaty/gpt-realtime-api`（`test1_transcription.py` 演示 gpt-realtime-whisper 麦克风转录，24kHz pcm16，手动 commit）。[仓库][khalid]
  - TypeScript/LiveKit: `livekit/agents-js` `plugins/openai/src/stt.ts`（默认 `REALTIME_SAMPLE_RATE=24000`，默认模型 `gpt-realtime-whisper`，处理 delta/completed）。[仓库][livekit]
  - C++ 直接对接 OpenAI Realtime 的成熟开源实现较少（多为本地 whisper.cpp 自建服务，如 `Jota-project/jota-transcriber`，非 OpenAI 官方 API）。**C++ 官方/社区参考有限，需自研 WS 客户端（libcurl）**。

---

## 5. C++ 集成要点

### 5.1 WebSocket 库选择（关键结论：可零新依赖）

项目现有依赖：libcurl（已要求 >= 7.86.0）、jsoncpp、无 WS 库。

**libcurl 自 7.86.0 起原生支持 WebSocket 客户端**，且本项目最低要求恰好是 7.86.0：
- `CURLOPT_CONNECT_ONLY = 2L` → `curl_easy_perform()` 完成 HTTP `Upgrade` 后返回，应用接管连接。
- `curl_ws_send()` / `curl_ws_recv()` 收发 WebSocket 帧；`curl_ws_meta()` 取帧元数据（`CURLWS_TEXT`/`CURLWS_BINARY`/`CURLWS_PING`/`CURLWS_PONG`）。
- libcurl 自动回 PING 的 PONG（可用 `CURLOPT_WS_OPTIONS=CURLWS_NOAUTOPONG` 关闭，8.14.0+）。
- 8.16.0+ 支持通过 `CURLOPT_READFUNCTION` 发送帧（本项目按现有异步/线程模型更适用 `curl_ws_send` 显式发送）。

> 因此**不必引入 websocketpp / uWebSockets 等新依赖**，复用 curl 即可。需确认发行版 curl 版本 ≥ 7.86（Ubuntu 24.04 自带 curl 8.x，满足；AUR 亦满足）。

### 5.2 是否有纯 HTTP（非 WS）流式方案？

**无**。Realtime 增量转录必须用 WebSocket/WebRTC 持久连接。若坚持纯 HTTP：
- 只能退回 `POST /v1/audio/transcriptions` + `stream=true`（对**已完成文件**的 SSE 流式，仍是一次性上传后流式输出，非边说边识别），可用现有 `gpt-transcribe`/`whisper-1`——即当前方案。**得不到持续增量**。
- 若目标是"减少延迟但接受一次性"，保持现状即可。

### 5.3 关键风险点

1. **采样率不匹配（最高优先）**：Realtime API 要求 **24kHz**，16kHz 会被拒（`Expected a value >= 24000`）。本项目 16kHz → **必须线性上采样到 24kHz**（如 OpenWhispr 的做法：16k 采集后 upsampling 到 24k，模型端按 24k 声明）。需在发送前重采样，带来额外 CPU 与复杂度。
2. **手动 commit 语义**：`gpt-realtime-whisper` 无 server_vad，需客户端周期性/语音停顿后 `input_audio_buffer.commit` 才能出结果；长句不中断则拿不到中间增量。需与现有 VAD（Silero）状态机衔接——现有 VAD 已产出 Speaking/Idle，可映射到 commit 时机，架构上自然契合。
3. **连接保持与断线重连**：需处理 WS 断线、30 分钟会话上限、重连拼接（分段结果拼接需靠 `item_id` 对齐，官方明确"不同 turn 的 completed 顺序不保证"）。
4. **鉴权/Beta 头**：GA 接口**不要**带 `OpenAI-Beta: realtime=v1`；带标准 Bearer key。
5. **Tier/限流**：需付费账号；关注每分钟音频分钟上限。
6. **延迟 vs 准确率**：低延迟=更早的部分文本但词错率可能更高；官方建议用真实音频调优。
7. **价格**：$0.017/min 比 whisper-1 高；连续空闲会话也会计费，需在语音段结束后及时关闭会话节省成本。

---

## 6. 集成方案全景（供决策）

| 方案 | 描述 | 增量体验 | 新依赖 | 成本/风险 |
|------|------|---------|--------|-----------|
| **A. 现状（whisper-1 HTTP）** | 保持现有多部分上传 | 无（一次性） | 无 | 最省事、最便宜；延迟秒级 |
| **B. 用 libcurl WS + `gpt-realtime-whisper`** | 复用 curl 内置 WS，手动 commit | 中（commit 时出增量，可能批量） | **无**（仅需 16k→24k 重采样） | 需重采样 + 手动 commit；官方非推荐新模型 |
| **C. 用 libcurl WS + `gpt-live-transcribe`** | 官方推荐的实时转录模型 | 好（真正连续 delta，可调 delay） | **无**（需重采样） | 官方推荐，体验最佳；价格同 $0.017/min |
| **D. 引入第三方 WS 库（websocketpp 等）+ gpt-live-transcribe** | 更成熟的 WS 抽象 | 好 | 1 个新依赖 | 依赖增多；C++ 维护成本高 |

**建议**：优先 **方案 C（libcurl 内置 WS + gpt-live-transcribe）**——零新依赖、官方推荐、增量体验最佳；`gpt-realtime-whisper`（方案 B）作为"现有支持模型"可作备选，但非新接入最佳起点。关键前置工作是 16k→24k 重采样。若团队不接受任何 WS 复杂度，则维持方案 A。

**最坏情况**：方案 B/C 需在 C++ 中新增 WS 客户端线程 + 重采样 + 重连逻辑，工作量集中；若 OpenAI Realtime 端在目标发行版 curl 上不可用（curl < 7.86 极罕见），可降级回方案 A（现有 HTTP 路径不动）。改动可回滚（新增引擎，不影响现有 whisper 路径）。

---

## 7. 元评审（剩余不确定与最弱证据）

- **最弱证据**：C5（delta 批量到达）主要来自 gpt-4o-transcribe 的社区实测与文档表述，对 `gpt-realtime-whisper`/`gpt-live-transcribe` 的逐词实时程度未见独立基准数据，置信度定为"中-高"。**建议以最小可运行原型实测确认增量粒度**。
- **24kHz 硬约束**：来自 Azure 官方文档 + 社区 PR 的报错文本，多源一致，置信度高。
- 端点命名（`transcription_sessions` vs 用户所提 `transcriptions`）为文档直接引用，置信度高；若 OpenAI 存在向后兼容别名（旧 beta 接口）未覆盖。
- 语言支持的具体语言清单未获官方完整枚举（官方仅给 ISO-639-1 hint 与"多语言"），置信度为中。
- 本调研依赖 websearch 抓取的官方文档摘要（exa fetch 因 key 失效不可用）；核心事实均有文档 URL，可按需复核原文。

---

## 8. 参考链接（一手来源优先）

**OpenAI 官方（一手）**
- [GPT-Realtime-Whisper 模型页][gtrw] — 计费 $0.017/min、Tier 表、上下文 16000、端点
- [Realtime and audio 总览][rt-overview] — 三类会话端点对照
- [Realtime transcription 指南][rt-transcription] — 会话配置、delta/completed 事件、语言、gpt-transcribe
- [Realtime WebSocket 指南][ws-guide] — 连接、Bearer 认证、JSON 事件
- [Realtime translation 指南][rt-translation] — 翻译会话端点、架构差异
- [Realtime VAD 指南][rt-vad] — gpt-realtime-whisper 需 turn_detection=null
- [Transcription 总览][transcription] — 各工作流推荐模型、whisper-1 定位
- [GPT-Realtime 模型页][gtr] — GA 状态、端点
- [Realtime 成本指南][rt-costs] — 计费方式
- OpenAI 发布公告：[Advancing voice intelligence][openai-announce]、[Introducing gpt-realtime][openai-gtr]、[Realtime API developer notes][openai-blog]
- [OpenAI Cookbook: speech_transcription_methods][cookbook-methods] — HTTP vs WS 对比表、24kHz
- [OpenAI Cookbook: realtime_out_of_band_transcription][cookbook-oob] — Realtime 事件处理、价格常量

**Azure（一手补充）**
- [Azure GPT Realtime Whisper 概述][azure-ms] — 语言 hint、GA、区域
- [Azure Realtime audio 指南][azure-realtime] — 24kHz 约束、区域、模型版本（gpt-realtime-whisper 2026-05-06）
- [Azure Model Catalog: gpt-realtime-whisper][azure-catalog] — GA、区域

**行业/社区（二手，辅助）**
- [TNW: OpenAI launches GPT-Realtime-2...][tnw] — $0.017/min 价格
- [OpenAI Community: New Realtime Voice Models][community-announce] — 三模型价格
- [OpenAI Community: gpt-4o-transcribe delta 批量到达][community-delta] — 反方证据（增量非逐词实测）
- [KhalidAbdelaty/gpt-realtime-api][khalid] — Python 实现参考（24kHz、手动 commit）
- [livekit/agents-js stt.ts][livekit] — Realtime 转录事件处理、24000 采样率
- [OpenWhispr PR #1044][openwhispr] — 16k/24k 采样率被拒实证
- [TheRouter: 转录四模型路由][therouter] — gpt-live-transcribe 端点归属

**C++ 依赖**
- [libcurl WebSocket 概览][curl-ws] — 7.86 起原生 WS
- [curl_ws_send][curl-ws-send] — 收发 API、示例
- [CURLOPT_WS_OPTIONS][curl-ws-opt]

[gtrw]: https://developers.openai.com/api/docs/models/gpt-realtime-whisper
[rt-overview]: https://developers.openai.com/api/docs/guides/realtime
[rt-transcription]: https://developers.openai.com/api/docs/guides/realtime-transcription
[ws-guide]: https://developers.openai.com/api/docs/guides/realtime-websocket
[rt-translation]: https://developers.openai.com/api/docs/guides/realtime-translation
[rt-vad]: https://developers.openai.com/api/docs/guides/realtime-vad
[transcription]: https://developers.openai.com/api/docs/guides/transcription
[gtr]: https://developers.openai.com/api/docs/models/gpt-realtime
[rt-costs]: https://developers.openai.com/api/docs/guides/realtime-costs
[openai-announce]: https://openai.com/index/advancing-voice-intelligence-with-new-models-in-the-api/
[openai-gtr]: https://openai.com/index/introducing-gpt-realtime/
[openai-blog]: https://developers.openai.com/blog/realtime-api
[cookbook-methods]: https://developers.openai.com/cookbook/examples/speech_transcription_methods
[cookbook-oob]: https://developers.openai.com/cookbook/examples/realtime_out_of_band_transcription
[azure-ms]: https://learn.microsoft.com/en-us/azure/foundry/openai/concepts/gpt-realtime-whisper
[azure-realtime]: https://learn.microsoft.com/en-us/azure/ai-foundry/openai/how-to/realtime-audio
[azure-catalog]: https://ai.azure.com/catalog/models/gpt-realtime-whisper
[tnw]: https://thenextweb.com/news/openai-gpt-realtime-2-voice-models
[community-announce]: https://community.openai.com/t/new-realtime-voice-models-in-the-api/1380471
[community-delta]: https://community.openai.com/t/gpt-4o-transcribe-realtime-the-delta-updates-not-received-during-the-transcription/1357039/1
[khalid]: https://github.com/KhalidAbdelaty/gpt-realtime-api
[livekit]: https://github.com/livekit/agents-js/blob/main/plugins/openai/src/stt.ts
[openwhispr]: https://github.com/OpenWhispr/openwhispr/pull/1044
[therouter]: https://therouter.ai/news/gpt-transcribe-live-transcribe-api-routing-decision/
[curl-ws]: https://curl.se/libcurl/c/libcurl-ws.html
[curl-ws-send]: https://curl.se/libcurl/c/curl_ws_send.html
[curl-ws-opt]: https://curl.se/libcurl/c/CURLOPT_WS_OPTIONS.html
