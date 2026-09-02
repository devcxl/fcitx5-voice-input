---
layout: home

hero:
  name: "fcitx5-voice-input"
  text: "Fcitx5 语音输入法插件"
  tagline: "PulseAudio/PipeWire 音频捕获 + Silero ONNX VAD 自动分段 + OpenAI 兼容 / 火山引擎 / Mistral 多后端 ASR"
  actions:
    - theme: brand
      text: 快速上手
      link: /00-overview/getting-started
    - theme: alt
      text: 安装指南
      link: /00-overview/installation
    - theme: alt
      text: 配置指南
      link: /00-overview/configuration
    - theme: alt
      text: 系统架构
      link: /03-architecture/system-design/ARCHITECTURE

features:
  - title: 自动分段与无感录音
    details: 切换输入法即自动捕获音频，Silero ONNX VAD 精准识别说话起止，静音后自动提交文本上屏，无需手动长按按键。
  - title: 丰富 ASR 后端支持
    details: 支持 OpenAI 兼容 API、阿里云百炼 DashScope、火山引擎豆包流式 ASR 以及 Mistral Realtime 等多种语音转写服务。
  - title: 实时增量与智能后处理
    details: 支持实时流式增量预览，并可选通过 LLM 大模型进行错别字修正、排版规范化与标点润色。
  - title: 极致轻量与高韧性
    details: 纯 Fcitx5 C++20 Addon 架构，无需独立守护进程；音频底层库采用 dlopen 延迟加载，系统升级平滑稳定。
---

## 文档目录

本站点是 fcitx5-voice-input 的完整官方文档（由 Cabbage 文档系统管理）。

| 分区 | 内容 |
|------|------|
| [00-overview 入门与概览](/00-overview/getting-started) | [快速入门](/00-overview/getting-started) · [安装指南](/00-overview/installation) · [详细配置](/00-overview/configuration) · [问题排查](/00-overview/troubleshooting) |
| [01-product 产品需求](/01-product/prd/product-prd-v1) | [产品级 PRD（现状基线）](/01-product/prd/product-prd-v1) · [PRD：输入法图标](/01-product/prd/add-input-method-icon) · [Out of Scope](/01-product/out-of-scope) |
| [03-architecture 系统架构](/03-architecture/system-design/ARCHITECTURE) | 系统架构、v4 会话模型设计、ADR 决策与架构评审 |
| [06-development 开发记录](/06-development/specs/gpt-realtime-asr) | 技术方案、任务分解与调研记录 |
| [11-ci-cd CI/CD 体系](/11-ci-cd/multi-distro-build-analysis) | 7 发行版容器矩阵与构建分析 |

## 文档工作流

所有变更遵循 Cabbage 生命周期门禁：PRD → Tech Spec → 任务 DAG → 实现 → 双轴评审 → 合并 → 归档。历史决策记录（ADR）不可改写，被新决策显式取代。
