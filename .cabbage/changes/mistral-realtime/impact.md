---
change: mistral-realtime
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

修复 Mistral Realtime 握手错误被 End 取消覆盖、响应异常崩溃及文本收尾丢失。用户提供的日志只证明 HTTP 握手拒绝，尚不能证明其真实状态码、凭据或模型原因。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | No | 不增加产品能力 |
| Architecture | Yes | Mistral 协议验证与 worker 失败契约 |
| API | No | AsrSession 与配置公开契约不变 |
| Database | No | 无持久化变更 |
| Security | No | 沿用凭据隔离与安全日志约束 |
| Testing | Yes | 真实公开会话、本地 WS 故障注入 |
| Deployment | No | 不安装、无新依赖 |
| Operations | No | 无新运行流程 |
| Data | No | 无数据迁移 |
| Performance | No | 保持有界队列和预算 |

# Impact Details

Provider 编解码验证后输出独立事件；会话只管理时序、资源及文本。复用已有线程失败边界，不改 OpenAI/豆包及宿主 UI。新增测试复用既有测试握手/帧编码工具。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| 非官方网关字段不符 | Medium | 明确失败而不是隐式转换 | 官方消息类型与合法扩展回归 | 维护者 |
| 云端账户或模型拒绝 | Medium | 仍不能转写 | 状态码提示；用户真实环境复测 | 维护者/用户 |

# Documentation Updates

- ADR-0004、CRASH-ASR-001 跟进状态、测试说明。
