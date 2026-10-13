---
change: system-capture-routing
cabbage_stage: tests
change_type: bugfix
---

# Strategy

真实 Engine activate/deactivate 和 Capture/Pipeline Start/Pause/Stop 边界；仅替换外部音频库、模型/识别边界，不访问私有状态。假库只在构建目录，UI 独立 XDG 路径，无真实录音、云端或安装。

# Cases

| Scenario | Expected / Observed |
|---|---|
| 内置源排前、USB 排后 | 旧实现显式内置且调用 pactl；新实现 dev=nullptr、无 pactl、身份格式不变 |
| 失焦超过旧 200ms | 旧实现 Stop；新实现保流但暂停识别、迟到 UI 不提交 |
| 暂停期间持续采样 | 未接门闸时入队/回放；新实现持续消费但不入识别 |
| read/RT callback 跨快速暂停恢复 | 旧音频被拒绝；PipeWire 基线旧样本泄漏，新实现清理 |
| 回焦与保流期间后端死亡 | 健康流不 reopen；死亡流回收重建、恢复新音频 |
| 无焦点切换、死亡流关闭、上下文销毁 | 明确关闭释放；活动上下文销毁只暂停；其他上下文切换/停用 Voice Input 先红后绿 |
| 四种空 PipeWire buffer 后仅到一帧有效音频 | 旧确认出口遗漏导致首帧丢失；所有完成回调确认 epoch 后首帧正常投递 |

# Verification

- Release 完整构建与 38 项 ctest 全通过；10 项核心生命周期测试重复 20 轮通过。
- 屏蔽 PipeWire pkg-config 的可选构建与相关测试通过。
- nm/readelf 无录音库直接未定义符号与 DT_NEEDED；旧 SDK 不使用 pw_buffer.time。
- 所有修改/新增文件小于 500 行，文档相对链接与 git diff --check 通过。

# Limits

2026-10-13 用户在自己的 XFCE 电脑上确认双麦与音频管理器设备切换通过，接受基本可用性。音频服务、实际采集后端和设备型号未提供；其他桌面/后端以及失焦、恢复、关闭的逐项现场结果仍未确认，麦克风硬件指示也未报告。已上传请求不可撤回，不承诺服务端采样级延迟或异常同步停止上界。远端 CI 在推送后另行确认。
