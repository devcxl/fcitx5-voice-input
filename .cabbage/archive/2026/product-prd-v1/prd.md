---
change: product-prd-v1
cabbage_stage: requirement
change_type: feature
---

# Goal

**fcitx5-voice-input** 是运行在 Fcitx5 进程内的高韧性语音输入法插件（Addon）：切换到该输入法即自动捕获音频，通过 Silero ONNX VAD 自动检测说话起止分段，将语音转写为文本并上屏，全程无需手动按键（免按即说）。面向 Linux 桌面端中文及多语言输入场景，解决键盘不擅长表达"长句、专有名词、即时口述"的痛点。

本 PRD 以当前代码实现（`src/addon/`，版本 0.5.0）为准，反推产品完整形态作为可验证的现状基线，供后续需求变更、回归测试与文档同步参照。

## 产品定位与价值

- **免按即说语音输入**：切换输入法即启动；说话自动分段，静音自动提交，无需 Push-to-Talk。
- **多后端 ASR 可插拔**：默认 OpenAI 兼容 API（whisper-1），另支持火山引擎豆包、Mistral Realtime、OpenAI Realtime 三种 WebSocket 流式后端。
- **LLM 智能后处理**：可将转写文本送入大语言模型做同音错字修正、排版与标点润色。
- **发行版全量打包**：Arch（AUR/PKGBUILD）、DEB/RPM/pkg.tar.zst 覆盖主流桌面发行版。

# Users and Use Cases

| User or actor | Need | Primary use case |
|---|---|---|
| Linux 桌面端中文输入用户 | 长句/口述内容打字成本高，希望直接说话即得文本 | 切换到 Voice Input → 说话 → 静音后文本自动上屏 |
| 多语种用户 | 识别语音语言可配置 | 在配置中选择输出语言（自动/中文/英文） |
| 流式识别需求用户 | 长语音实时预览中间结果 | 使用 Volcengine / Mistral Realtime / OpenAI Realtime 后端，说话过程中候选栏实时显示增量文本 |
| 对文本质量要求高的用户 | 识别文本存在同音错字 | 启用 LLM 后处理（OpenAI 兼容后端），在候选栏显示"修正中"后提交润色文本 |

# Scope

## In Scope

本 PRD 覆盖 `src/addon/` 当前已实现的产品能力（现状还原）：

1. **音频捕获**：PulseAudio 优先、PipeWire 直连回退的双后端；16kHz/mono/int16/512 samples(32ms) 统一音频格式；运行期 `dlopen` 延迟加载音频库（无链接期依赖）。
2. **VAD 自动分段**：Silero ONNX 唯一后端，Idle/Speaking 状态机，pre-roll 预卷、静音判停、最短/最长语音限幅。
3. **ASR 抽象与会话管理**：AsrEngine/AsrSession 工厂抽象，会话容量控制（最多 3 活跃）、SessionReaper 防僵尸线程、generation 过滤。
4. **ASR 后端**：
   - OpenAI 兼容（默认）：whisper 音频上传 / chat 多模态 / realtime 流式，三种 apiMode；
   - 火山引擎豆包：WebSocket 流式，支持 ITN、标点、语义顺滑(DDC)、二次识别(nonstream)；
   - Mistral Realtime：16kHz PCM WebSocket 流式。
5. **LLM 后处理**：非流式/流式两类，停顿文本修正后上屏，generation 取消。
6. **结果汇聚与保序**：多会话并发结果按 utteranceId 保序汇聚，过滤过期 generation。
7. **Fcitx5 集成 UI**：输入法切换激活/停用、候选栏 preedit 增量预览、状态栏提示（就绪/录音中/修正中/失败）、200ms 防误触延迟停止、配置热更新。
8. **安全与隐私**：配置文件 0600 权限、非 TLS 端点明文凭据告警、日志仅记录长度不记录文本内容。
9. **打包分发**：CMake/CPack 生成 DEB/RPM，AUR PKGBUILD，7 发行版 CI 矩阵；依赖运行期 dlopen 降低升级耦合。

## Out of Scope

以下能力当前未实现，明确排除于本产品基线（未来可另行评估）：

- 本地/离线 ASR（Sherpa-ONNX 本地推理）
- Command 命令引擎（语音触发系统命令/快捷键）
- 场景系统（按应用窗口差异化配置）
- 热词/自定义词库优化
- VAD 多模型支持（仅 Silero ONNX）
- 手动 Push-to-Talk 交互模式（本产品为免按即说设计）

# Requirements

