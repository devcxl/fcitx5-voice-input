<div align="center">

<p align="center">
  <img width="1774" alt="Banner" src="https://github.com/user-attachments/assets/d3fe33eb-6c91-4000-925b-99eb83a498a7" />
</p>

# fcitx5-voice-input

<p>
  <a href="https://github.com/devcxl/fcitx5-voice-input/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/devcxl/fcitx5-voice-input/ci.yml?branch=main&logo=github&label=build" alt="Build"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-LGPL%20v3-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-Linux-important" alt="Platform">
  <img src="https://img.shields.io/badge/fcitx5-%3E%3D5.1.19-blueviolet" alt="Fcitx5">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
</p>

[English](README.md) | **中文**

---

</div>

**fcitx5-voice-input** 是一个 Fcitx5 语音输入插件。通过 PulseAudio（或 PipeWire fallback）捕获音频，使用 Silero ONNX VAD 检测人声分段，通过 OpenAI 兼容 API、火山引擎豆包流式语音、Mistral Realtime 流式实时转录，或完全本地的 sherpa-onnx 后端（流式 / X-ASR 离线）进行识别。

## 功能

- 中文语音输入（OpenAI Whisper API / 兼容服务、火山引擎豆包流式语音、Mistral Realtime 流式实时转录，或本地 sherpa-onnx / X-ASR 离线识别）
- Silero ONNX VAD 自动分段录音（无需手动按键控制开始结束）
- 说话过程中实时显示识别中间结果（火山引擎 / Mistral Realtime / OpenAI Realtime / 本地 sherpa-onnx 流式后端支持）
- 完全本地离线识别：选择 sherpa-onnx 后端时语音不出本机，无网环境也可用
- 队列管道架构：音频采集 → VAD 分段 → ASR 识别 → EventDispatcher 上屏
- 通过 `fcitx5-configtool` 图形化配置
- 切窗即暂停识别、保留录音流：失焦立即停止识别并丢弃新采样，回焦复用同一条流继续输入，不会重启流水线

<p align="center">
  <img width="720" alt="演示" src="https://github.com/user-attachments/assets/48164962-deba-4328-bf26-70cd258f86a6" />
</p>

## 使用

### 1. 安装

#### [AUR (Arch Linux)](https://aur.archlinux.org/packages/fcitx5-voice-input)

AUR 包在仓库外部单独维护：

```bash
yay -S fcitx5-voice-input
# 或
paru -S fcitx5-voice-input
# 或手动构建
git clone https://aur.archlinux.org/fcitx5-voice-input.git
cd fcitx5-voice-input
makepkg -si
```

#### 手动编译安装

