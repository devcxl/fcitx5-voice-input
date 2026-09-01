---
layout: home

hero:
  name: "fcitx5-voice-input"
  text: "Fcitx5 语音输入法插件"
  tagline: "PulseAudio/PipeWire 音频捕获 + Silero ONNX VAD 分段 + OpenAI 兼容 / Volcengine / Mistral Realtime 多后端 ASR"
  actions:
    - theme: brand
      text: 系统架构
      link: /03-architecture/
    - theme: alt
      text: 产品需求
      link: /01-product/
    - theme: alt
      text: CI/CD 分析
      link: /11-ci-cd/

features:
  - title: 语音输入
    details: 切换输入法即开始录音，VAD 自动分段，静音后提交 ASR 结果上屏，无需按键。
  - title: 多 ASR 后端
    details: OpenAI 兼容 API（whisper-1 / chat / realtime）、Volcengine Doubao 流式、Mistral Realtime 流式转写。
  - title: 纯 Addon 架构
    details: 无 daemon、无 CLI、无 Qt，全部逻辑运行在 Fcitx5 进程内的三个工作线程中。
---

## 文档目录

本站点是 fcitx5-voice-input 的项目文档（由 Cabbage 文档系统管理）。

| 分区 | 内容 |
|------|------|
| [00-overview](/00-overview/) | 项目概览与现状 |
| [01-product](/01-product/) | PRD、范围排除记录 |
| [03-architecture](/03-architecture/) | 系统架构、会话模型设计、ADR、架构评审 |
| [06-development](/06-development/) | 技术方案、任务分解、调研记录 |
| [11-ci-cd](/11-ci-cd/) | 多发行版构建分析 |

## 文档工作流

所有变更遵循 Cabbage 生命周期门禁：PRD → Tech Spec → 任务 DAG → 实现 → 双轴评审 → 合并 → 归档。历史决策记录（ADR）不可改写，被新决策显式取代。