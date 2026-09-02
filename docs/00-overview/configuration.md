# 配置指南

fcitx5-voice-input 支持通过图形化工具（`fcitx5-configtool`）或直接编辑配置文件进行参数调优。

---

## 配置文件路径

所有配置均保存在当前用户的 Fcitx5 配置目录下：

| 配置文件 | 说明 |
|---|---|
| `~/.config/fcitx5/conf/voiceinput.conf` | 主插件配置（后端选择、VAD 灵敏度、静音阈值等） |
| `~/.config/fcitx5/conf/voiceinput-openai.conf` | OpenAI 兼容后端子配置（包含 API Key、模型、LLM 后处理等） |
| `~/.config/fcitx5/conf/voiceinput-volcengine.conf` | 火山引擎豆包 ASR 子配置（鉴权密钥、资源 ID、标点规则等） |
| `~/.config/fcitx5/conf/voiceinput-mistral.conf` | Mistral Realtime 子配置（WebSocket 实时转写参数） |

> **安全提示**：API Key 与认证凭证以明文形式保存在上述子配置文件中，请确保 `~/.config/fcitx5/conf/` 目录权限安全。

---

## 主配置参数 (VoiceInput)

在 `fcitx5-configtool` 中点击 **VoiceInput** 右侧齿轮 ⚙，即可调节主配置参数：

| 参数项 | 说明 | 默认值 | 推荐调节建议 |
|---|---|---|---|
| `ActiveBackend` | 当前生效的 ASR 识别后端 | `openai` | 可选 `openai`（OpenAI 兼容）、`volcengine`（火山引擎）、`mistral`（Mistral Realtime） |
| `VADThreshold` | VAD 语音检测阈值百分比（0-100） | `20` | 值越低越灵敏；如果环境杂音较大引起误识别，可调高至 `30`~`50` |
| `SilenceThresholdMs` | 判定说话结束的静音等待时长 (ms) | `800` | 语速较慢或思考停顿多时可调至 `1000`~`1500`；追求极速上屏可调至 `500`~`600` |
| `StartFrames` | 连续多少帧判定为有效人声开始 | `2` | 每帧 32ms。默认 2 帧（64ms），可过滤突发的短暂杂音（如键盘敲击声） |
| `PreRollMs` | 判定说话开始前向前抓取的音频长度 (ms) | `300` | 确保首字发音（如爆破音、轻声）不被截断，一般保持 `300` 即可 |
| `MinSpeechMs` | 最短有效语音时长 (ms) | `300` | 低于此长度的突发声响会被丢弃，避免咳嗽或清嗓子触发无意义识别 |
| `MaxSpeechMs` | 单次说话最长持续时长 (ms) | `30000` | 达到最长限制后将强制分段提交，防止长时间录音导致内存或请求过大 |

---

## OpenAI 兼容后端

适用于 OpenAI 官方接口、第三方兼容中转服务、Groq、硅基流动（SiliconFlow）、阿里云百炼 DashScope 等。

### 配置参数项

| 参数项 | 说明 | 默认值 | 备选项 / 描述 |
|---|---|---|---|
| `BaseUrl` | API 基础地址 | `https://api.openai.com/v1` | 兼容服务或本地代理端点 URL |
| `ApiKey` | API 密钥 | (空，必填) | 平台获取的鉴权 Token |
| `Model` | ASR 语音模型名称 | `whisper-1` | 如 `whisper-1`、`qwen3-asr-flash`、`gpt-live-transcribe` |
| `Language` | 输出语言 | `auto` | 可选 `auto`（自动检测）、`zh`（强制中文）、`en`（强制英文） |
| `ApiMode` | API 调用模式 | `whisper` | 详见下方 API 模式说明 |
| `CommitIntervalMs` | 实时流式周期性提交间隔 (ms) | `5000` | 仅在 `ApiMode=realtime` 下生效，超长句无停顿时持续分片提交 |
| `LLMEnabled` | 是否启用 LLM 后处理 | `false` | 开启后可对 ASR 识别出的文本进行错字修正与润色 |
| `LLMModel` | LLM 后处理模型名称 | (空) | 如 `gpt-4o-mini`、`qwen-plus` 等 Chat 兼容模型 |
| `LLMSystemPrompt` | LLM 提示词 | (空) | 引导模型进行纠错或排版的 Prompt |
| `LLMStream` | LLM 是否流式输出 | `true` | 流式输出可加速后处理文本上屏速度 |
| `AutoCommit` | 无 LLM 时自动上屏 | `true` | 禁用 LLM 时识别完成直接上屏 |

### API 模式 (`ApiMode`) 说明

1. **`whisper` (标准 Whisper API，默认)**：
   - 每次语音结束向 `/v1/audio/transcriptions` 发送 multipart/form-data 音频文件（WAV 格式）。
   - 适用于 OpenAI 官方 `whisper-1`、Groq `whisper-large-v3`、硅基流动 `FunAudioLLM/SenseVoiceSmall` 等。
2. **`chat` (Chat Completions 接口)**：
   - 适用于**阿里云百炼**（DashScope）等通过 Chat 接口提供语音识别的服务（如 `qwen3-asr-flash`）。
