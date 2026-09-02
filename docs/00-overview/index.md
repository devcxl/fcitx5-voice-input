# 项目概览

**fcitx5-voice-input** 是一个专为 Linux 桌面设计的 [Fcitx5](https://fcitx-im.org/) 语音输入法插件（Addon），提供高准确率、低延迟的中文及多语言语音输入能力。

---

## 快速导航

- [入门指南](/00-overview/getting-started)：5 分钟快速上手语音输入与日常操作流程。
- [快速安装指南](/00-overview/installation)：AUR、各发行版预编译包（DEB / RPM）与源码编译安装详解。
- [配置指南](/00-overview/configuration)：详细参数说明、各 ASR 后端配置示例与 LLM 后处理调优。
- [常见问题与排查 (FAQ)](/00-overview/troubleshooting)：麦克风故障、API 报错与调试日志获取方法。

---

## 核心特性

- **自动人声检测 (VAD)**：集成轻量级 Silero ONNX VAD 模型，自动判定说话起止并分段，完全告别手动按键长按。
- **多 ASR 后端支持**：
  - **OpenAI 兼容后端**：支持 OpenAI Whisper 官方 API、Groq、硅基流动（SiliconFlow）以及阿里云百炼 DashScope（`qwen3-asr-flash`）。
  - **火山引擎豆包 ASR**：低延迟 WebSocket 流式识别，支持实时增量上屏、逆文本标准化 (ITN) 与二次语义修正。
  - **Mistral Realtime**：原生 16kHz PCM WebSocket 流式转写。
  - **OpenAI Realtime**：24kHz 双向流式增量转录。
- **实时增量候选 (Preedit)**：流式识别模式下，说话过程中候选栏实时显示中间识别字词。
- **LLM 智能后处理**：支持将识别文本送入大语言模型进行同音错字修正、排版美化与标点润色。
- **高韧性音频捕获**：PulseAudio 优先并支持原生 PipeWire 直连降级；运行期 `dlopen` 延迟加载，系统音频库升级无缝兼容。
- **智能防误触设计**：窗口快速切换时具备 200ms 防抖保护，切回后无缝继续输入。

---

## 技术栈概览

| 模块组件 | 技术方案 | 特性与优势 |
|---|---|---|
| **插件架构** | Fcitx5 C++20 Addon | 纯共享库无守护进程，常驻 Fcitx5 进程内低开销运行 |
| **音频采集** | PulseAudio + PipeWire | `dlopen` 延迟绑定，支持优先捕获硬件麦克风 |
| **语音分段** | Silero ONNX Runtime | 16kHz mono int16，512 采样点/窗口（32ms）极速低开销推理 |
| **管道架构** | 多线程安全队列 (SPSC/MPSC) | Capture → VAD → ASR Dispatcher → Session Reaper → EventDispatcher |
| **网络通讯** | libcurl (HTTP / WebSocket) | 支持多路并发会话管理与乱序汇聚按序上屏 |
| **打包分发** | CMake + CPack + AUR | 覆盖 Arch、Ubuntu、Debian、Fedora、openSUSE 等主流发行版 |

---

## 进阶文档

- [系统架构设计](/03-architecture/)：详细设计、会话模型与 ADR 决策记录。
- [产品需求与规范](/01-product/)：产品功能定义与排除范围。
- [CI/CD 多发行版构建](/11-ci-cd/)：多发行版容器化构建矩阵分析。
