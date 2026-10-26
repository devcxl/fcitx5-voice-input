# 路线图

记录 fcitx5-voice-input 的交付进展与规划方向。架构现状见 [ARCHITECTURE](/architecture/)，产品边界见 [Out of Scope](/product/out-of-scope)。

## 已交付 ✅

- [x] CMake 构建框架 + Fcitx5 Addon 骨架
- [x] PulseAudio + PipeWire 音频捕获（dlopen 延迟加载）
- [x] Silero ONNX VAD 分段
- [x] OpenAI 兼容 API ASR 引擎（whisper / chat / realtime）
- [x] Volcengine Doubao 流式 ASR 引擎
- [x] Mistral Realtime 流式 ASR 引擎（v0.5.0）
- [x] v4 会话模型：AsrSession / SessionReaper / ResultCoordinator 保序交付
- [x] LLM 后处理（流式/非流式，generation 取消）
- [x] fcitx5-configtool 配置界面（多后端子配置）
- [x] 多发行版打包（7 发行版 CI 矩阵：DEB / RPM / pkg.tar.zst）

## 待实现 ⏳

- [ ] **本地 ASR 引擎**——离线转写，sherpa-onnx 子模块已预留，未接入构建
- [ ] **Command 引擎**——外部命令云 ASR
- [ ] **场景系统**——按应用/窗口场景切换识别行为
- [ ] **热词优化**——专有名词与领域词汇增强