3. **`realtime` (OpenAI Realtime 流式转录)**：
   - 基于 WebSocket 双向流式协议（音频自动重采样至 24kHz 发送）。
   - 边说话边在 Preedit 显示增量转写，静音后立刻提交上屏。需要付费 Tier 账号支持。

### 服务商配置示例

::: code-group

```ini [OpenAI 官方 (标准模式)]
BaseUrl=https://api.openai.com/v1
ApiKey=sk-proj-xxxxxxxxxxxxxxxxxxxx
Model=whisper-1
ApiMode=whisper
Language=zh
```

```ini [OpenAI 官方 (Realtime 流式)]
BaseUrl=https://api.openai.com/v1
ApiKey=sk-proj-xxxxxxxxxxxxxxxxxxxx
Model=gpt-live-transcribe
ApiMode=realtime
Language=zh
```

```ini [阿里云百炼 (DashScope)]
BaseUrl=https://dashscope.aliyuncs.com/compatible-mode/v1
ApiKey=sk-xxxxxxxxxxxxxxxxxxxxxxxx
Model=qwen3-asr-flash
ApiMode=chat
Language=zh
```

```ini [硅基流动 (SiliconFlow)]
BaseUrl=https://api.siliconflow.cn/v1
ApiKey=sk-xxxxxxxxxxxxxxxxxxxxxxxx
Model=FunAudioLLM/SenseVoiceSmall
ApiMode=whisper
Language=zh
```

```ini [Groq (超快转写)]
BaseUrl=https://api.groq.com/openai/v1
ApiKey=gsk_xxxxxxxxxxxxxxxxxxxxxx
Model=whisper-large-v3
ApiMode=whisper
Language=zh
```

:::

---

## 火山引擎豆包后端 (Volcengine)

火山引擎流式语音识别具有极高的中文识别准确度与极低延迟，支持边说边出字与二次识别修正。

### 配置参数项

| 参数项 | 说明 | 默认值 | 描述 |
|---|---|---|---|
| `Endpoint` | WebSocket 服务端点 | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` | 豆包大模型流式端点 |
| `AuthMode` | 认证模式 | `api_key` | 可选 `api_key`（新控制台单 Key 认证）或 `app_access_key`（旧控制台双 Key 认证） |
| `ApiKey` | API Key | (空) | `api_key` 模式下的鉴权密钥 |
| `AppKey` | App Key | (空) | `app_access_key` 模式下的应用 AppKey |
| `AccessKey` | Access Key / Token | (空) | `app_access_key` 模式下的访问 Token |
| `ResourceId` | 资源 ID (Resource ID) | `volc.seedasr.sauc.duration` | 在控制台购买的服务资源包 ID |
| `ChunkMs` | 单包音频推送切片大小 (ms) | `200` | 音频分片粒度（100~200ms） |
| `EnableITN` | 逆文本标准化 (ITN) | `true` | 将“一百二十三”自动转为“123” |
| `EnablePunc` | 标点符号自动添加 | `true` | 自动预测并添加逗号、句号等标点 |
| `EnableDDC` | 语义顺滑 (DDC) | `false` | 过滤口语中的“呃、啊、那个”等无意义语气词 |
| `EnableNonstream` | 开启二次识别修正 | `true` | 句子结束时执行全局二次重评分，提升整句最终准确率 |
| `EndWindowMs` | 服务端判停窗口时长 (ms) | `800` | 服务端静音截断判定时长 |

### 资源 ID (ResourceId) 对照表

在[火山引擎控制台](https://console.volcengine.com/)开通服务时，根据购买类型填写对应的资源 ID：

- **豆包语音大模型 2.0 (按调用量/小时版)**：`volc.seedasr.sauc.duration`
- **豆包语音大模型 2.0 (按并发版)**：`volc.seedasr.sauc.concurrent`
- **语音大模型 1.0 (按调用量/小时版)**：`volc.bigasr.sauc.duration`
- **语音大模型 1.0 (按并发版)**：`volc.bigasr.sauc.concurrent`

---

## Mistral Realtime 后端

Mistral Realtime 转录提供高质量多语言实时流式语音转写（基于 WebSocket 连接，原生 16kHz PCM 音频推流）。

| 参数项 | 说明 | 默认值 |
|---|---|---|
| `BaseUrl` | 接口基础地址 | `https://api.mistral.ai/v1` |
| `ApiKey` | Mistral API 密钥 | (空，必填) |
| `Model` | 实时语音转录模型 | `voxtral-mini-transcribe-realtime-2602` |
| `CommitIntervalMs` | 实时提交间隔 (ms) | `5000` |
| `TargetStreamingDelayMs` | 目标流式延迟 (ms, 0-1000) | `0`（数值越小延迟越低） |

---

## LLM 智能后处理配置

在 `OpenAI` 后端中启用 LLM 后处理后，ASR 识别结果在上屏前会先经过大语言模型进行纠错与润色。

### 推荐 System Prompt 示例

```text
你是一个专业的输入法语音转写后处理助手。
请对输入的语音识别文本进行以下处理：
1. 修正同音错别字与口误，保持原意不变；
2. 规范中英文混排格式（中英文之间保持合适空格）；
3. 补充或纠正标点符号；
4. 仅输出最终处理后的文本，严禁包含任何解释、前缀或多余内容。
```
