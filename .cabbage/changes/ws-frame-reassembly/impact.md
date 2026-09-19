---
change: ws-frame-reassembly
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

修复三个流式 WebSocket ASR 后端（OpenAI Realtime / Mistral Realtime / Volcengine）在单条服务端事件跨 TCP 分片到达时**丢弃已收前缀**的缺陷（GitHub issue #41）。三个后端的接收函数都把累积缓冲放在函数局部变量里并在每次调用开头 `clear()`，遇到 `CURLE_AGAIN` 直接返回并丢失已收字节，导致同一事件的后续片段被当作新事件解析，JSON 解析失败后被静默跳过。

影响链路：丢 `delta` → preedit 缺字跳字；丢 `completed` / `transcription.done` / 火山 final → 会话无法判定终态，只能等 30s 兜底才上屏（与 issue #44 的等待叠加）。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | No | 用户可见行为只在「大帧被分片」这一原本失败的路径上从「丢内容/等 30s」变为「正常出字」，正常路径无变化 |
| Architecture | Yes | 新增共享接收器 `src/addon/asr/utils/ws_frame_receiver.h`，三个后端改为消费同一实现（消除三处同构重复代码） |
| API | No | 无对外接口变化；`AsrSession`/`AsrEngine` 契约不变 |
| Database | No | |
| Security | No | 保留并统一了单条消息 16MB 上限与超限整条丢弃语义 |
| Testing | Yes | 新增 `tests/ws_frame_reassembly_test.cpp`（8 个接收器单测 + 2 个真实会话端到端分片用例），并纳入 `ctest` |
| Deployment | No | |
| Operations | No | 超限丢弃由原先的「返回 Error 断会话」改为「跳过该消息继续」，日志文案变化 |
| Data | No | |
| Performance | No | 与旧实现同为线性拷贝；仅新增一条消息级的 `clear()` 消除 |

# Impact Details

- **Architecture**: `ws_frame_receiver.h` 提供 `WsFrameReceiver`（消息级重组状态机）与 `ReceiveWsMessage()`（connect-only 连接上的收包 helper）。三处各自实现的 `ReceiveTextFrame` / `ReceiveWebSocketFrame` 及局部 `RecvStatus` 枚举被删除，改为调用共享实现，接收缓冲跨调用保留。
- **Testing**: 单测覆盖整流完成、跨片段拼接、`Again` 保留前缀、超限跳过且边界恢复、非目标类型过滤、WS 分片（`CURLWS_CONT`）、`CLOSE` 复位；端到端用本地假 WS 服务端把 24KB 的 Realtime delta 文本帧与火山二进制响应帧各按 4KB 分 6 次写出，断言真实 `RealtimeAsrSession` / `VolcengineAsrSession` 能拿到完整内容。
- **Operations**: 超限消息现在返回 `Skipped` 并由调用方 `continue`，不再被当成传输错误中断会话；错误日志仍打印实际上限值（经 `MaxMessageBytes()` 暴露，修正了原先无论配置如何都打印默认值的行为）。
- **Product**: issue #41 验收标准的第 3 条「帧超过 `kMaxFrameBytes` 时整帧丢弃且不破坏后续帧的解析边界」由 `TestOversizedMessageIsSkippedAndBoundaryRecovered` 覆盖。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| 消息级状态机在罕见的「分片消息中途切换类型」等协议违规输入下进入错误状态，反而吞掉后续合法消息 | Low | 单会话后续事件丢失 | 违规时退回 `Ignoring` 状态并在消息边界复位；`TestOtherMessageKindIsIgnored` / `TestFragmentedMessageAcrossFrames` 覆盖边界复位 | maintainer |
| 超限语义由「报错断会话」变为「跳过继续」，掩盖服务端异常返回巨型消息 | Low | 问题更难被发现 | `Skipped` 仍以 `FCITX_ERROR` 打印实际上限；调用方保留 `continue` 但不再触发重连 | maintainer |
| 三个后端行为一致但测试只覆盖 Realtime 与 Volcengine 两个端到端路径，Mistral 仅由共享实现的单测间接覆盖 | Low | Mistral 特有路径回归漏检 | Mistral 与 Realtime 是同构的 TEXT 接收（差异仅事件名）；接收器为共享实现，端到端二选一即可覆盖传输层 | maintainer |
| 新增测试依赖本地回环 socket 与 libcurl WS 支持 | Low | CI 环境不可用时测试失败 | CI 的 ubuntu-24.04 verify job 已构建 libcurl 8.x 并运行 `ctest`；测试用固定上限的轮询与 `Cancel()` 保证不挂起 | maintainer |

# Documentation Updates

- `docs/03-architecture/system-design/ARCHITECTURE.md`：在流式 WS 后端说明与错误处理策略中补充「事件分片重组」这一当前实现事实。
