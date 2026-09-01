# 项目概览

**fcitx5-voice-input** 是一个 [Fcitx5](https://fcitx-im.org/) 输入法插件（addon），提供中文语音输入能力。

## 功能特性

- 语音输入：OpenAI Whisper API / 兼容服务、Volcengine Doubao 流式 ASR、Mistral Realtime 流式转写
- Silero ONNX VAD 自动语音分段（无需按键说话）
- 流式实时增量上屏（Volcengine / Mistral Realtime / OpenAI realtime 后端）
- 队列管道：Audio Capture → VAD → ASR → EventDispatcher → commit
- `fcitx5-configtool` 图形化配置
- 窗口切换智能延迟停止

## 技术栈

| 组件 | 技术 |
|------|------|
| 插件形态 | Fcitx5 addon（共享库，无 daemon / CLI / Qt） |
| 音频捕获 | PulseAudio 优先，PipeWire fallback（dlopen 延迟加载） |
| VAD | Silero ONNX（16kHz mono int16，512 samples/窗口） |
| ASR | OpenAI 兼容 API / Volcengine / Mistral Realtime（HTTP/WebSocket） |
| 构建 | CMake + CPack（DEB/RPM/pkg.tar.zst） |

## 相关链接

- 用户使用文档：[GitHub README](https://github.com/devcxl/fcitx5-voice-input)
- [系统架构](/03-architecture/)
- [产品需求](/01-product/)