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

## 约定

- 测试不依赖外部网络与真实 API Key：网络类用例在进程内起假服务端（回环端口），协议握手由测试自实现。
- 并发/线程用例必须有界：等待使用固定上限轮询，会话统一 `Cancel()` + `JoinWithTimeout`。
- 修复缺陷时遵循 RED → GREEN：先在未修复代码上运行同一用例确认失败，再实施修复并确认通过。