见下方 [编译构建](#编译构建)。

### 2. 配置

安装后，打开 `fcitx5-configtool`，在 Input Method 列表中找到 **Voice Input** 并添加到输入法列表。

然后在 Addon 配置中找到 **VoiceInput**：

#### 主配置

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `ActiveBackend` | ASR 后端 | `openai` |
| `VADThreshold` | VAD 灵敏度 (0-100)，越高越不易触发 | `20` |
| `SilenceThresholdMs` | 静音多久结束说话 (ms) | `800` |
| `StartFrames` | 连续多少帧判定说话开始 | `2` |
| `PreRollMs` | 说话开始前预取音频 (ms) | `300` |
| `MinSpeechMs` | 最短有效语音段 (ms) | `300` |
| `MaxSpeechMs` | 最长语音段后强制分段 (ms) | `30000` |

在下拉框选择后端后，点击齿轮按钮 ⚙ 打开对应后端的配置页。

#### OpenAI 后端（子配置）

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `BaseUrl` | API 地址 | `https://api.openai.com/v1` |
| `ApiKey` | API Key | **（必填）** |
| `Model` | 模型名 | `whisper-1` |
| `Language` | 输出语言 | `auto`（English/中文） |
| `ApiMode` | API 模式：`whisper`（标准 Whisper API）、`chat`（OpenAI 兼容 Chat Completions，如百炼/小米 MiMo）或 `realtime`（GPT Realtime 流式实时转录） | `whisper` |
| `EnableItn` | chat 模式下是否发送 DashScope 扩展字段 `asr_options.enable_itn`；小米 MiMo 建议关闭（其接口仅定义 `language`） | `true` |
| `CommitIntervalMs` | Realtime 模式下的周期性提交间隔 (ms)，长句无停顿也能持续出增量 | `5000` |
| `LLMEnabled` | LLM 后处理 | `false` |
| `LLMModel` | 后处理 LLM 模型 | （空） |
| `LLMSystemPrompt` | 后处理系统提示词 | （空） |
| `LLMStream` | LLM 流式输出 | `true` |
| `AutoCommit` | 无 LLM 时自动上屏 | `true` |

设置 `ActiveBackend=openai`，点击齿轮按钮，然后填入您的 API Key。支持所有 OpenAI 兼容服务，如：

- [OpenAI](https://platform.openai.com/) — `https://api.openai.com/v1`
- [Groq](https://console.groq.com/) — `https://api.groq.com/openai/v1`
- [硅基流动 (SiliconFlow)](https://siliconflow.cn/) — `https://api.siliconflow.cn/v1`
- [小米 MiMo](https://mimo.mi.com/docs/zh-CN/api/audio/Speech-Recognition) — `https://api.xiaomimimo.com/v1`
- [阿里云百炼 (DashScope)](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference) — `https://dashscope.aliyuncs.com/compatible-mode/v1`

  **Chat 模式（OpenAI 兼容 Chat Completions）：** 百炼 `qwen3-asr-flash` 与小米 MiMo `mimo-v2.5-asr` 不走标准 Whisper API，需将 `ApiMode` 设为 `chat`；两者均为整段音频提交的分段转录，不做实时流式识别。MiMo 的 `asr_options` 仅定义 `language`，建议关闭 `EnableItn`。
  ```
  # 阿里云百炼
  BaseUrl=https://dashscope.aliyuncs.com/compatible-mode/v1
  ApiKey=your_dashscope_api_key
  Model=qwen3-asr-flash
  ApiMode=chat
  Language=zh
  ```
  ```
  # 小米 MiMo
  BaseUrl=https://api.xiaomimimo.com/v1
  ApiKey=your_mimo_api_key
  Model=mimo-v2.5-asr
  ApiMode=chat
  Language=zh
  EnableItn=false
  ```
  接口文档：[阿里云百炼](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)、[小米 MiMo](https://mimo.mi.com/docs/zh-CN/api/audio/Speech-Recognition)。

  **Realtime 流式实时转录（可选）：** 将 `ApiMode` 设为 `realtime`，即可通过 OpenAI Realtime 转录会话实现**边说边出**增量识别结果（实时刷入候选框 preedit，说话结束提交上屏）。使用 OpenAI 官方账号时，模型推荐 `gpt-live-transcribe`（官方推荐，真正连续增量）或 `gpt-realtime-whisper`（兼容备选）。配置示例：
  ```
  BaseUrl=https://api.openai.com/v1
  ApiKey=your_openai_api_key
  Model=gpt-live-transcribe
  ApiMode=realtime
  Language=zh
  ```
  **注意：** Realtime 模式需要支持 WebSocket 的连接端点，且 `gpt-live-transcribe` / `gpt-realtime-whisper` 需付费 Tier 账号（Free 不支持）。音频以 24kHz 发送（插件会自动将采集的 16kHz 重采样到 24kHz）。

#### 火山引擎豆包后端（子配置）

设置 `ActiveBackend=volcengine`，点击齿轮按钮打开火山引擎配置页。

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `Endpoint` | WebSocket 地址 | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` |
| `AuthMode` | 认证模式：`api_key` 或 `app_access_key` | `api_key` |
| `ApiKey` | API Key（新版控制台） | **api_key 模式必填** |
| `AppKey` | App Key（旧版控制台） | **app_access_key 模式必填** |
| `AccessKey` | Access Key（旧版控制台） | **app_access_key 模式必填** |
| `ResourceId` | 资源 ID | `volc.seedasr.sauc.duration` |
| `ChunkMs` | 单包音频长度 (ms) | `200` |
| `EnableITN` | 启用 ITN 文本规范化 | `true` |
| `EnablePunc` | 启用标点符号 | `true` |
| `EnableDDC` | 启用语义顺滑 | `false` |
| `EnableNonstream` | 启用二次识别 | `true` |
| `EndWindowMs` | 服务端判停窗口 (ms) | `800` |

火山引擎需要先在[火山引擎控制台](https://console.volcengine.com/)购买语音识别资源。资源 ID 取决于模型和购买方式：

- 模型 2.0 小时版：`volc.seedasr.sauc.duration`
- 模型 2.0 并发版：`volc.seedasr.sauc.concurrent`
- 模型 1.0 小时版：`volc.bigasr.sauc.duration`
- 模型 1.0 并发版：`volc.bigasr.sauc.concurrent`

**故障排查：** 如果识别失败，在插件日志中查找 `X-Tt-Logid`，并提交给火山引擎技术支持。

#### Mistral Realtime 后端（子配置）

设置 `ActiveBackend=mistral`，点击齿轮按钮打开 Mistral 配置页。

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `BaseUrl` | API 地址 | `https://api.mistral.ai/v1` |
| `ApiKey` | API Key | **（必填）** |
| `Model` | 模型名 | `voxtral-mini-transcribe-realtime-2602` |
| `CommitIntervalMs` | 周期性提交间隔 (ms)，长句无停顿也能持续出增量 | `5000` |
| `TargetStreamingDelayMs` | 目标流式延迟 (ms, 0-1000)：数值越大等待的上下文越多，文本更晚但更准确；越小出字更快但更不准确 | `0` |

该后端通过 WebSocket 使用 [Mistral Realtime 转录 API](https://docs.mistral.ai/capabilities/transcription/)。音频以 16kHz PCM 直推（无需重采样）。`transcription.text.delta` 增量事件实时刷入 preedit，`transcription.done` 在说话结束时提交最终结果。断线自动重连并保持同一会话。

### 3. 使用

1. 切换到 **Voice Input** 输入法
2. 开始说话，VAD 自动检测人声并录音
3. 使用**火山引擎 / Mistral Realtime / OpenAI Realtime** 后端时，说话过程中会实时显示识别中间结果
4. 停止说话（默认 800ms 静音超时），最终识别结果自动上屏
5. 保持语音输入模式，继续说话可连续识别

切换窗口时插件会立即暂停识别：丢弃新采样并取消在途会话，但录音流保持打开。切回 Voice Input 后复用同一条流继续识别；只有显式切换到其他输入法、更改输入法组、关闭语音输入或退出 Fcitx5 时才释放录音流。

## 编译构建

### 依赖

- `fcitx5` — 输入法框架
- `libpulse-simple` — PulseAudio 音频捕获（优先）
- `libpipewire-0.3` — PipeWire 音频捕获（fallback）
- `jsoncpp` — JSON 解析
- `libcurl` — HTTP/WebSocket 客户端（>= 7.86.0，ASR 必需）
- `zlib` — Gzip 压缩（火山引擎后端必需）
- `onnxruntime` — Silero VAD ONNX Runtime

> **Arch Linux:** `sudo pacman -S fcitx5 pulseaudio pipewire jsoncpp curl onnxruntime-cpu zlib`
>
> **Debian/Ubuntu:** `sudo apt install fcitx5 libpulse-dev libpipewire-0.3-dev libjsoncpp-dev libcurl4-openssl-dev libonnxruntime-dev zlib1g-dev`

### 编译

```bash
# 克隆并初始化子模块（获取 Silero VAD 模型）
git clone https://github.com/devcxl/fcitx5-voice-input.git
cd fcitx5-voice-input
git submodule update --init --recursive

# 配置
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

# 编译
cmake --build build -j"$(nproc)"

# 安装
sudo cmake --install build --prefix /usr
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_TESTS` | `OFF` | 构建测试 |
| `ONNXRUNTIME_ROOT` | — | ONNX Runtime 自定义安装路径 |


## 注意事项

- **API Key 安全**：API Key 明文存储在 `~/.config/fcitx5/conf/voiceinput-openai.conf`、`voiceinput-volcengine.conf` 和 `voiceinput-mistral.conf` 中，请注意文件权限
- **网络要求**：OpenAI / 火山引擎 / Mistral 后端需要网络连接。内置的本地后端 `sherpa_onnx`（流式）与 `sherpa_offline`（X-ASR 离线）完全在本机识别，无需联网，见 [sherpa-onnx.md](docs/guide/providers/sherpa-onnx.md) 与 [sherpa-offline.md](docs/guide/providers/sherpa-offline.md)
- **音频设备**：录音路由由系统音频服务（PulseAudio / WirePlumber）决定。插件不枚举、不筛选设备，也不保存设备 ID：建流时不指定 source，尊重系统默认输入或音频管理器为本应用保存的设备偏好。暂不支持手动指定设备；请确认默认输入不是回放 Monitor 源
- **VAD 模型**：Silero VAD 模型通过 git submodule 分发（`third_party/silero-vad/`），编译时自动复制到安装目录。构建前务必执行 `git submodule update --init --recursive`
- **PipeWire 用户**：PulseAudio 后端也能在 pipewire-pulse 下正常工作，仅在 PulseAudio 完全不可用时 fallback 到 PipeWire 直连
- **本地 ASR**：已实现。`sherpa_onnx` 在本机流式识别（边说边出增量），`sherpa_offline` 整句离线解码（X-ASR）。模型不随安装包分发，用 `scripts/download-model.sh --online` / `--offline` 下载后在 `ActiveBackend` 中选择对应后端
- **窗口切换**：失焦会立即暂停识别（丢弃新采样、取消在途会话），但录音流保持打开；回焦复用同一条流。切换到其他输入法、更改输入法组或退出 Fcitx5 时才释放录音流

## 架构简介

```
音频捕获线程 → FrameQueue → VAD Worker → SpeechEventQueue → ASR Dispatcher → AsrSession 会话 worker
    → ResultCoordinator（结果保序 + 可选 LLM 后处理）→ ResultQueue → EventDispatcher → commitString

SpeechEvent 类型: Begin（说话开始）→ Audio（32ms 帧，Pipeline 聚合到 200ms）→ End（静音）/ Cancel（语音太短丢弃）
```

主线程 + 捕获线程 + VAD worker + 每语音段独立的会话 worker，通过 `ThreadSafeQueue` 连接各阶段。每句语音是一个 `AsrSession`（由 `AsrEngine::StartSession` 工厂创建），完成后由 `SessionReaper` 超时回收，并发结果按说话顺序保序上屏。详见 [ARCHITECTURE.md](docs/architecture/index.md)。

## 许可证

GNU Lesser General Public License v3.0
