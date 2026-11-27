# 配置指南

fcitx5-voice-input 支持通过图形化工具（`fcitx5-configtool`）或直接编辑配置文件进行参数调优。

各 ASR 后端的详细参数与配置示例：

- [OpenAI 兼容后端](/guide/providers/openai)——OpenAI、Groq、硅基流动、阿里云百炼、小米 MiMo
- [火山引擎豆包后端](/guide/providers/volcengine)
- [Mistral Realtime 后端](/guide/providers/mistral)

---

## 配置文件路径

所有配置均保存在当前用户的 Fcitx5 配置目录下：

| 配置文件 | 说明 |
|---|---|
| `~/.config/fcitx5/conf/voiceinput.conf` | 主插件配置（后端选择、VAD 灵敏度、静音阈值等） |
| `~/.config/fcitx5/conf/voiceinput-openai.conf` | OpenAI 兼容后端子配置（包含 API Key、模型、LLM 后处理等） |
| `~/.config/fcitx5/conf/voiceinput-volcengine.conf` | 火山引擎豆包 ASR 子配置（鉴权密钥、资源 ID、标点规则等） |
| `~/.config/fcitx5/conf/voiceinput-mistral.conf` | Mistral Realtime 子配置（WebSocket 实时转写参数） |
| `~/.config/fcitx5/conf/voiceinput-sherpa-onnx.conf` | 本地流式子配置（线程数、热词、自动上屏） |
| `~/.config/fcitx5/conf/voiceinput-sherpa-offline.conf` | 本地离线子配置（线程数、热词、自动上屏） |

> **安全提示**：API Key 与认证凭证以明文形式保存在上述子配置文件中，请确保 `~/.config/fcitx5/conf/` 目录权限安全。

---

## 主配置参数 (VoiceInput)

在 `fcitx5-configtool` 中点击 **VoiceInput** 右侧齿轮 ⚙，即可调节主配置参数：

| 参数项 | 说明 | 默认值 | 推荐调节建议 |
|---|---|---|---|
| `ActiveBackend` | 当前生效的 ASR 识别后端 | `openai` | 可选 `openai`（OpenAI 兼容）、`volcengine`（火山引擎）、`mistral`（Mistral Realtime）、`sherpa_onnx`（本地流式）、`sherpa_offline`（本地 X-ASR 离线）；本地模型见下文「本地模型管理」 |
| `VADThreshold` | VAD 语音检测阈值百分比（0-100） | `20` | 值越低越灵敏；如果环境杂音较大引起误识别，可调高至 `30`~`50` |
| `SilenceThresholdMs` | 判定说话结束的静音等待时长 (ms) | `800` | 语速较慢或思考停顿多时可调至 `1000`~`1500`；追求极速上屏可调至 `500`~`600` |
| `StartFrames` | 连续多少帧判定为有效人声开始 | `2` | 每帧 32ms。默认 2 帧（64ms），可过滤突发的短暂杂音（如键盘敲击声） |
| `PreRollMs` | 判定说话开始前向前抓取的音频长度 (ms) | `300` | 确保首字发音（如爆破音、轻声）不被截断，一般保持 `300` 即可 |
| `MinSpeechMs` | 最短有效语音时长 (ms) | `300` | 低于此长度的突发声响会被丢弃，避免咳嗽或清嗓子触发无意义识别 |
| `MaxSpeechMs` | 单次说话最长持续时长 (ms) | `30000` | 达到最长限制后将强制分段提交，防止长时间录音导致内存或请求过大 |

---

## LLM 智能后处理配置

在 [OpenAI 后端](/guide/providers/openai)中启用 LLM 后处理（`LLMEnabled=true`）后，ASR 识别结果在上屏前会先经过大语言模型进行纠错与润色。

需要配置：

| 参数项 | 说明 |
|---|---|
| `LLMModel` | Chat 兼容模型名，如 `gpt-4o-mini`、`qwen-plus` |
| `LLMSystemPrompt` | 后处理提示词，参考下方示例 |
| `LLMStream` | 是否流式输出（默认 `true`，上屏更快） |
| `AutoCommit` | 无 LLM 时自动上屏（默认 `true`） |

### 推荐 System Prompt 示例

```text
你是一个专业的输入法语音转写后处理助手。
请对输入的语音识别文本进行以下处理：
1. 修正同音错别字与口误，保持原意不变；
2. 规范中英文混排格式（中英文之间保持合适空格）；
3. 补充或纠正标点符号；
4. 仅输出最终处理后的文本，严禁包含任何解释、前缀或多余内容。
```

## 本地模型管理

在 `fcitx5-configtool` 中选择 Voice Input 的 Sherpa-ONNX 后端，点击该后端右侧的齿轮，再选择**管理本地模型**。流式后端提供中文 Zipformer（约 570 MiB），离线后端提供中英 X-ASR（约 130 MiB）。

可以直接下载，或导入对应的官方模型压缩包；两种方式都会校验压缩包和模型文件，并在独立进程中试加载。安装过程中可取消。**已安装**表示文件通过安装校验，**已就绪**表示当前选中的后端已完成加载，两者不同；安装未选中的后端不会切换当前后端。

模型安装在用户数据目录中，默认位于 `~/.local/share/fcitx5/voice-input/models/`。仅支持程序内置清单中的固定模型版本，不自动搜索旧目录或导入任意第三方模型。本地 ASR 不上传音频；如果另行启用云端 LLM 后处理，识别文本仍可能被发送至云端。
