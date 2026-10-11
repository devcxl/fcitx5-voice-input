---
change: asr-host-crashes
cabbage_stage: implementation
change_type: bugfix
---

# Tasks

- [x] 从公开边界复现 UI 销毁崩溃与两种 WS 字段异常崩溃，区别历史 core 根因与额外缺陷。
- [x] 补 UI 回归并修复主线程引用、generation、同步停用清理与 pending 文本归属。
- [x] 行为保持地拆分 provider 协议模块，所有本次修改的代码模块不超过 500 行。
- [x] 提取既有 WS 测试夹具，收发/停止有界、投递后保持连接、join 后回收晚到 accept。
- [x] 为真实 Realtime/豆包会话补非法响应与 callback 异常回归，红阶段确认 SIGABRT。
- [x] 加入已知字段验证、资源所有权、worker 异常边界与一次终态；保留合法可空字段与空确认帧。
- [x] 本地 Release 和 ASan/UBSan 全套各 28/28 通过；独立暂存树 22/22 通过；UI 连续 10 轮通过。
- [x] 同步架构、ADR 与测试现状，将其他后端/启动路径记录为独立后续任务，不扩大实现范围。

# Delivery Boundary

以上勾选仅表示已执行并验证的本地工作。Cabbage 记录补登记后验证 merge gate，再推送分支、创建草稿 PR、检查全部远端 CI；不将尚未触发的 CI 写为成功，也不合并主分支或安装构建产物。
