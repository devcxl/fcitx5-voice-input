# ADR-0002：输入框生命周期与结果归属分别检查

> 状态：已接受；日期：2026-10-09

历史崩溃栈位于延迟 `ClearUI → InputPanel::reset`；公开 activate/deactivate 回归确认，generation 仍有效时输入框也可能已销毁。用 Fcitx `watch()` 引用替代长期保存的裸指针，引用仅在主线程访问；跨线程回调只携带 generation 和值数据，主线程同时检查归属与存活。generation 不是生命周期锁。

停用事件已经位于主线程，所以同步提交/清空原输入框并立即解除引用，仅 pipeline 的停止保留 200ms 延迟。继续排队清空会丢失快速切换时的待提交文本，或把它提交给新输入框。销毁后无法提交的文本直接丢弃，不允许跨输入框继承。

不使用 Fcitx 5.1.8 才提供的 `scheduleWithContext`，以保留 Debian 12 等旧版宿主兼容性；已有 `watch()/get()` 足够。结果交付移入 `engine_results.cpp`，不新增 UI 层或依赖。验收由 `engine_ui_*` 公开边界测试覆盖，回滚可独立撤销本项修复；它不决定 WS 库选型，也不提供 native 故障的进程隔离。
