# ADR-0004：Mistral Realtime 握手诊断与失败边界

> 状态：已接受；日期：2026-10-11

用户日志显示 WS 握手 HTTP 拒绝两次后，End 触发的进度回调中止覆盖原始错误。旧日志未保存 HTTP 状态码，不能推断该账户的具体拒绝原因。默认地址和模型与[官方文档](https://docs.mistral.ai/studio/audio/speech_to_text/realtime_transcription)一致；不擅自换模型或迁移 WS 库。

Mistral 会话读取 `CURLINFO_RESPONSE_CODE`，错误只输出状态码、固定处理提示及 sessionId/attempt。3xx/4xx（408/429 除外）属于不可通过原样重试解决的拒绝，立即空 final；暂态连接失败至多三次且重试等待可取消。End 不开启新连接，也不覆盖之前的握手失败。库 API 已核对 curl 8.22.0 的实现及官方文档，不提升现有最低版本。

应用协议依据 [client-python@6229ed96](https://github.com/mistralai/client-python/tree/6229ed96cb5bfcfc9068a11dee5ff357697a8ad9/src/mistralai/extra/realtime) 与该提交 `client/models`：先等待 `session.created` 再配置/推流，End 发余音频、flush、end，共用已有预算。delta 是段增量；segment 确认该段而不是追加一份 delta；done.text 是当前连接的权威全文，不能用 substring 判断是否需要追加。连接关闭或 End 超时仍交付已累积的合法文本。

Provider 独立编解码先验证后转换，JsonCpp 对象不进入会话状态。error.message 官方允许字符串或字典，error.code 为整数；不把未知字典内容当成文本，更不将可能反射凭据的服务端 message 回显到日志/错误响应。固定错误与数值 code 保留安全的诊断上下文；未知事件/扩展忽略。协议非法为空 final，不提交之前的 partial。

线程入口复用 ADR-0003 的 `RunAsrWorker` 与 curl/header RAII。final 在调用 callback 前标记，异常不重试副作用；取消不产失败 final。Mistral 的终态标志留在 provider 内，不为其引入 OpenAI 的 item/commit 语义。回归通过真实公开会话+本地 WS 验证，而非仅测解析器；非读 socket 夹具先发送 created，避免创建超时掩盖实际发送背压。

取舍与回滚：不修 CRASH-ASR-002（构造/启动失败）或旧 curl WS 能力；本地测试不证明用户真实账户能转写，需要更新后按新状态码复测。可独立回滚 Mistral 生产代码与对应测试，不撤销公共失败边界，无数据迁移。
