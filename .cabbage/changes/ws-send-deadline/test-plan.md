---
change: ws-send-deadline
cabbage_stage: tests
change_type: bugfix
---

# Strategy

缺陷的可观测行为有两层：`SendWsMessage` 的预算语义（纯发送循环），以及真实会话在「上游不读 socket」下的 End 收敛性（跨线程：pipeline 线程 `End()` + worker 线程发送/建连）。测试分别以这两个 seam 覆盖，并额外保留一条协议级前置条件检查（libcurl 是否具备 WS）。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| Unit | 预算到期即失败（不再无限重试）；取消优先中止 | `SendWsMessage` | maintainer |
| Integration (session) | 三个真实会话在「上游不读」下 `End()` 后 worker 有界结束、产出终态、可被 `JoinWithTimeout` 回收 | `AsrSession::End/JoinWithTimeout` + `WaitForWorkerCompletion` | maintainer |
| Platform precondition | 无 WS 支持的 libcurl 上跳过协议级用例 | `curl_version_info()` | maintainer |

不覆盖：半死连接的空闲检测（本变更未实现）、保序缓冲的跳过策略（issue #43）、`commitsInFlight` 判定与 30s 终态窗口（issue #44）。

# Test Environment and Data

- 环境：本地回环 TCP，无需外部网络与真实 API Key。
- 假服务端 `NonReadingWsServer`：完成 RFC6455 握手（内置精简 SHA1 + Base64）后**不再读取 socket**，并把客户端接收缓冲收紧到 4096 字节，使发送方迅速遇到 TCP 窗口关闭。
- 数据：单测发送 8MB TEXT 负载（远超窗口，保证进入 `CURLE_AGAIN` 分支）；会话用例每段喂 40 × 1600 帧（约 640ms 音频）以驱动真实推流。
- 隔离与清理：每用例独立服务端，结束时关闭客户端连接并 join 服务端线程；会话统一 `JoinWithTimeout(2s)`。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | 向不读 socket 的对端发送 8MB，预算 1500ms | Unit | 返回 false，耗时 ≈1505ms（≥1500ms 且 <6000ms），不无限重试 | High |
| T-2 | `cancelFlag` 已置位时发送 | Unit | 立即返回 false，不发起发送 | High |
| T-3 | 真实 `RealtimeAsrSession` 推流中 `End()`，上游不读 | E2E | worker 在 14s 内结束、产出 final、`JoinWithTimeout` 不超时（实测 3ms） | High |
| T-4 | 真实 `MistralAsrSession` 同上 | E2E | 同上（实测 3ms） | High |
| T-5 | 真实 `VolcengineAsrSession` 同上 | E2E | 同上（实测 8034ms ≈ End 预算 8s，仍远低于 15s） | High |

# Regression Coverage

- 既有 7 个测试继续通过（`ctest` 共 8 个）：`session_worker_completion_test`、`audio_frame_assembler_test`、`llm_request_cancellation_test`、`asr_ordering_test`、`result_coordinator_test`、`realtime_terminal_state_test`、`ws_frame_reassembly_test`。
- 正常路径不受影响：未武装 End 预算时 `WsEndBudget::Deadline()` 返回一份全新默认预算，推流发送行为与旧的单次 10s 语义一致；`CancelOrFinished` 在 `finished` 未置位时等价于仅检查 `cancelled`。
- 缺陷回归时的判定方式：发送循环去掉截止时间后，T-1 与 T-3~T-5 会**挂死**而非失败，因此 `ws_send_budget_test` 设置 `ctest` `TIMEOUT 90`，使回归以失败而非挂起结束（已实测：突变后运行 400s 未退出）。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| Resilience | 上游不读 socket 的三个后端 End 收敛用例 | worker 在 14s 内结束；`JoinWithTimeout(2s)` 不超时；不出现 detach 告警 |
| Liveness / 无泄漏 | `WaitForWorkerCompletion(workerDone, 14s)` | 返回 true（证明 worker 真正退出，而非被超时 detach） |
| 端到端时延 | End → worker 结束 | Realtime/Mistral 毫秒级；Volcengine ≈ End 预算 8s（仍 < 15s reaper 上限） |
| 终止语义 | 终态回调 | 三个后端在卡死场景均产出一次 final（`publishFinal` 或 `cb(..., true, sid)`） |

# Entry and Exit Criteria

- Entry: 能构造「握手后不读 socket」的对端，并驱动真实会话推流。
- Exit: `ctest --test-dir build --output-on-failure` 8/8 通过；突变验证确认测试能捕获「无截止时间」缺陷（挂死 → 超时失败）。

# Risks

- 端到端用例依赖 WS 支持与回环 socket：无 WS 的 libcurl（Ubuntu 24.04 / Debian 12）上协议级用例打印 `SKIP` 并跳过，单测（T-1/T-2）仍执行。
- 用例含固定等待（每段喂音频 10ms × 40 + 收敛观察），在极慢 CI 上可能逼近阈值；阈值取 14s（reaper 15s）与 90s（ctest 超时）留出充分余量。
- 收敛时间依赖 End 预算常量：若后续调整 `WsEndBudget::kDefaultTotal`，需同步复核本测试的 14s 断言（8s + 余量仍成立即通过）。
- `ctest` 超时（90s）同时充当「测试自身不得挂起」的兜底：若实现对某条路径意外阻塞，测试以超时失败（而非卡住 CI）结束。
