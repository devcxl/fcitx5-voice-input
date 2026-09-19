---
change: ordered-buffer-timeout-skip
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

修复保序闸门的**单点堵塞放大**（GitHub issue #43）：`OrderedResultBuffer` 是严格串行闸门，`nextUtteranceId_` 对应的语音段没有 terminal 结果时，后续已完成的语音段结果全部不下发，且 `ResultCoordinator` 只在 `SkipSession`/`SkipUtterance` 被显式调用时才推进闸门。因此任何一段 ASR 会话卡住/丢终态，都会把「单段延迟」放大成「整条链路不可见」。

issue 实测：第 1 个连接完全不应答、后续连接对每次 commit 立即回 `completed`，按真实 Begin/End 时序驱动 5 段语音，**5 段累计交付 0 条**；后 4 段在服务端都正常返回了 `completed`，但用户一条都看不到。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | Yes | 用户可见行为：卡住的那段改为「语音识别失败」提示（而非静默），后续段照常按序上屏；正常路径不变 |
| Architecture | Yes | `OrderedResultBuffer` 增加停滞判定与超时放行；`ResultCoordinator` 暴露 `ExpireStale()`；`Pipeline` 分发循环新增周期性检查 |
| API | No | 无对外接口变化；新增的 `ExpireStale()` 仅内部使用 |
| Database | No | |
| Security | No | |
| Testing | Yes | 新增 `tests/ordered_buffer_stall_test.cpp`（9 个用例，含确定性时间注入与并发端到端） |
| Deployment | No | |
| Operations | Yes | 新增 40s 停滞放行的可观测行为与排查说明（`docs/13-operations/troubleshooting.md`） |
| Data | No | |
| Performance | No | 检查为 O(log n) 查表 + 空跑；分发循环 500ms 一次，空闲时才触发 |

# Impact Details

- **Architecture**: `OrderedResultBuffer` 的 `PendingUtterance` 记录 `lastSeen`（最后一次回调时刻）。停滞判定的基准取「该段最后进展时刻」与「最早的被阻塞后续段到达时刻」中更早者——前者覆盖「段 1 出了一个 partial 就死掉」，后者覆盖「段 1 完全不应答但后续段已完成被扣住」。超阈值时清掉该段缓存增量，改为下推一条 `isError` 结果（沿用该段既有结果的 generation/sessionId，或用最早被阻塞段的元信息），随后推进闸门。所有判定逻辑提供可注入时间基准的 `*At` 变体，测试因此完全确定性。
- **Product**: 超时结果走既有的 `isError` 分支（`engine.cpp` 的 `PollResults` → 清 preedit + 状态栏「语音识别失败」），用户能区分「这段失败了」与「输入法卡死了」。
- **Operations**: 新增排查章节说明「某段卡住导致后续结果不上屏」的现象、40s 阈值的取值依据（非流式 LLM 的 30s `CURLOPT_TIMEOUT` + 余量）与处置方式。
- **Testing**: 关键场景「段 1 卡死 + 段 2..5 已完成 → 阈值内不放行、超阈值一次放出 5 条（1 条超时错误 + 4 条正常，顺序正确）」；另覆盖「队首完全无结果」「迟到结果丢弃」「多段连续停滞」「正常路径不误伤」「显式 Skip 语义不变」与 `ResultCoordinator` 端到端。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| 40s 阈值误伤合法的慢路径（如 LLM 后处理接近其 30s 超时） | Low | 本可正常上屏的段被判为超时失败 | 判定基准是「最后一次进展」而非「终点」：LLM 流式 token 会持续刷新 `lastSeen`；非流式路径 30s `<` 40s 阈值，且 ASR 侧 End 路径已有 8s 上界 | maintainer |
| 停滞放行后卡死段的迟到结果污染已推进的闸门 | Low | 结果错位 | 放行后 `nextUtteranceId_` 已推进，迟到结果命中 `result.utteranceId < nextUtteranceId_` 分支被丢弃（有用例覆盖） | maintainer |
| 周期性检查（500ms）引入额外锁竞争 | Low | 分发循环与结果回调争用 `ResultCoordinator::mutex_` | 检查仅在队列空闲时触发，且持有锁的时间为 O(log n) 查表；结果回调本就需要同一把锁 | maintainer |
| 部分结果（partial）已被交付后又被替换为超时错误，preedit 出现“先显示后清空” | Low | 观感上的闪烁 | 与既有 `isError` 分支一致（清 preedit + 提示失败）；相比永久静默是可接受的取舍（issue 修复方向 1 的既定取舍） | maintainer |

# Documentation Updates

- `docs/03-architecture/system-design/ARCHITECTURE.md`：`OrderedResultBuffer` 职责说明、错误处理策略表、FAQ 补充停滞放行语义。
- `docs/08-testing/README.md`：登记 `ordered_buffer_stall_test`。
- `docs/13-operations/troubleshooting.md`：新增「某段语音卡住导致后续结果不上屏」排查章节。
