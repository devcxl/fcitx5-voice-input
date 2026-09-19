---
change: realtime-commit-tracking
cabbage_stage: tests
change_type: bugfix
---

# Strategy

缺陷位于「服务端事件 → 终态判定」的状态机，可用两级 seam 覆盖：`RealtimeCommitTracker` 的纯判定逻辑（单元、确定性），以及真实 `RealtimeAsrSession` + 假 WS 服务端的端到端行为（复刻 issue 场景，验证 final 的实际到达时间）。另有对「终止时长常量关系」的静态断言测试。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| Unit | 最终 item 判定的四条规则、failed 的精确移除、空 item_id 的保守处理 | `RealtimeCommitTracker::{OnCommitted,OnCompleted,OnFailed,MarkEndCommitSent}` | maintainer |
| Unit (constants) | 空闲超时 ≤ 5s、小于 End 总预算、End 总预算 < reaper 15s | `kEndIdleTimeout` / `WsEndBudget` / `WsDeadline` | maintainer |
| E2E (session) | 周期 commit 被 error 拒绝 → End commit → 最终 completed 立即上报（含文本正确性） | `RealtimeAsrSession::SetResultCallback` | maintainer |

不覆盖：Volcengine/Mistral 的对应路径（无同类判定逻辑；Mistral 仅在事件名与发送分支上同构，其回归修复由 `ws_send_budget_test` 的 End 收敛用例间接保护）。

# Test Environment and Data

- 环境：本地回环 TCP；E2E 用例在无 WS 支持的 libcurl 上整体跳过并返回 0。
- 假服务端 `CommitErrorWsServer`：解析客户端 TEXT 帧（处理掩码位），对第 1 次 `input_audio_buffer.commit` 回 `error`（`input_audio_buffer_commit_empty`，复刻 issue），对第 2 次（End）**不回 committed ack**、延时 80ms 后回最终 `completed`（`transcript: "最终文本"`）。这正是 issue 实测的服务端行为。
- 数据：单测直接构造 item id 序列；E2E 喂入 1.6k 帧音频（约 2.5s 内触发周期 commit）。
- 隔离与清理：单测无 I/O；E2E 每用例独立服务端与线程，结束时 `Cancel()` + `JoinWithTimeout(3s)` 并关闭服务端。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | 服务端确认 End item，周期 item 的 completed 先到 | Unit | 周期 item 非最终；End item 为最终且被移除 | High |
| T-2 | End commit 未发出时的 completed | Unit | 非最终 | High |
| T-3 | **周期 commit 被 error 拒绝（不产生 item），随后 End item 的 completed** | Unit | 立即判最终（计数不漂移） | High |
| T-4 | 服务端完全不回 committed ack，只回 completed | Unit | 立即判最终（不退化为等满兜底） | High |
| T-5 | End item 未知且存在已确认周期 item，先到周期 completed | Unit | 周期 completed 非最终；随后未 ack 的 item 为最终 | High |
| T-6 | `failed` 移除自身 item，不影响 End item 判定 | Unit | 在途数正确；End item 仍为最终 | Medium |
| T-7 | completed 不带 item_id 且存在在途项 | Unit | 非最终（不绕过在途检查）；在途清空后可判最终 | Medium |
| T-8 | 空闲超时与预算的常量关系 | Unit | `kEndIdleTimeout ≤ 5s` < `WsEndBudget` < 15s；`Arm` 幂等不延长截止时刻 | High |
| T-9 | **复刻 issue 场景（周期 commit 被 error、无 ack、End 后 80ms 补发最终 completed）** | E2E | final 在 **3s 内**到达且文本为 `最终文本` | High |

# Regression Coverage

- 既有 9 个测试全部保留：`session_worker_completion_test`、`audio_frame_assembler_test`、`llm_request_cancellation_test`、`asr_ordering_test`、`result_coordinator_test`、`realtime_terminal_state_test`（终态唯一发布契约）、`ws_frame_reassembly_test`、`ws_send_budget_test`、`ordered_buffer_stall_test`（`ctest` 合计 11 个）。
- 每会话仅上报一次 final：`realtime_terminal_state_test::TestTerminalCanOnlyBePublishedOnce` 仍通过。
- RED 证据（以真实修复前代码运行 T-9）：`git stash` 恢复 `realtime_asr.cpp` 的计数实现后，实测输出
  `commit-error: final=1 text="" after 30ms` → 断言「文本必须是服务端转写」失败；恢复修复后为 `text="最终文本" after 161ms`。
- T-3 的 RED 证据（突变）：把判定规则 3 替换为「按集合是否清空」后，`TestErrorOnPeriodicCommitDoesNotDelayFinal` 失败（`the final completed must be recognized immediately after a rejected periodic commit`）。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| 时延（终态） | T-9 实测 End → final 的墙钟时间 | < 3s（修复前为 30s 级兜底） |
| 有界性 | T-8 常量关系 + E2E 的 12s 观察窗口 | 空闲超时 5s ≤ End 预算 8s < reaper 15s；用例 `ctest` `TIMEOUT 90` |
| 鲁棒性 | T-3~T-7 覆盖缺失 ack、重复/空 item_id、无关 failed | 任一情形都不得退化为「等满兜底」 |

# Entry and Exit Criteria

- Entry: 能构造「周期 commit 被 error 拒绝 + End commit 后补发 completed」的服务端行为。
- Exit: `ctest` 11/11 通过；T-9 在修复前代码上失败（文本为空、30ms 提前收尾），修复后 161~192ms 拿到 `最终文本`；`cabbage validate/gate` 通过。

# Risks

- T-9 依赖固定等待：喂音频最多 2.5s（以便周期 commit 触发）+ 80ms 服务端延时；阈值取 3s（约 15× 余量），并使用 `ctest` `TIMEOUT 90` 兜底，降低 flaky 风险。
- T-9 需要解析客户端帧的掩码位（libcurl 会掩码）：由测试内实现；若未来 libcurl 改变掩码行为，用例会因「周期 commit 未到达」而失败并给出明确信息。
- 判定规则 3 的边界（服务端顺序异常）由单测固定语义；若服务端行为变化需同步更新 T-5。
