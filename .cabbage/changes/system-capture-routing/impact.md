---
change: system-capture-routing
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

修复 #54 多麦克风猜源错误。用户批准设备由音频管理器选择，且失焦时保留录音流但不识别；2026-10-12 补登记实现及本地验证，远端验收尚在进行。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | Yes | 系统选设备；开启 Voice Input 后失焦保流，明确关闭释放 |
| Architecture | Yes | 采集与识别生命周期分离、投递 epoch、PipeWire 生产者屏障 |
| API | No | 无用户设置或外部协议变化；内部 Pipeline 新增 Pause |
| Database | No | 无数据库 |
| Security | No | 不改认证、凭据、TLS；已有 generation 隔离延伸到暂停，失焦停止处理 |
| Testing | Yes | 真实 Engine/Capture/Pipeline 边界与测试专用假音频库 |
| Deployment | No | 无新增依赖、安装、发布或打包策略变化 |
| Operations | No | 沿用音频管理器及现有后端诊断机制，无新服务 |
| Data | No | 不保存设备 ID，不迁移音频服务状态 |
| Performance | No | 非性能专项；RT 回调仍不锁、不分配 |

# Risks

- 持续麦克风使用指示是有意行为；失焦不新开识别，已上传请求不可撤回。
- 原有 PulseAudio read/join 阻塞问题仍由 CAPTURE-54-001 跟踪，不能承诺停止上界。
- 系统/应用默认可能指向虚拟源或 monitor，须检查输入和电平。
- 假库验证不代表真实双麦、桌面焦点/托盘和无缝移流；需现场验收。

# Documentation Updates

PRD R-19、架构组件文档、ADR-0004、使用/排障与测试清单已同步。证据及后续任务见 [调研报告](../../../docs/06-development/research/issue-54-audio-source-routing.md)。
