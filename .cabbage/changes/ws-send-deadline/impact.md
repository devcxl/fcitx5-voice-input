---
change: ws-send-deadline
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

修复三个流式 WebSocket ASR 后端（OpenAI Realtime / Mistral Realtime / Volcengine）的**发送路径无截止时间**问题（GitHub issue #42）：`curl_ws_send` 返回 `CURLE_AGAIN` 时只做 10ms 退避 + 重试，仅检查 `cancelled`、不检查 `finished`，也没有 wall-clock 预算。上游不读 socket（卡死/限流/反代 hang，TCP 窗口关闭）时循环永不退出，而 `End()` 只置 `finished`，因此 End 路径一旦卡住就永远出不来，worker 在 `SessionReaper::JoinWithTimeout(15s)` 超时后被 detach 泄漏，该会话永不产生终态，保序缓冲把后续所有语音段结果一起扣住。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | No | 正常路径行为不变；仅在「上游卡死」这一原本永久卡死的路径上变为有界失败并交出兜底终态 |
| Architecture | Yes | 新增 `src/addon/asr/utils/ws_deadline.h`（`WsDeadline` / `WsEndBudget` / `WsAbort` / `WsAbortProgressCallback`）与 `ws_frame_sender.h`（`SendWsMessage`），三处发送实现收敛为共享实现 |
| API | No | `AsrSession` / `AsrEngine` 契约不变 |
| Database | No | |
| Security | No | 无新增信任边界；错误日志只含长度与预算值，不含转写内容 |
| Testing | Yes | 新增 `tests/ws_send_budget_test.cpp`：发送预算单测 + 三个真实会话在「上游不读」下的 End 有界收敛用例 |
| Deployment | No | |
| Operations | Yes | 新增可观测信号：发送超时错误日志（含待发字节数与预算）与「End reached, skipping reconnect」告警；行为由「永久挂起」变为「有界失败 + 兜底终态」 |
| Data | No | |
| Performance | No | 正常路径仅多两次原子读；退避睡眠与旧实现一致（10ms） |

# Impact Details

- **Architecture**: `WsDeadline` 按值复制共享同一截止时刻（复制不重置计时），用于把「同一逻辑操作」的多次发送绑到一份预算；`WsEndBudget` 为跨线程（`End()` 由 pipeline 线程武装、worker 读取）的原子预算，覆盖 End 后的 flush/commit、等待终态与可能发生的重连；`WsAbort` 统一表达中止条件（`cancelled` 始终生效，`finished` 仅用于非 End 路径的推流与建连）。三个后端删除各自的局部发送函数，统一调用 `SendWsMessage()`。
- **Testing**: 单测覆盖「预算到期即失败」（实测 1500ms 预算在 1505ms 返回，而非无限重试）与「取消优先」。端到端用「握手后不再读取 socket」的假服务端驱动真实会话，断言 `End()` 后 worker 在 14s 内结束、产出终态、`JoinWithTimeout` 不超时（Realtime 3ms、Mistral 3ms、Volcengine 8034ms，均远低于 reaper 的 15s）。
- **Operations**: 新增 `WS send timeout after Nms (M bytes pending): peer is not reading` 与 `End reached, skipping reconnect` 两条日志，用于区分「网络慢」与「上游卡死」。End 预算耗尽时不再重连，直接以已累积文本兜底 final（避免「重连 → 再阻塞 → 被 detach」）。
- **Product**: issue #42 的「卡死会话不影响后续语音段结果交付」由「会话必然产生终态」保证：发送路径任何失败都会走到 `publishFinal` / `cb(..., true, sid)` 兜底分支，保序闸门因此不再被无终态会话永久扣住。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| End 预算（8s）对极端慢但在推进的上游过于激进，导致本可成功的收尾被放弃 | Low | 该段以已累积文本兜底上屏（内容不丢，仅可能少最后一段增量） | 预算远大于正常上游的收尾耗时（实测正常路径毫秒级）；单次发送预算为 10s 且按剩余时间收缩；如需放宽只改 `WsEndBudget::kDefaultTotal` | maintainer |
| `WsAbort` 存的是 `std::atomic<bool>*` 裸指针，若会话对象提前析构会悬空 | Low | 未定义行为 | 指针指向 `AsrSession::State`（`shared_ptr`，与 worker 存活期绑定，`asr_session.h` 已有同样约定）；`connectAbort_` 为会话成员，生命周期覆盖 `curl_easy_perform` | maintainer |
| 建连阶段响应 `finished` 后立即退出，可能让「End 与建连并发」的会话直接产出空 final | Low | 该段无转写结果（但本来就是卡死场景） | 与 issue 验收一致：有界失败优于永久挂起；空 final 会触发既有错误/清理分支并释放保序闸门 | maintainer |
| 三个后端的 End 预算语义需保持一致，后续新增后端可能遗漏 | Low | 新后端重新引入同类缺陷 | `WsEndBudget` 为共享实现；对应 issue #42 的端到端用例模式可作为新后端的验收模板 | maintainer |

# Documentation Updates

- `docs/03-architecture/system-design/ARCHITECTURE.md`：补充发送预算与 End 有界收敛的当前实现描述，更新错误处理策略表。
- `docs/08-testing/README.md`：登记 `ws_send_budget_test`。
