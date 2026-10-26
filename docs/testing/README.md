# 测试现状

> 状态：当前实现文档。测试源在仓库 `tests/`，由 CMake 选项 `BUILD_TESTS`（默认 `OFF`）编译，`ctest` 运行。

## 运行方式

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

CI 中 `ci.yml` 的 `tests` job 在具备 WS 能力的 Ubuntu 26.04 上开启 `BUILD_TESTS` 并执行 `ctest`；多发行版 build 矩阵不重复跑测试。

## 测试清单

| 测试 | 覆盖范围 | 类型 |
|------|---------|------|
| `session_worker_completion_test` | worker 完成标志的等待语义（`WaitForWorkerCompletion`） | 单元 |
| `audio_frame_assembler_test` | 音频帧组装（16kHz/512 samples 窗口） | 单元 |
| `llm_request_cancellation_test` | LLM 请求按 generation 取消/发布门 | 单元 |
| `result_coordinator_test` | 结果汇聚：保序、跳过、LLM 回退、终态不丢 | 单元 + 并发 |
| `asr_ordering_test` | `OrderedResultBuffer` 保序与引擎容量取消 | 单元 |
| `realtime_terminal_state_test` | Realtime 终态判定与传输失败决策表 | 单元 |
| `ws_frame_reassembly_test` | WS 消息重组：跨 TCP 分片保留前缀、超限整条丢弃且边界恢复、类型过滤、CLOSE 复位；真实 `RealtimeAsrSession` / `VolcengineAsrSession` 分片端到端 | 单元 + 集成（本地假 WS 服务端） |
| `realtime_commit_tracking_test` | Realtime 终态判定：按 `item_id` 追踪在途 item、`error` 不造成计数漂移、缺失 ack 时仍能识别最终、`failed` 精确移除、End 空闲超时常量关系 | 单元 |
| `realtime_commit_e2e_test` | 复刻 issue #44 场景：周期 commit 被 `error` 拒绝 + End 后补发最终 `completed` → final 在 3s 内到达且文本正确 | 集成（真实会话 + 假 WS 服务端） |
| `ordered_buffer_stall_test` | 保序闸门停滞放行：卡死段在阈值内不误伤、超阈值后以超时错误放行、后续段按序交付、迟到结果被丢弃、显式 `Skip` 语义不变；`ResultCoordinator` 端到端交付 | 单元 + 并发 |
| `chat_asr_request_test` | chat 模式请求体构造：language 按配置发送、`enable_itn` 可关、空 `asr_options` 省略、model/messages/data URL 形状 | 单元 |
| `ws_send_budget_test` | WS 发送预算：单次发送到期即失败（不无限重试）、取消优先；真实 Realtime/Mistral/Volcengine 会话在「上游不读 socket」下 `End()` 有界收敛、产出终态、`JoinWithTimeout` 不超时 | 单元 + 集成（本地假 WS 服务端） |
| `engine_ui_*` | 输入框销毁、有效面板清空、worker 回调排队后销毁、快速重新激活时过滤旧 UI 任务、待提交文本只提交给原输入框 | 集成（真实引擎 + Fcitx dispatcher） |
| `ws_invalid_response_*` | Realtime/豆包异常 JSON、字段类型/数值溢出、嵌套结果深度、非法 gzip；断言一次错误/一次空 final、无非法文本、worker 在 Cancel 前自行完成；可空字段后仍交付正常 delta | 集成（真实会话 + 本地假 WS 服务端） |
| `ws_worker_*` | partial/error/final callback 抛异常、非标准异常、final 不重试；豆包空确认帧后仍正常交付 | 集成（真实会话 + 本地假 WS 服务端） |
| `engine_ui_focus-*` | 失焦保流、回焦复用、新旧 generation、无焦点关闭、死亡流关闭、其他上下文切换/停用 Voice Input 不误关 | 集成（真实引擎 + Fcitx dispatcher） |
| `pulse_capture_routing_test` | dev=nullptr/身份/格式、无 pactl、建流失败、暂停丢弃、在途 read 跨快速暂停/恢复 | 集成（真实 capture + dlopen 假库） |
| `pipeline_capture_lifecycle_test` | 保流期间无识别、取消会话、清旧应用缓存、健康流复用、死流恢复、幂等关闭 | 集成（真实 Pipeline/Capture + 外部边界替身） |
| `pipewire_capture_lifecycle_test` | 保流/丢弃、新音频恢复、旧 RT callback 跨 epoch、空 buffer 后首帧、error 后 liveness 与资源回收 | 集成（真实 capture + dlopen 假库；PipeWire 可选编译） |

## 约定

