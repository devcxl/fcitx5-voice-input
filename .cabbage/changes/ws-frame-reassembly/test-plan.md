---
change: ws-frame-reassembly
cabbage_stage: tests
change_type: bugfix
---

# Strategy

修复对象是「接收路径的状态机」，其可观测行为分两层：接收器本身的分片/边界语义，以及真实会话在传输层分片下的端到端结果。因此测试采用两级 seam：`WsFrameReceiver::OnChunk`（纯函数式，无 I/O）与 `AsrSession` 的 result callback（真实 socket + 真实 libcurl + 真实会话代码）。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| Unit | 接收器的拼接、保持、超限、类型过滤、WS 分片、CLOSE 复位 | `WsFrameReceiver::OnChunk` / `Bytes()` | maintainer |
| Integration | 真实 `curl_ws_recv` 在分片投递下的消息完整性 | `ReceiveWsMessage(curl, receiver, tag)` | maintainer |
| E2E (session) | `RealtimeAsrSession` 分片 TEXT 事件 → partial 回调；`VolcengineAsrSession` 分片 BINARY 响应 → partial 回调 | `AsrSession::SetResultCallback` | maintainer |

不覆盖：Mistral 的端到端路径（与 Realtime 同构的 TEXT 接收，仅事件名不同；共享实现由上述两级覆盖）。理由记入 `# Risks`。

# Test Environment and Data

- 环境：本地回环 TCP（`127.0.0.1`，端口由内核分配）。无需外部网络、无需 API Key（测试用占位值）。
- 假服务端：`FakeWsServer` 在测试进程内完成 RFC6455 握手（内置精简 SHA1 + Base64 计算 `Sec-WebSocket-Accept`），随后把预置的原始 WS 帧字节按固定 write size 与间隔写出，用于稳定制造跨 TCP 分段。
- 数据：Realtime 用例构造 24000 字符 `delta` 的 `conversation.item.input_audio_transcription.delta` 事件（编码后约 24KB）；Volcengine 用例构造 `{"code":20000000,"result":{"text":"<24000 字符>"}}` 的 bigmodel 响应帧（带 sequence 的 full server response）。
- 隔离与清理：每个用例自建/自毁服务端（`Stop()` 关闭监听 fd 并 join）；会话统一 `Cancel()` + `JoinWithTimeout(3s)`，避免悬挂线程。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | 一条完整消息在一个片段内到达 | Unit | `Ok`，内容与原文一致 | High |
| T-2 | 同一条消息分 3 段喂入（`bytesleft > 0`） | Unit | 前两段返回 `Again`，末段返回 `Ok`，累计 100 字节 | High |
| T-3 | 24071 字节消息按 4092 字节分 6 段，段间返回 `Again` | Unit | 前缀不丢，末段 `Ok` 且内容逐字节相同 | High |
| T-4 | 单条消息超过上限（上限 10 字节，消息 25 字节） | Unit | 越过边界时返回一次 `Skipped`；随后 4 字节消息正常 `Ok` 且内容正确 | High |
| T-5 | 非目标类型帧（TEXT 接收器收到 BINARY）后再来目标消息 | Unit | 非目标帧返回 `Again` 且字节不泄漏；目标消息 `Ok` 且内容正确 | Medium |
| T-6 | WS 分片消息（`CURLWS_CONT` 两部分 + FIN 结尾） | Unit | 仅最后一段返回 `Ok`，内容为三段拼接 | Medium |
| T-7 | 采集半条消息后收到 `CURLWS_CLOSE` | Unit | 返回 `Closed` 且缓冲被复位（无残留前缀） | Medium |
| T-8 | 真实 socket 上 24KB TEXT 消息按 4KB 分片投递 | Integration | `ReceiveWsMessage` 返回一次 `Ok`，消息长度 > 24KB 且以帧首前缀开头 | High |
| T-9 | 真实 `RealtimeAsrSession` 连接假服务端，分片投递 24KB `delta` | E2E | 观察到 `isFinal=false` 回调且文本长度达到 24000 | High |
| T-10 | 真实 `VolcengineAsrSession` 连接假服务端，分片投递 24KB 二进制响应帧 | E2E | 观察到 `isFinal=false` 回调且文本长度达到 24000 | High |

# Regression Coverage

- 既有 6 个测试必须继续通过：`session_worker_completion_test`、`audio_frame_assembler_test`、`llm_request_cancellation_test`、`asr_ordering_test`、`result_coordinator_test`、`realtime_terminal_state_test`（保序、会话回收、终态判定语义不受本变更影响）。
- `tests/ws_frame_reassembly_test.cpp` 纳入 `ctest`（`add_test(NAME ws_frame_reassembly_test ...)`），由 CI 的 verify job（`BUILD_TESTS=true` → `ctest`）执行。
- 变更前后对照：在未修复代码上运行同一测试，T-3/T-4/T-8/T-9/T-10 失败（前缀被丢弃、超限返回 Error 而非 Skipped），修复后全部通过。RED 证据见 `tasks.md`。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| Resilience | 分片投递 + 超限消息 + CLOSE/传输错误的组合用例 | 任何路径都不得无限阻塞；每用例 3s 内 `JoinWithTimeout` 回收会话 |
| Memory safety | 超限消息不下沉到缓冲（`Discarding` 状态吞片） | 缓冲不随超限消息增长；上限可配置并在测试中用 10 字节验证 |
| Latency | 分段间隔 120ms、6 段（约 720ms） | partial 回调在 10s 观察窗口内到达（远大于传输耗时，避免 flaky） |

# Entry and Exit Criteria

- Entry: 能从 issue #41 的复现步骤出发，在本机以假服务端触发分片投递。
- Exit: `ctest --test-dir build --output-on-failure` 全部通过（7 个可执行测试，含新增 10 个用例）；未修复代码上对应用例失败（RED→GREEN 证据记录于 `tasks.md`）。

# Risks

- **平台限制（已实测）**：Ubuntu 24.04 的 libcurl 8.5 与 Debian 12 的 7.88 在编译时未启用 WebSocket（`curl -V` 的 `Protocols` 无 `ws`/`wss`），这些平台上流式 WS 后端本身不可用。测试的协议级用例通过 `curl_version_info()` 探测 `ws` 协议：不具备时输出 `SKIP` 并跳过，避免把平台限制误报为回归。四个发行版（ubuntu-26.04 / debian-13 / fedora-44 / opensuse-tumbleweed）已验证具备 WS。
- CI 中新增 `.github/actions/tests` 与新 job `tests`（容器 `ubuntu:26.04`），保证协议级用例在具备 WS 的环境中真实执行；`verify` job 因运行在无 WS 的 Ubuntu 24.04 上而不会执行分片端到端用例。
- 端到端用例依赖本地回环 socket。CI 使用发行版 libcurl；若未来某个发行版去掉 WS 支持，相关用例会跳过而非静默失败——此时需根据平台策略决定是补充依赖还是接受降级。
- Mistral 端到端未单独覆盖：其接收路径与 Realtime 同为 TEXT 且已改用同一实现，重复搭建一套假服务端不增加判别力。若后续 Mistral 事件解析逻辑变动，需在 `mistral_asr.cpp` 相关改动中补充用例。
- 假服务端只验证服务端到客户端方向；客户端到服务端的发送路径由 issue #42 单独修复与测试。
