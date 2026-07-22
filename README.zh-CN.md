<div align="center">

<p align="center">
  <img width="1774" alt="Banner" src="https://github.com/user-attachments/assets/d3fe33eb-6c91-4000-925b-99eb83a498a7" />
</p>

# fcitx5-voice-input

<p>
  <a href="https://github.com/devcxl/fcitx5-voice-input/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/devcxl/fcitx5-voice-input/build.yml?branch=main&logo=github&label=build" alt="Build"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-LGPL%20v3-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-Linux-important" alt="Platform">
  <img src="https://img.shields.io/badge/fcitx5-%3E%3D5.1.19-blueviolet" alt="Fcitx5">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
</p>

[English](README.md) | **中文**

---

</div>

**fcitx5-voice-input** 是一个 Fcitx5 语音输入插件。通过 PulseAudio（或 PipeWire fallback）捕获音频，使用 Silero ONNX VAD 检测人声分段，通过 OpenAI 兼容 API 或火山引擎豆包流式语音进行识别。

## 功能

- 中文语音输入（OpenAI Whisper API / 兼容服务，或火山引擎豆包流式语音）
- Silero ONNX VAD 自动分段录音（无需手动按键控制开始结束）
- 说话过程中实时显示识别中间结果（火山引擎后端支持）
- 队列管道架构：音频采集 → VAD 分段 → ASR 识别 → EventDispatcher 上屏
- 通过 `fcitx5-configtool` 图形化配置
- 窗口快速切换自动延迟停止，防止误停

<p align="center">
  <img width="720" alt="演示" src="https://github.com/user-attachments/assets/48164962-deba-4328-bf26-70cd258f86a6" />
</p>

## 使用

### 1. 安装

#### AUR (Arch Linux)

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
| `ApiMode` | API 模式：`whisper`（标准 Whisper API）或 `chat`（百炼 Chat Completions） | `whisper` |
| `LLMEnabled` | LLM 后处理 | `false` |
| `LLMModel` | 后处理 LLM 模型 | （空） |
| `LLMSystemPrompt` | 后处理系统提示词 | （空） |
| `LLMStream` | LLM 流式输出 | `true` |
| `AutoCommit` | 无 LLM 时自动上屏 | `true` |

设置 `ActiveBackend=openai`，点击齿轮按钮，然后填入您的 API Key。支持所有 OpenAI 兼容服务，如：

- [OpenAI](https://platform.openai.com/) — `https://api.openai.com/v1`
- [Groq](https://console.groq.com/) — `https://api.groq.com/openai/v1`
- [硅基流动 (SiliconFlow)](https://siliconflow.cn/) — `https://api.siliconflow.cn/v1`
- [阿里云百炼 (DashScope)](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference) — `https://dashscope.aliyuncs.com/compatible-mode/v1`

  **注意：** 阿里云百炼使用的 `qwen3-asr-flash` 模型不走标准的 Whisper API，需要通过 Chat Completions 接口调用。使用时需将 `ApiMode` 设为 `chat`，并补充对应的 DashScope API Key。配置示例：
  ```
  BaseUrl=https://dashscope.aliyuncs.com/compatible-mode/v1
  ApiKey=your_dashscope_api_key
  Model=qwen3-asr-flash
  ApiMode=chat
  Language=zh
  ```
  百炼 ASR 的具体接口文档请参考[阿里云官方文档](https://help.aliyun.com/zh/model-studio/qwen-asr-api-reference)。

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

### 3. 使用

1. 切换到 **Voice Input** 输入法
2. 开始说话，VAD 自动检测人声并录音
3. 使用**火山引擎**后端时，说话过程中会实时显示识别中间结果
4. 停止说话（默认 800ms 静音超时），最终识别结果自动上屏
5. 保持语音输入模式，继续说话可连续识别

切换窗口时插件会自动延迟 200ms 停止，快速切回会取消停止，避免不必要的重启。

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

- **API Key 安全**：API Key 明文存储在 `~/.config/fcitx5/conf/voiceinput-openai.conf` 和 `~/.config/fcitx5/conf/voiceinput-volcengine.conf` 中，请注意文件权限
- **网络要求**：OpenAI 后端需要网络连接。本地 ASR 可通过 AsrEngine 接口后续扩展
- **音频设备**：默认自动选择系统音频输入设备。如需指定，在 `AudioSource` 下拉框中选择。仅支持输入源（Source），不支持 Monitor 源
- **VAD 模型**：Silero VAD 模型通过 git submodule 分发（`third_party/silero-vad/`），编译时自动复制到安装目录。构建前务必执行 `git submodule update --init --recursive`
- **PipeWire 用户**：PulseAudio 后端也能在 pipewire-pulse 下正常工作，仅在 PulseAudio 完全不可用时 fallback 到 PipeWire 直连
- **本地 ASR**：暂未实现。代码提供了 `AsrEngine` 抽象接口，后续可扩展本地引擎
- **窗口切换**：快速切换窗口时插件使用延迟停止机制（200ms），不会频繁重启流水线。长时间切出后会自动停止

## 架构简介

```
音频捕获线程 → FrameQueue → VAD Worker 线程 → SpeechEventQueue → ASR Worker 线程 → ResultQueue → EventDispatcher → commitString

SpeechEvent 类型: Begin（说话开始）→ Audio（32ms 帧，Pipeline 聚合到 200ms）→ End（静音）/ Cancel（语音太短丢弃）
```

三个工作线程 + 主线程，通过 `ThreadSafeQueue` 连接各阶段。详见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 许可证

GNU Lesser General Public License v3.0
