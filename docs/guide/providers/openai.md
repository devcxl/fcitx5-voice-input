# OpenAI 兼容后端

适用于 OpenAI 官方接口、第三方兼容中转服务、Groq、硅基流动（SiliconFlow）、阿里云百炼 DashScope、小米 MiMo 等。配置保存在 `~/.config/fcitx5/conf/voiceinput-openai.conf`。

选择此后端：主配置中 `ActiveBackend=openai`。后端选择与其他全局参数见[配置总览](/guide/configuration)。

## 配置参数项

| 参数项 | 说明 | 默认值 | 备选项 / 描述 |
|---|---|---|---|
| `BaseUrl` | API 基础地址 | `https://api.openai.com/v1` | 兼容服务或本地代理端点 URL |
| `ApiKey` | API 密钥 | (空，必填) | 平台获取的鉴权 Token |
| `Model` | ASR 语音模型名称 | `whisper-1` | 如 `whisper-1`、`qwen3-asr-flash`、`mimo-v2.5-asr`、`gpt-live-transcribe` |
| `Language` | 输出语言 | `auto` | 可选 `auto`（自动检测）、`zh`（强制中文）、`en`（强制英文） |
| `ApiMode` | API 调用模式 | `whisper` | 详见下方 API 模式说明 |
| `EnableItn` | chat 模式是否发送 DashScope 扩展字段 `asr_options.enable_itn` | `true` | 小米 MiMo 的 `asr_options` 仅定义 `language`，建议关闭 |
| `CommitIntervalMs` | 实时流式周期性提交间隔 (ms) | `5000` | 仅在 `ApiMode=realtime` 下生效，超长句无停顿时持续分片提交 |
| `LLMEnabled` | 是否启用 LLM 后处理 | `false` | 开启后可对 ASR 识别出的文本进行错字修正与润色，见[LLM 后处理](/guide/configuration#llm-智能后处理配置) |
| `LLMModel` | LLM 后处理模型名称 | (空) | 如 `gpt-4o-mini`、`qwen-plus` 等 Chat 兼容模型 |
| `LLMSystemPrompt` | LLM 提示词 | (空) | 引导模型进行纠错或排版的 Prompt |
| `LLMStream` | LLM 是否流式输出 | `true` | 流式输出可加速后处理文本上屏速度 |
| `AutoCommit` | 无 LLM 时自动上屏 | `true` | 禁用 LLM 时识别完成直接上屏 |

## API 模式 (`ApiMode`) 说明

1. **`whisper` (标准 Whisper API，默认)**：
   - 每次语音结束向 `/v1/audio/transcriptions` 发送 multipart/form-data 音频文件（WAV 格式）。
   - 适用于 OpenAI 官方 `whisper-1`、Groq `whisper-large-v3`、硅基流动 `FunAudioLLM/SenseVoiceSmall` 等。
2. **`chat` (Chat Completions 接口)**：
   - 适用于**阿里云百炼**（DashScope）、**小米 MiMo** 等通过 Chat 接口提供语音识别的服务（如 `qwen3-asr-flash`、`mimo-v2.5-asr`）；整段音频提交后一次性返回，非实时流式。
3. **`realtime` (OpenAI Realtime 流式转录)**：
   - 基于 WebSocket 双向流式协议（音频自动重采样至 24kHz 发送）。
   - 边说话边在 Preedit 显示增量转写，静音后立刻提交上屏。需要付费 Tier 账号支持。

## 服务商配置示例

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

```ini [小米 MiMo]
BaseUrl=https://api.xiaomimimo.com/v1
ApiKey=sk-xxxxxxxxxxxxxxxxxxxxxxxx
Model=mimo-v2.5-asr
ApiMode=chat
Language=zh
EnableItn=false
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
