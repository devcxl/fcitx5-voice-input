---
change: system-capture-routing
cabbage_stage: implementation
change_type: bugfix
---

# Tasks

- [x] 根因与路由机制调研，用户确认失焦保流但暂停识别及公开测试边界。
- [x] 默认路由测试先红后绿，删除猜源与 pactl 子进程。
- [x] 分离 Pipeline 采集和识别、epoch 投递及旧缓存清理，生命周期先红后绿。
- [x] PipeWire 生产者屏障与有界清理、错误 liveness/资源回收，旧实现回归先红后绿。
- [x] Engine 失焦/销毁暂停、明确关闭释放、迟到 UI 过滤、无焦点及其他上下文边界。
- [x] 独立复核后补采集归属引用、其他 Voice Input 上下文边界、空 PipeWire buffer 确认出口，新增回归先红后绿。
- [x] 本地完整构建/38 tests、10 项核心回归重复 20 轮、无 PipeWire 构建及链接卫生验证。
- [x] 同步 PRD、架构/ADR、测试、使用/排障；真实设备验收和旧同步停止债务明确保留。

# Delivery Boundary

勾选表示已执行的本地验证。PR #55 的 f894fdd 已通过远端七发行版构建及全部适用检查（Pages 按 PR 条件跳过）。2026-10-13 用户确认自己的 XFCE 电脑双麦与音频管理器切换通过；其他桌面/未确认后端及完整生命周期现场验收不标为完成，不宣称已合并或由代理安装。
