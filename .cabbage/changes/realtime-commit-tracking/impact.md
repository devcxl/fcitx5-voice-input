---
change: realtime-commit-tracking
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

修复 Realtime 后端的终态判定缺陷（GitHub issue #44）：`commitsInFlight` 计数器只在 `completed` 与 `transcription.failed` 事件上递减，服务端返回的 `error` 事件被明确忽略，因此任何一次 error（例如对空 buffer 的 `input_audio_buffer_commit_empty`）都会让计数永久虚高，最终的 `completed` 被误判为「非最终 item」，会话只能等兜底超时才收尾。issue 实测：服务端在 End commit 后约 200ms 内就补发了最终 `completed`（内容 `最终文本`），客户端却等了 **30037ms**。

本变更同时处理 issue 指出的另外两点：error 事件完全静默（用户无法区分「网络慢」与「服务端拒绝」），以及固定的 30s 硬兜底窗口。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | Yes | 正常情况下本应亚秒级返回的最终结果不再被固定推迟 30s；服务端错误可见（状态栏「语音识别失败」+ 日志含 message/code） |
| Architecture | Yes | 新增 `src/addon/asr/utils/realtime_commit_tracker.h`（按 item_id 追踪在途 item + 判定最终 item），替代内联计数器；`realtime_asr.cpp` 的事件处理收敛为对 tracker 的调用 |
| API | No | `AsrSession`/`AsrEngine` 契约不变 |
| Database | No | |
| Security | No | error message 来自服务端，随日志输出；转写内容仍只记长度 |
| Testing | Yes | 新增 `tests/realtime_commit_tracking_test.cpp`（9 个单测）与 `tests/realtime_commit_e2e_test.cpp`（真实会话 + 假服务端复刻 issue 场景） |
| Deployment | No | |
| Operations | Yes | End 等待由固定 30s 改为「距最后服务端事件 5s 空闲超时」；新增 error 事件日志与发送中止路径的回归修复 |
| Data | No | |
| Performance | No | 判定由 O(1) 计数变为 O(log n) 集合查找；结果集通常 ≤3 项 |

# Impact Details

- **Architecture**: `RealtimeCommitTracker` 以服务端确认的 `input_audio_buffer.committed` 事件登记在途 item（以服务端确认而非本地发送为准，避免把被拒绝的 commit 计入），并在 `completed` 时判定是否最终 item。判定优先级：End commit 未发出 → 非最终；已知 End item id → 精确匹配；未知 End item id 且事件带 item_id → **未确认过的 item（不在在途集合）即为最终**（这是修复点：error 被拒绝的 commit 不产生 item，其 completed 在 End 之后出现即为最终）；事件不带 item_id → 以集合是否清空判定。
- **Product**: End 后的最终结果在服务端返回时立即上屏（实测 161~192ms），不再固定等 30s。
- **Operations**: End 等待窗口改为「距最后一个服务端事件 5s」（`kEndIdleTimeout`），远小于 reaper 的 15s join 与 `WsEndBudget` 的 8s 总预算；error 事件以 `FCITX_WARN` 打印 message/code 并经 error 回调上报。
- **附带修复（回归）**：新增的端到端用例暴露出此前（PR #46）引入的一处回归——End 到达时推流发送被 `WsAbort` 中止会被误判为「传输失败」，从而走重连/兜底分支、跳过 End 收尾路径，导致尾部音频与最终结果丢失（实测以空文本在 30ms 内提前收尾）。修复：中止源自 `finished`/`cancelled` 时不置 `reconnectNeeded`，而是回到循环顶部消费 End 哨兵。Realtime 与 Mistral 两处同构路径均已修正。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| 「未知 End item id 且事件带 item_id → 判最终」在服务端顺序异常时误判周期 item 为最终 | Low | 该会话可能用周期文本提前收尾（但仍是已识别内容，且不丢字） | 仅在「该 item 从未被服务端 ack 过」时成立；已 ack 的周期 item 仍走非最终分支（有单测覆盖）。服务端正常情况下会回 `input_audio_buffer.committed`，走精确匹配 | maintainer |
| End 空闲超时 5s 对偶发慢的上游偏紧 | Low | 以已累积文本兜底上屏（内容不丢，可能少尾部增量） | 空闲超时以「最后一个服务端事件」为基准而非固定总时长，慢但有进展的上游不会被误判；且仍受 `WsEndBudget`（8s）与 reaper（15s）双重上界约束 | maintainer |
| 判定逻辑改动影响既有终态契约（每会话仅一个 final） | Low | 重复 final 或漏 final | `RealtimeTerminalState` 的 `TryMarkPublished()` 保证只发布一次（既有测试 `realtime_terminal_state_test` 覆盖）；新用例断言 final 文本内容 | maintainer |
| 服务端 `error` 事件持续刷屏 | Low | 日志噪音 | 仅在事件到达时打印；error 不产生 item，不影响终态判定 | maintainer |

# Documentation Updates

- `docs/03-architecture/system-design/ARCHITECTURE.md`：Realtime 事件处理说明（item_id 追踪、error 可见、空闲超时）。
- `docs/08-testing/README.md`：登记两个新测试。
- `docs/13-operations/troubleshooting.md`：error 事件日志与「最终结果不再固定等 30s」的说明。
- `docs/01-product/prd/product-prd-v1.md`：成功指标中的流式延迟表述与实测一致。