- 测试不依赖外部网络与真实 API Key：网络类用例在进程内起假服务端（回环端口），协议握手由测试自实现。
- 并发/线程用例必须有界：等待使用固定上限轮询，会话统一 `Cancel()` + `JoinWithTimeout`。
- 修复缺陷时遵循 RED → GREEN：先在未修复代码上运行同一用例确认失败，再实施修复并确认通过。
- 涉及「永久阻塞」类缺陷的用例显式设置 `ctest` `TIMEOUT`（`ws_frame_reassembly_test` 120s、`ws_send_budget_test` 90s）：回归时表现为超时失败，而不是挂住 CI。
- UI 测试链接本次构建的 addon，以 Linux ELF 符号替换隔离 `Pipeline` 启动/暂停/停止/采集状态和两个公开回调注册入口；不访问引擎私有状态，不启动录音/云端。每个用例使用构建目录下独立的 XDG 路径，不能读取真实用户配置。若改用 `-Bsymbolic` 等禁用符号替换的链接选项，需同步调整该测试边界。
- 假录音库仅在构建目录，不安装或覆盖系统库；capture 测试子进程用 LD_LIBRARY_PATH 在真实 dlopen 边界加载。Pipeline 测试仅替换 VAD 模型入口及 ASR 引擎，不替换 Pipeline 编排；测试不能改变用户默认输入或应用路由。
- WS 回归共用 `tests/support/fake_ws_server.h`：停用可中止收发/accept，join 后回收 socket；投递后保留连接，避免 EOF 触发的重连让未处理消息假通过。异常响应必须先观察 `workerDone` 再清理，不能先 Cancel 掩盖错误；回调观察值由共享测试夹具持有，超时也不能留下悬空捕获。
- `ws_invalid_response_*` / `ws_worker_*` 缺少运行期 WS 能力时返回 77，由 CTest 明确记录为跳过，不能据此宣称后端已通过协议验证。

## 崩溃修复验证记录（2026-10-09）

- 未修复时：公开 UI 销毁场景 SIGSEGV；两个非法字段场景均因 `Json::LogicError` SIGABRT。旧基线上的 8 个回调异常场景也均 SIGABRT。
- 修复后：Release 全套 28/28 通过；独立 Debug + AddressSanitizer/UndefinedBehaviorSanitizer 构建全套 28/28 通过。初版遗漏豆包空确认帧的兼容性，新增用例先失败后修复。
- UI 核心 5 个场景连续 10 轮通过；无真实账号、云端、录音或安装操作。这不等于真实云端兼容性/七发行版 WS 运行矩阵已经验证，见下方平台限制和 [后续任务](../development/tasks/asr-crash-follow-ups.md)。

## 系统路由/保流验证记录（2026-10-12）

- 旧猜源、200ms 失焦停流、未接投递门闸及旧 PipeWire 缓存分别由公开边界测试确认失败后修复。
- Release 完整构建与 38 项 ctest 通过；10 项核心生命周期测试重复 20 轮通过，无 PipeWire 可选构建与相关测试通过。复核补充的其他 Voice Input 上下文误关和空 buffer 后首帧均先失败后修复。链接卫生仍满足无录音库直接符号/DT_NEEDED。
- 用户于 2026-10-13 在自己的 XFCE 电脑上确认双麦克风正常、可通过音频管理器切换设备，接受该环境下的基本可用性。此为用户实测反馈，不是代理硬件测试；音频服务、实际采集后端和设备型号未提供，不推定 PulseAudio 与原生 PipeWire 均已覆盖。
- 其他桌面、未确认的采集后端，以及失焦无上传/回焦无旧音频/托盘关闭的逐项现场结果仍未验证。PulseAudio 同步 read/join 停止风险仍是独立后续任务。详见 [调研与限制](../development/research/issue-54-audio-source-routing.md)。

## 平台限制（已实测）

流式 WS 后端需要 libcurl 编译时启用 WebSocket。实测：

| 发行版 | 系统 libcurl | `ws`/`wss` 协议 |
|--------|-------------|----------------|
| Ubuntu 24.04 | 8.5.0 | 不可用 |
| Debian 12 | 7.88 | 不可用 |
| Ubuntu 26.04 / Debian 13 / Fedora 44 / openSUSE Tumbleweed | 8.x 新版 | 可用 |

`ws_frame_reassembly_test` 通过 `curl_version_info()` 探测 `ws` 协议：不具备时打印 `SKIP` 并跳过协议级用例（接收器状态机单测仍然执行），避免把平台限制误报为回归。CI 中新增 `tests` job（容器 `ubuntu-26.04`）保证协议级用例在具备 WS 的环境中真实执行。
