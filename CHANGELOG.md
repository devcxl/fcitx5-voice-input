# Changelog

## [0.4.0] - 2026-08-11

### Added
- OpenAI GPT-Realtime 流式实时转录（#10/#11）
- 输入法图标 SVG + 多尺寸 PNG（#13）
- 录音后端可选依赖：PipeWire/PulseAudio 任一缺失时仅失去对应后端，addon 仍可构建（#19）
- 录音库运行期 dlopen 延迟加载：无链接期 DT_NEEDED 依赖，库升级/soname 变更不影响已安装 addon（#19）
- DEB 包补齐 Depends/Recommends 依赖声明；CI 新增 build-no-pipewire 降级构建验证（#19）

### Fixed
- 修复会话线程 UAF 与多处数据竞争（#14/#15）
- 日志降级、配置权限、停止路径与资源上限加固（#14/#16）
- Realtime 引擎周期 commit 失效 / End 尾部音频丢失 / preedit 回退（#12）
- 修复 PulseAudio Stop 跨线程 `pa_simple_free` 导致的 `free(): invalid pointer` 崩溃（#17）
- 修复 classicui 不显示图标问题（#13）

## [0.3.1] - 2026-07-14

### Changed
- 构建/安装脚本移至 `scripts/`

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