| ID | Requirement (SHALL/MUST) | Priority | Rationale |
|---|---|---|---|
| R-1 | 切换至 Voice Input 输入法时 SHALL 自动启动音频捕获与识别管道，无需任何按键操作 | Must | 核心产品定位——免按即说 |
| R-2 | 音频捕获 SHALL 优先尝试 PulseAudio，失败时 MUST 自动回退到 PipeWire 直连 | Must | PulseAudio 为兼容基线，PipeWire 桌面可降级直连避免依赖 pipewire-pulse |
| R-3 | 音频格式 MUST 统一为 16kHz / mono / int16 / 512 samples(32ms) 窗口 | Must | 各环节（捕获/VAD/ASR）格式契约 |
| R-4 | 运行时音频库（libpulse-simple / libpipewire）MUST 采用 `dlopen` 延迟加载，不产生链接期依赖 | Must | 系统音频库升级/soname 变化不影响已安装 addon |
| R-5 | VAD SHALL 使用 Silero ONNX 模型对每帧输出语音概率，并基于 Idle/Speaking 状态机判定说话起止 | Must | 说话分段是自动上屏的前提 |
| R-6 | VAD 判定开始说话时 MUST 附带 pre-roll 前摇音频（默认 300ms，可配置 0-1000ms） | Must | 避免语音起始被截断，保证转写完整 |
| R-7 | 检测到静音超过阈值（默认 800ms，可配置）或说话超过最长时长（默认 30s）SHALL 结束当前分段并提交 ASR | Must | 自动分段提交的判定依据 |
| R-8 | 不足最短语音时长（默认 300ms）的分段 SHALL 被取消而非提交 | Must | 滤除环境噪声/误触发片段 |
| R-9 | ASR 引擎 SHALL 提供统一的 AsrEngine/AsrSession 抽象，支持新增后端而不改管道 | Must | 后端可插拔架构 |
| R-10 | 活动会话数 SHALL 不超过 3，超限时取消最旧会话 | Should | 防止资源滥用与延迟堆积 |
| R-11 | 默认后端 OpenAI 兼容 SHALL 支持 whisper（`audio/transcriptions`）与 chat（`chat/completions`）两种非流式模式及 realtime 流式模式 | Must | 覆盖一次性上传与流式实时两种识别路径 |
| R-12 | OpenAI 兼容后端 SHALL 支持配置 baseUrl，从而兼容任意 OpenAI-compatible 服务（含本地化镜像） | Must | 服务地址可指向 Groq/SiliconFlow/百炼等兼容端点 |
| R-13 | OpenAI 兼容后端 SHALL 支持输出语言配置（auto/zh/en） | Should | 多语言转写需求 |
| R-14 | 火山引擎豆包后端 SHALL 通过 WebSocket 流式识别，SHALL 支持 ITN/标点/语义顺滑/二次识别开关及判停窗口配置 | Should | 流式低延迟与文本规整能力 |
| R-15 | Mistral Realtime 后端 SHALL 通过 WebSocket 以 16kHz PCM 流式识别 | Should | 多后端覆盖 |
| R-16 | OpenAI Realtime 后端 SHALL 将 16kHz 音频上采样至 24kHz 并周期 commit（默认 5s）以支持长语音流式 | Should | Realtime API 要求 ≥24kHz 输入 |
| R-17 | 启用 LLM 后处理时，最终识别文本 SHALL 经 LLM 润色后上屏；非流式输出立即提交，流式输出在候选栏逐字增量显示"修正中" | Should | 错字修正与排版润色是核心增值能力 |
| R-18 | 多会话并发结果 SHALL 按 utteranceId 保序汇合并按 generation 过滤过期结果 | Must | 避免乱序/残留结果上屏 |
| R-19 | 停用输入法后 SHALL 延迟 200ms 再真正停止管道，期间重新激活则取消停止 | Must | 窗口快速切换防误触断流 |
| R-20 | 任一 ASR 后端返回错误 SHALL 在状态栏提示失败并清空候选栏，不崩溃 | Must | 云端服务失败时的用户体验与健壮性 |
| R-21 | 配置文件（含 API Key）保存后 SHALL 收紧为仅所有者可读写（0600） | Must | 凭据安全 |
| R-22 | 对非 TLS 且非本机回环的端点 SHALL 记录明文凭据传输告警 | Should | 防止用户无意间明文传输 API Key |
| R-23 | 识别文本（个人信息）日志 SHALL 仅记录长度而非内容 | Must | 隐私保护 |
| R-24 | 所有配置（后端选择、VAD 阈值、后端参数、LLM 参数）SHALL 在 `fcitx5-configtool` 中可查看并通过 `setConfig` 热更新 | Must | 图形化配置与免重启生效 |

# Acceptance Criteria

### Scenario 1: 免按即说话音输入（默认 OpenAI whisper 后端）
- **GIVEN**: 已安装 addon，fcitx5-configtool 已添加 Voice Input 输入法且配置了有效 OpenAI 兼容 API Key
- **WHEN**: 用户切换到 Voice Input 输入法并正常说话一段话
- **THEN**: VAD 自动识别说话起止，静音后候选文本自动提交上屏，状态栏回到"语音输入就绪"
- [ ] 无需任何按键完成一次完整语音转写并提交

