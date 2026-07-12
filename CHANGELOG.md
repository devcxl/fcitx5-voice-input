# Changelog

## [0.3.0] - 2026-07-13

### Added
- 输入法图标：SVG 矢量图标 + 多尺寸 PNG（16/22/24/32/48）
- `voiceinput.conf` 输入法注册配置文件
- 输入法注册采用 `OnDemand=True` conf 模式，对齐 fcitx5 官方做法

### Fixed
- 修复 classicui 不显示图标问题（仅 SVG 不被 GTK 图标主题识别）
- 修复 conf + C++ `listInputMethods()` 双重注册冲突
- 清理无意义的假多语言占位

## [0.2.0] - 2026-06-30

### Added
- 火山引擎 WebSocket ASR 支持
- LLM 后处理客户端（文本润色）
- ASR Session Reaper（会话回收机制）

## [0.1.5] - 2026-06-15

### Added
- PulseAudio 音频捕获后端
- Silero VAD 集成
- 管道编排（FrameQueue → VADWorker → UtteranceQueue → ASRWorker）

## [0.1.0] - 2026-06-01

### Added
- 初始版本：OpenAI 兼容 API ASR
- PipeWire 音频捕获
- Fcitx5 InputMethodEngineV2 集成
