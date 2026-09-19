---
change: ordered-buffer-timeout-skip
cabbage_stage: tests
change_type: bugfix
---

# Strategy

停滞放行是纯状态机逻辑（输入：一系列带时间戳的提交；输出：按序交付的结果序列），因此主 seam 是 `OrderedResultBuffer` 的**可注入时间基准**变体（`SubmitAt` / `SkipAt` / `ExpireStaleAt`），使全部判定完全确定性、无真实等待。另有一层端到端验证：`ResultCoordinator` 构造时注入极小阈值，验证「卡死段 + 4 段正常」的完整交付链路。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| Unit (deterministic) | 停滞判定基准、放行语义、迟到丢弃、多段连续停滞、正常路径不误伤、显式 Skip 不变 | `OrderedResultBuffer::{SubmitAt,SkipAt,ExpireStaleAt}` | maintainer |
| Integration | 卡死段阻塞后续段 → 周期检查 → 全部按序交付（含一条超时错误） | `ResultCoordinator::ExpireStale` + `ResultQueue` | maintainer |

不覆盖：Pipeline 分发循环的 500ms 周期触发（属调度行为，其正确性由「`ExpireStale()` 幂等且无副作用」保证；单测覆盖该方法本身）。

# Test Environment and Data

- 环境：无 I/O、无网络、无睡眠（单测部分用注入的 `steady_clock::time_point`）。
- 数据：构造 `AsrResult` 的辅助函数 `Result(utteranceId, text, partial?, isError?, generation=1)`。
- 阈值：单测统一使用 1000ms 便于推理；端到端用例用 50ms + 80ms 真实睡眠（唯一需要真实时间的地方，用于跨过阈值）。
- 隔离与清理：每个用例独立构造 `OrderedResultBuffer` / `ResultCoordinator`，无共享状态。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | 队首出 partial、后续段未完成 | Unit | partial 立即可见；闸门不前进 | High |
| T-2 | 段 1 出 partial 后静默，段 2..5 均完成（issue 核心场景） | Unit | 900ms 时 0 条；1000ms 时一次放出 5 条：1 条超时错误（uid=1）+ 段 2..5 按序 | High |
| T-3 | 段 1 完全无结果，段 2 完成 | Unit | 999ms 时 0 条；1000ms 时放出「超时错误 + 段 2」 | High |
| T-4 | 停滞放行后卡死段的迟到终态到达 | Unit | 迟到结果被丢弃（`ready.empty()`） | High |
| T-5 | 连续两段停滞 | Unit | 一次排空放出 3 条（2 条超时错误 + 1 条正常），顺序为 uid 1/2/3 | Medium |
| T-6 | 正常路径：A 有 raw partial → B final → A final | Unit | 阈值内严格保序：先 `A raw`，再 `A refined`, `B` | High |
| T-7 | 显式 `Skip(1)` 放行被阻塞的段 2 | Unit | 立即放行且**不**产生 `isError` 结果 | Medium |
| T-8 | `ResultCoordinator(50ms)`：段 1 卡死 + 段 2..5 完成 | Integration | 阈值内只交付 1 条（卡死段 partial）；阈值后 `ExpireStale()` 放出 1 条超时错误 + 4 条正常且顺序正确 | High |

# Regression Coverage

- 既有 9 个测试全部保留：`session_worker_completion_test`、`audio_frame_assembler_test`、`llm_request_cancellation_test`、`result_coordinator_test`、`asr_ordering_test`、`realtime_terminal_state_test`、`ws_frame_reassembly_test`、`ws_send_budget_test`（`ctest` 合计 10 个）。
- 其中 `asr_ordering_test` 的 4 个保序用例（含 `TestLlmResultBlocksLaterUtteranceUntilFinal`）是「不误伤正常慢路径」的直接回归保护：它们默认阈值下必须继续通过。
- 缺陷对照：把 `HeadStalledLocked` 的返回值改为恒 `false`（即恢复严格闸门），T-2 立即失败——`expiring a stalled head must release it and all later results`。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| 确定性 | 全部判定用例注入时间基准 | 无真实睡眠（除 T-8 的端到端跨阈值） |
| 并发安全 | `ResultCoordinator` 路径（`Notify` 在锁外调用） | 既有 `result_coordinator_test` 的并发用例继续通过 |
| 有界性 | 停滞放行必须一次排空所有可推进段 | T-5：单次调用放出 3 条 |

# Entry and Exit Criteria

- Entry: 能构造「某段无终态 + 后续段已完成」的缓冲状态并注入时间。
- Exit: `ctest` 10/10 通过；突变（关闭停滞判定）时 T-2 失败；`cabbage validate/gate` 通过。

# Risks

- 端到端用例（T-8）依赖真实 80ms 睡眠跨过 50ms 阈值：极端负载下可能不足。余量取 80ms（1.6×）并断言「放行后的交付内容」而非精确耗时，降低 flaky 风险。
- T-2/T-3 断言精确条数与顺序，若未来调整停滞语义（如保留 partial 再追加错误）需同步更新。
