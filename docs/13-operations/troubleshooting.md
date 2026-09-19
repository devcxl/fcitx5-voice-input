# 运行与故障排查（WS 流式后端）

> 状态：当前实现文档。适用范围：使用 OpenAI Realtime / Mistral Realtime / 火山引擎豆包（WS 流式）后端的场景。

## 关键日志信号

日志由 fcitx5 输出（默认 INFO 级；`FCITX_LOG_RULES` 可提升到 DEBUG）。转写内容属敏感信息，日志只记录长度与状态，不记录文本。

| 日志 | 级别 | 含义 | 处置 |
|------|------|------|------|
| `[voice-input:<backend>] WS connected session=N` | INFO | WS 会话已建立 | 正常 |
| `[voice-input:<backend>] WS recv error` | ERROR | 底层接收错误（非分片），随后可能重连 | 观察是否反复出现 |
| `[voice-input:realtime] server error session=N: <message>` | WARN | 服务端返回 `error` 事件（如对空 buffer 的 `input_audio_buffer_commit_empty`、鉴权/配额问题） | 按 message 内容处置；不影响最终结果判定（不产生转写 item） |
| `[voice-input] ASR error: <message>` | ERROR | 引擎错误回调汇总（含上面的服务端 error） | 同上 |
| `[voice-input:<backend>] WS message exceeds N bytes, dropped` | ERROR | 服务端单条消息超过 16MB，被整条丢弃且不破坏后续解析边界 | 正常防护；持续出现说明上游异常 |
| `[voice-input:<backend>] WS send timeout after Nms (M bytes pending): peer is not reading` | ERROR | 上游不读 socket（卡死/限流/反代 hang），发送在预算内失败 | 查看下面的「上游不读 socket」一节 |
| `[voice-input:<backend>] WS send stalled (M bytes pending)` | ERROR | `curl_ws_send` 未报错但零进展，同样按预算失败 | 同上 |
| `[voice-input:<backend>] End reached, skipping reconnect session=N` | WARN | 语音已结束（或 End 预算耗尽），不再重连，直接以已累积文本兜底上屏 | 正常收敛路径 |
| `[voice-input:<backend>] End wait timeout session=N` | WARN | End 等待窗口内未收到服务端终态，已用已累积文本兜底上屏 | 上游响应慢；结果不丢，仅可能少最后一段增量 |
| `[voice-input:<backend>] Join timeout session=N` | WARN | worker 未在 15s 内结束，已 detach（线程泄漏信号） | 不应出现；出现则按缺陷上报 |
| 状态栏出现「语音识别失败」 | UI | 某段结果为空或超时（含保序闸门 40s 停滞放行） | 该段被跳过，后续段照常上屏；持续出现请检查上游 |
| `[voice-input] ASR error: <...>` | ERROR | 引擎上报的错误（认证失败、服务端拒绝等） | 检查配置与上游状态 |

## 上游不读 socket（会话卡死）

### 现象

- 输入法像「卡住/变慢」，某段语音没有结果；
- 极端情况下后续语音段结果也不上屏（会话未产出终态，保序闸门被扣住）；
- 日志出现 `WS send timeout ... peer is not reading` 与 `End reached, skipping reconnect`。

### 背景（为什么不再无限等待）

libcurl 的 `CURLOPT_TIMEOUT` 只约束传输阶段；`CONNECT_ONLY` 建立后的 WS 收发不受其约束。因此发送路径由实现侧限时：

- 单次发送预算：`WsDeadline::kDefaultBudget`（10s）；
- End 路径总预算：`WsEndBudget::kDefaultTotal`（8s），覆盖尾部音频 flush、commit/end、等待服务端终态与可能的重连；
- 两者都明显小于 `SessionReaper` 的 `JoinWithTimeout`（15s），保证 worker 能被正常回收（不 detach、不泄漏）。

End 之后不再重连：此时已无新音频可发，重连只会再阻塞一轮，因此直接以已累积文本交出终态（`End reached, skipping reconnect`）。

### 处置

1. 确认网络到上游的连通性与稳定性（代理/网关是否 hang、是否有连接数限流）。
2. 若上游持续不可用：切换后端（配置界面 Active Backend），或在 OpenAI 配置下改用非流式 `whisper`/`chat` 模式。
3. 若日志反复出现 `Join timeout session=N`：这是实现缺陷信号（worker 未在 15s 内退出），请附日志上报。

## 平台限制：libcurl 需要 WebSocket 支持

流式 WS 后端依赖 libcurl 启用 WebSocket。实测各发行版系统 libcurl：

| 发行版 | 系统 libcurl | `ws`/`wss` | 流式 WS 后端 |
|--------|-------------|-----------|-------------|
| Ubuntu 24.04 | 8.5.0 | 不可用 | 不可用 |
| Debian 12 | 7.88 | 不可用 | 不可用 |
| Ubuntu 26.04 / Debian 13 / Fedora 44 / openSUSE Tumbleweed | 8.x 新版 | 可用 | 可用 |

自查当前系统：

```bash
curl -V | grep -o 'Protocols:.*' | tr ' ' '\n' | grep -x 'ws\|wss'
```

无输出说明当前 libcurl 不支持 WS：请在该平台使用非流式后端，或改用支持 WS 的发行版/libcurl 构建。

## 某段语音卡住导致后续结果不上屏

### 现象

- 某段语音说完后没有结果，且**后续语音段也不上屏**（观感是「输入法卡住了」）；
- 等待约 40s 后，后续段的结果会成批出现，卡住的那段显示「语音识别失败」。

### 背景（保序闸门与停滞放行）

结果按语音段创建顺序保序上屏（`OrderedResultBuffer`），因此某段没有终态时，后续段必须等待。为避免「单段故障 = 整链路静默」，闸门提供**停滞放行**（默认 40s）：某段长时间无进展（或阻塞后续段超过 40s）时，该段以超时错误放行，后续段立即按序上屏。

40s 的取值依据：非流式 LLM 后处理的最长等待为 30s（`LLMClient` 的 `CURLOPT_TIMEOUT`），加调度余量；ASR 侧 End 路径已有 8s 上界（见上节），因此 40s 不会误伤正常慢路径。

### 最终结果为什么不再固定等 30s

Realtime 后端此前在 End 后固定等待 30s 才收尾（即使服务端早已返回最终结果）。现在：

- 终态判定按服务端确认的 `item_id` 追踪在途 item——若某次 commit 被服务端以 `error` 拒绝（例如空 buffer），不会造成计数漂移，最终 `completed` 到达即立即上屏（实测 161~192ms）；
- End 等待的上界是「距最后一个服务端事件 5s」的空闲超时，并与 End 总预算（8s）、reaper join（15s）形成三重上界。

### 处置

1. 出现该现象说明某段会话确实没有交出终态，请对照上一节的发送超时/接收错误日志定位上游行为。
2. 结果不会丢：后续段最终都会上屏；仅卡住的那段被标记为失败。
3. 若频繁出现，按「上游不读 socket」一节的步骤排查网络与上游限流。

## 相关文档

- `docs/03-architecture/system-design/ARCHITECTURE.md` — 当前实现（线程模型、错误处理策略、构建选项）
- `docs/00-overview/troubleshooting.md` — 面向安装与配置的常见问题
- `docs/08-testing/README.md` — 测试清单与运行方式
