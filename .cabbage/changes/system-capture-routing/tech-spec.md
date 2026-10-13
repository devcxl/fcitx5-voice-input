---
change: system-capture-routing
cabbage_stage: design
change_type: bugfix
---

# Context

不加设备下拉框，删除猜源；用户确认失焦保流但丢弃采样、取消识别，回焦复用流，明确切换/关闭释放。公开测试边界已确认。

# Design

- PulseAudio `dev=nullptr`，保留应用/流身份；原生 PipeWire 不指定 target。
- Engine 区分原始 FocusOut 与 Switch 事件；销毁活动上下文暂停，失焦保留独立采集归属引用，所属上下文无焦点明确关闭也释放，不响应其他上下文的切换/停用（即使原输入法也是 Voice Input）。组变更/退出全局释放。
- Pipeline 的 running 只代表识别，Pause 先关取帧门闸、停 VAD/dispatcher，再移交并取消会话、清队列。Stop 另释放 capture。恢复健康流，不健康则清理重建。
- Capture 的 epoch 拒绝跨暂停/恢复的在途读取；投递锁不与阻塞 read 共用。PipeWire 每个已完成 RT 回调（含空 buffer）归还所取 buffer 后发布 epoch，Drain 观察屏障再有界清 ring/组帧；不加 RT mutex。
- VAD 每次启动重置 Silero 状态。结果协调器及 UI generation 拒绝旧结果，LLM 取消沿用既有能力。

# Failure Modes

| Failure | Handling |
|---|---|
| 保流期间 read/error 导致后端死亡 | IsRunning 反映失败，回焦清理重建；明确关闭仍回收资源 |
| 快速 pause/resume、旧 RT 回调晚到 | epoch 门闸、生产者屏障及应用缓存清理 |
| 迟到转写/状态、输入框销毁 | 取消会话，主线程验证 generation/引用 |
| 系统默认/应用偏好不符 | 管理器选择，不恢复插件猜源 |
| PulseAudio 停止阻塞 | 先停止识别；独立后续任务，不 detach 或并发 free |

# Rollout

代码/测试与文档分开提交，分支 PR #55 验证 CI；不合并主分支、不安装。2026-10-13 用户确认 XFCE 双麦与音频管理器切换通过，其他桌面/未确认后端及完整生命周期现场验收仍待补充。详见 [ADR](../../../docs/03-architecture/adr/ADR-0004-system-managed-capture-routing.md)。

# Rollback

无配置或持久化迁移，整体回退本次采集/识别生命周期与路由修改即可；不保留旧猜源兼容路径。
