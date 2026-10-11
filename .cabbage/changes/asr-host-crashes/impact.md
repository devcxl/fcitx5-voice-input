---
change: asr-host-crashes
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

修复两项已复现的进程级崩溃：输入框销毁后 UI 回调访问悬空对象，以及 Realtime/豆包收到字段类型错误的合法 JSON 后异常逃逸 worker。本记录于 2026-10-09 补登记本轮诊断、设计与本地验收；远端 CI 尚需在推送后核实。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | No | 恢复既有输入/失败语义，无新功能或配置 |
| Architecture | Yes | UI 主线程引用与 generation 分离；provider 协议验证独立于会话编排；共用线程失败边界/资源所有权 |
| API | No | AsrSession/AsrEngine、用户配置与 addon 契约不变 |
| Database | No | 无数据库 |
| Security | No | 不变更认证/TLS/信任边界；仅防护现有外部响应路径，新增异常诊断不记录 what() 或凭据 |
| Testing | Yes | 真实引擎/会话公开边界回归，共享可停止的本地 WS 夹具 |
| Deployment | No | 不安装、不发布，无依赖/打包策略变化 |
| Operations | No | 现有错误回调与日志内新增失败类别，不改变运维流程 |
| Data | No | 无持久化迁移 |
| Performance | No | 不做性能专项优化，音频实时回调不变 |

# Risks

- 过严验证可能误拒兼容端点：保留未知扩展、可空 delta/error 字段、缺失 item_id 的旧 fallback 和豆包空确认帧；有正向回归。
- final callback 可能产生副作用后抛异常：标记后不重试，分别尝试错误通知/终态并完成 worker。
- Fcitx 引用不是线程安全的：只在主线程复制/读取，worker 仅排队值数据。
- Mistral 与构造/启动失败路径未在本轮修复，见 [后续任务](../../../docs/06-development/tasks/asr-crash-follow-ups.md)。native 故障的进程隔离与 curl WS 兼容性未解决。

# Documentation Updates

当前架构、ADR-0002/0003 与测试清单均已同步；core/用户配置只用于本地诊断，不提交或上传。
