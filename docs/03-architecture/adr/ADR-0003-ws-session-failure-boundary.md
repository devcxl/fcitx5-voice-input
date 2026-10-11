# ADR-0003：WS 协议验证与会话线程失败边界

> 状态：已接受；日期：2026-10-09

真实 Realtime/豆包会话收到字段类型错误的合法 JSON 时，JsonCpp 转换异常越过线程入口，触发 `std::terminate` 并中止整个 Fcitx5。仅换 WS 库不会消除这个应用协议缺陷。本项保持 curl、配置和 `AsrSession` 公开契约不变，不取代 ADR-0001 的库选型。

协议编解码留在各 provider 的无会话状态模块：先验证根对象、已知字段的类型/数值范围和嵌套结果深度，再转换为不含 JsonCpp 对象的识别事件。可选字段缺失保留原有默认行为，未知事件/扩展字段允许忽略；已知事件的错误字段和非法 gzip 则立即使会话失败，报告固定格式错误并发布一个空 final，不重连、不提交此前 partial。正常 Realtime `error` 事件仍按 issue #44 的契约通知但不推进 commit 计数或结束会话。

两个会话复用最外层线程异常边界与 curl 资源所有权，协议状态机不合并。异常路径释放 curl/header/zlib 资源，独立尝试错误通知和空 final，保证 `finished`、`workerDone` 可观测且能够 join；取消不另发失败终态。final 在调用 callback 前标记，callback 可能已经产生副作用再抛异常，所以不能重试 final。异常诊断只记录类别和 sessionId，不记录 `what()`、原始 JSON、语音或凭据；错误 callback 自身抛异常也不能中止宿主。

取舍：错误类型不再被 JsonCpp 隐式转换或静默跳过，第三方兼容端点若发送不符合已知事件契约的数据会明确失败。根因回归使用真实 `AsrSession` 与共享的本地假 WS 服务端，断言一次错误/一次空 final、未传播非法文本、worker 自行完成（不先 Cancel 掩盖未收敛），并由 sanitizer/现有分片、commit、发送预算测试验证。协议模块先做行为保持的抽取，随后两种 provider 分别修复，方便独立审查和回滚。

空的未压缩豆包确认帧保留原有忽略行为，不当作非法 JSON；它后面的正常转写仍须交付。`ws_worker_volcengine_empty-ack` 曾在初版修复上失败，恢复该契约后通过。OpenAI 可空字段依据官方生成类型 [openai-python@8e1fd258](https://github.com/openai/openai-python/tree/8e1fd2587deae364367e791385997ab45aa2a520/src/openai/types/realtime) 核实。

本项不为任意 native 内存故障提供进程隔离，也不声称旧发行版 curl 获得了 WS 能力；库迁移与其他后端的协议审查仍是单独范围。错误 callback 的防逃逸保证限于 worker，构造阶段的旧错误通知不在本轮修改范围内。Mistral 和构造/启动路径分别由 [CRASH-ASR-001/002](../../06-development/tasks/asr-crash-follow-ups.md) 追踪。
