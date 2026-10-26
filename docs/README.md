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
    details: 支持 OpenAI 兼容 API（含阿里云百炼 DashScope、小米 MiMo）、火山引擎豆包流式 ASR 以及 Mistral Realtime 等多种语音转写服务。
  - title: 实时增量与智能后处理
    details: 支持实时流式增量预览，并可选通过 LLM 大模型进行错别字修正、排版规范化与标点润色。
  - title: 极致轻量与高韧性
    details: 纯 Fcitx5 C++20 Addon 架构，无需独立守护进程；音频底层库采用 dlopen 延迟加载，系统升级平滑稳定。
---

## 文档目录

本站点是 fcitx5-voice-input 的完整官方文档。

| 分区 | 内容 |
|------|------|
| [00-overview 入门与概览](/00-overview/) | [快速入门](/00-overview/getting-started) · [安装指南](/00-overview/installation) · [详细配置](/00-overview/configuration) · [问题排查](/00-overview/troubleshooting) |
| [01-product 产品需求](/01-product/prd/product-prd-v1) | [产品级 PRD（现状基线）](/01-product/prd/product-prd-v1) · [PRD：输入法图标](/01-product/prd/add-input-method-icon) · [Out of Scope](/01-product/out-of-scope) |
| [03-architecture 系统架构](/03-architecture/system-design/ARCHITECTURE) | 系统架构、v4 会话模型设计、ADR 决策与架构评审 |
| [06-development 开发](/06-development/guide) | [开发指南](/06-development/guide) · 技术方案、任务分解与调研记录 |
| [08-testing 测试](/08-testing/) | 测试运行方式与测试清单 |
| [11-ci-cd CI/CD 体系](/11-ci-cd/multi-distro-build-analysis) | 7 发行版容器矩阵与构建分析 |
| [13-operations 运维](/13-operations/troubleshooting) | 运行期问题排查 |

## 文档维护

- `ARCHITECTURE.md` 与代码同步更新，Route Map 中未实现的功能保持「待实现」标记。
- ADR 记录不可改写的历史决策，被新决策显式取代时在新 ADR 中注明。
- 文档站由 VitePress 构建，源码在 `docs/`，推送至主分支后由 `docs-deploy.yml` 自动部署到 GitHub Pages。
