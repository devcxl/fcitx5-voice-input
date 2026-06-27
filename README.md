# fcitx5-voice-input

[English](#english) | [中文](#中文)

---

## English

**fcitx5-voice-input** is a Fcitx5 addon for voice input. Captures audio via PulseAudio (or PipeWire fallback), detects speech with Silero ONNX VAD, and transcribes via OpenAI-compatible API.

**Features**
- Speaker-independent Chinese speech recognition
- Silero ONNX VAD for automatic speech segmentation
- Queue-based pipeline: Capture → VAD Worker → ASR Worker → Main Thread
- Simple configuration via `fcitx5-configtool`

**Dependencies** `fcitx5`, `libpulse-simple`, `libpipewire-0.3`, `jsoncpp`, `libcurl`, `onnxruntime`

After cloning:
```bash
git submodule update --init --recursive
```

**Build**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
sudo cmake --install build --prefix /usr
```

**ASR Backends** OpenAI-compatible API (Whisper) — use with Groq, OpenAI, DeepSeek, etc.

---

## 中文

**fcitx5-voice-input** 是一个 Fcitx5 语音输入插件。通过 PulseAudio（或 PipeWire fallback）捕获音频，使用 Silero ONNX VAD 检测人声，通过 OpenAI 兼容 API 进行语音识别。

**功能**
- 中文语音输入
- Silero ONNX VAD 自动分段录音
- 队列管道架构：采集 → VAD Worker → ASR Worker → 主线程
- 通过 `fcitx5-configtool` 简单配置

**依赖** `fcitx5`, `libpulse-simple`, `libpipewire-0.3`, `jsoncpp`, `libcurl`, `onnxruntime`

克隆后：
```bash
git submodule update --init --recursive
```

**构建**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
sudo cmake --install build --prefix /usr
```

**ASR 后端** OpenAI 兼容 API（Whisper）— 可搭配 Groq、OpenAI、DeepSeek 等使用
