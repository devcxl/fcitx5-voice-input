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

CI 中由 `ci.yml` 的 `verify` job 开启 `BUILD_TESTS` 并执行 `ctest`（多发行版 build 矩阵不跑测试，避免重复）。

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

## 约定

- 测试不依赖外部网络与真实 API Key：网络类用例在进程内起假服务端（回环端口），协议握手由测试自实现。
- 并发/线程用例必须有界：等待使用固定上限轮询，会话统一 `Cancel()` + `JoinWithTimeout`。
- 修复缺陷时遵循 RED → GREEN：先在未修复代码上运行同一用例确认失败，再实施修复并确认通过。
- 涉及「永久阻塞」类缺陷的用例显式设置 `ctest` `TIMEOUT`（`ws_frame_reassembly_test` 120s、`ws_send_budget_test` 90s）：回归时表现为超时失败，而不是挂住 CI。

## 平台限制（已实测）

流式 WS 后端需要 libcurl 编译时启用 WebSocket。实测：

| 发行版 | 系统 libcurl | `ws`/`wss` 协议 |
|--------|-------------|----------------|
| Ubuntu 24.04 | 8.5.0 | 不可用 |
| Debian 12 | 7.88 | 不可用 |
| Ubuntu 26.04 / Debian 13 / Fedora 44 / openSUSE Tumbleweed | 8.x 新版 | 可用 |

`ws_frame_reassembly_test` 通过 `curl_version_info()` 探测 `ws` 协议：不具备时打印 `SKIP` 并跳过协议级用例（接收器状态机单测仍然执行），避免把平台限制误报为回归。CI 中新增 `tests` job（容器 `ubuntu-26.04`）保证协议级用例在具备 WS 的环境中真实执行。
