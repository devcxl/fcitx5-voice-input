# Mistral Realtime 后端

Mistral Realtime 转录提供高质量多语言实时流式语音转写（基于 WebSocket 连接，原生 16kHz PCM 音频推流）。配置保存在 `~/.config/fcitx5/conf/voiceinput-mistral.conf`。

选择此后端：主配置中 `ActiveBackend=mistral`。后端选择与其他全局参数见[配置总览](/guide/configuration)。

## 配置参数项

| 参数项 | 说明 | 默认值 |
|---|---|---|
| `BaseUrl` | 接口基础地址 | `https://api.mistral.ai/v1` |
| `ApiKey` | Mistral API 密钥 | (空，必填) |
| `Model` | 实时语音转录模型 | `voxtral-mini-transcribe-realtime-2602` |
| `CommitIntervalMs` | 实时提交间隔 (ms) | `5000` |
| `TargetStreamingDelayMs` | 目标流式延迟 (ms, 0-1000) | `0`（数值越小延迟越低） |

## 运行排查

使用流式后端遇到问题时，见[流式后端运行排查](/guide/ws-troubleshooting)。