### Scenario 2: 流式实时增量（Volcengine 后端）
- **GIVEN**: ActiveBackend 设为 volcengine，已配置有效凭据
- **WHEN**: 用户切换输入法并持续说话超过一个增量周期
- **THEN**: 说话过程中候选栏实时显示中间识别字词并在静音后提交最终文本
- [ ] 说话期间可见增量 preedit，结束静音后提交完整文本

### Scenario 3: LLM 后处理
- **GIVEN**: OpenAI 兼容后端，已启用 LLMEnabled 并配置 llmModel
- **WHEN**: 一段含同音错字的语音转写后进入 LLM 修润
- **THEN**: 候选栏显示"修正中..."提示，随后修正后的文本提交上屏
- [ ] 最终上屏文本为 LLM 润色后结果且中间有"修正中"状态提示

### Scenario 4: 音频后端回退
- **GIVEN**: 系统存在 PipeWire 但不提供 pipewire-pulse（或 PulseAudio 启动失败）
- **WHEN**: 切换至 Voice Input 输入法
- **THEN**: 捕获后端自动从 PulseAudio 回退到 PipeWire 直连，语音输入仍可用
- [ ] 无 pipewire-pulse 环境下仍能完成语音转写

### Scenario 5: 窗口快速切换防误触
- **GIVEN**: 输入法正在激活状态，用户尚未开始说话
- **WHEN**: 用户快速在窗口间切换（重新激活 Voice Input 在 200ms 延迟窗口内）
- **THEN**: 管道不中断，取消待执行的停止指令，继续正常接收语音
- [ ] 窗口快速切换不导致录音中断/片段丢失

### Scenario 6: ASR 后端热切换
- **GIVEN**: 当前使用 openai 后端，已正确配置 volcengine 后端凭据
- **WHEN**: 用户在 fcitx5-configtool 中把 ActiveBackend 改为 volcengine 并保存
- **THEN**: 管道热更新为火山引擎会话，后续转写走新后端，无残留 session
- [ ] 配置保存后新的转写结果来自新后端且无旧会话残留

# Success Metrics

| Metric | Baseline | Target | Measurement window |
|---|---|---|---|
| 识别提交文本与发言内容一致性 | 无量化基线（当前无自动化测试） | 人工验收场景 1-6 全通过 | 每次发布验收 |
| 流式后端增量可见 | Volcengine/Mistral/Realtime 支持增量 | 静音判停后增量不丢失、保序上屏 | 发布回归窗口 |
| 音频后端回退成功率 | PulseAudio 优先/失败回退已实现 | PipeWire 直连环境正常工作 | 发行版打包 CI |
| 崩溃/异常韧性 | 无已知崩溃 | ASR 错误/空文本/后端缺失不导致 addon 崩溃 | 每次发布回归 |

# Dependencies and Constraints

- **构建依赖**：fcitx5（≥5.1.19）、jsoncpp、libcurl（≥7.86.0，WS 要求）、zlib、onnxruntime（Silero VAD）、pipewire-0.3/libpulse-simple 至少其一。
- **运行时依赖（dlopen）**：libpulse-simple / libpipewire 运行期按候选 soname 加载，库缺失时优雅降级。
- **Silero VAD 模型**：`silero_vad.onnx` 需随包分发至 `VOICE_INPUT_MODEL_DIR`，模型缺失时管道启动失败并回滚捕获。
- **Fcitx5 集成**：addon 为 `OnDemand=True` 共享库，必须保持主线程渲染/事件处理不被阻塞。
- **网络服务可用性**：云 ASR/LLM 依赖外部服务，需用户自行配置有效凭据与网络。

# Risks

| Risk | Impact | Mitigation |
|---|---|---|
| 云 ASR 服务不可用/网络波动 | 识别失败，用户体验下降 | 错误状态栏提示 + 优雅降级（不崩溃）；自动提交未识别为空时不上屏 |
| VAD 静音阈值误配 | 分段过碎或过长，转写质量下降 | 提供 VADThreshold/SilenceThresholdMs 等可配置参数，配置热更新 |
| 后端切换/热更新竞态 | 旧 session 残留转写异步回调 | SkipAllSessions + CancelAllSessions + generation 过滤，会话入 reaper 回收 |
| 明文端点误配导致凭据泄漏 | API Key 明文传输风险 | 非 TLS 非回环端点记录醒目告警 |
| 流式后端长语音资源占用 | 内存/连接占用增长 | 活跃会话上限 3 + 周期 commit + SessionReaper 超时回收 |

# Open Questions

- N/A — 本 PRD 为现状还原，不引入新决策；roadmap 能力（本地 ASR/Command/场景/热词）已在 Out of Scope 中显式标注，待未来单独立项评估。