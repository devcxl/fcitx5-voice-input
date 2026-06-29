<div align="center">

<p align="center">
  <img width="1774" alt="Banner" src="https://github.com/user-attachments/assets/ecd6665e-937a-4e81-8472-bd065dd5261f" />
</p>

# fcitx5-voice-input

<p>
  <a href="https://github.com/devcxl/fcitx5-voice-input/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/devcxl/fcitx5-voice-input/build.yml?branch=main&logo=github&label=build" alt="Build"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-LGPL%20v3-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-Linux-important" alt="Platform">
  <img src="https://img.shields.io/badge/fcitx5-%3E%3D5.1.19-blueviolet" alt="Fcitx5">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20">
</p>

[中文](README.zh-CN.md)

---

</div>

**fcitx5-voice-input** is a Fcitx5 addon for voice input. Captures audio via PulseAudio (or PipeWire fallback), detects speech segments with Silero ONNX VAD, and transcribes via OpenAI-compatible API.

## Features

- Voice input (OpenAI Whisper API / compatible services)
- Silero ONNX VAD for automatic speech segmentation (no push-to-talk required)
- Queue-based pipeline: Audio Capture → VAD → ASR → EventDispatcher → commit
- Graphical configuration via `fcitx5-configtool`
- Smart delayed stop on window switching

<p align="center">
  <img width="720" alt="Demo" src="https://github.com/user-attachments/assets/48164962-deba-4328-bf26-70cd258f86a6" />
</p>

## Usage

### 1. Installation

#### Build from source

See [Build](#build) below.

### 2. Configuration

After installation, open `fcitx5-configtool`, find **Voice Input** in the Input Method list and add it.

Then open the Addon config for **VoiceInput** and set:

| Option | Description | Default |
|--------|-------------|---------|
| `ASRBackend` | ASR backend | `openai` |
| `OpenAIEndpoint` | API endpoint URL | `https://api.openai.com/v1` |
| `OpenAIApiKey` | API Key | **(required)** |
| `OpenAIModel` | Model name | `whisper-1` |
| `OpenAILanguage` | Output language, empty for auto | (empty) |
| `AudioSource` | Input device, empty for auto | (empty) |
| `VADThreshold` | VAD sensitivity (0-100), higher = less sensitive | `50` |
| `SilenceThresholdMs` | Silence duration to end utterance (ms) | `800` |

**API Key**: Fill in your API Key in `OpenAIApiKey`. Compatible with any OpenAI-format service:

- [OpenAI](https://platform.openai.com/) — `https://api.openai.com/v1`
- [Groq](https://console.groq.com/) — `https://api.groq.com/openai/v1`
- [SiliconFlow](https://cloud.siliconflow.com) — `https://api.siliconflow.com/v1`

### 3. How to Use

1. Switch to **Voice Input** IME
2. Start speaking — VAD automatically detects speech and records
3. Stop speaking (default 800ms silence timeout) — audio is sent for ASR
4. Recognition result is committed automatically
5. Stay in Voice Input mode and continue speaking for consecutive recognition

When switching windows, the plugin delays stop by 200ms. Quick switch-back cancels the stop, avoiding unnecessary restarts.

## Build

### Dependencies

- `fcitx5` — Input method framework
- `libpulse-simple` — PulseAudio capture (preferred)
- `libpipewire-0.3` — PipeWire capture (fallback)
- `jsoncpp` — JSON parsing
- `libcurl` — HTTP client (required for OpenAI ASR)
- `onnxruntime` — Silero VAD ONNX Runtime

> **Arch Linux:** `sudo pacman -S fcitx5 pulseaudio pipewire jsoncpp curl onnxruntime-cpu`
>
> **Debian/Ubuntu:** `sudo apt install fcitx5 libpulse-dev libpipewire-0.3-dev libjsoncpp-dev libcurl4-openssl-dev libonnxruntime-dev`

### Build Steps

```bash
# Clone and init submodules (for Silero VAD model)
git clone https://github.com/devcxl/fcitx5-voice-input.git
cd fcitx5-voice-input
git submodule update --init --recursive

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

# Build
cmake --build build -j"$(nproc)"

# Install
sudo cmake --install build --prefix /usr
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | `OFF` | Build tests |
| `ONNXRUNTIME_ROOT` | — | Custom ONNX Runtime install path |


## Notes

- **API Key Security**: API key is stored in plain text in `~/.config/fcitx5/conf/voiceinput.conf`. Ensure proper file permissions
- **Network Required**: OpenAI backend requires internet. Local ASR can be added via the AsrEngine interface
- **Audio Device**: Auto-selects system default input. To specify a device, choose from the `AudioSource` dropdown. Only input sources are listed (no Monitor sources)
- **VAD Model**: The Silero VAD model is distributed via git submodule (`third_party/silero-vad/`) and copied to the install directory at build time. Run `git submodule update --init --recursive` before building
- **PipeWire Users**: The PulseAudio backend works fine under pipewire-pulse. Native PipeWire is only used as fallback when PulseAudio is completely unavailable
- **Local ASR**: Not yet implemented. The codebase provides an `AsrEngine` abstract interface for future local ASR integration
- **Window Switching**: A 200ms delayed stop prevents unnecessary restarts on quick window switches. Long inactivity will stop the pipeline

## Architecture Overview

```
Audio Capture Thread → FrameQueue → VAD Worker Thread → UtteranceQueue → ASR Worker Thread → ResultQueue → EventDispatcher → commitString
```

Three worker threads + main thread, connected by `ThreadSafeQueue`. See [ARCHITECTURE.md](ARCHITECTURE.md) for details.

## License

GNU Lesser General Public License v3.0
