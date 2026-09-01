---
layout: home

hero:
  name: "Cabbage Documentation"
  text: "面向 AI Agent / 软件团队的项目文档管理系统"
  tagline: "把需求、架构、API、数据库、测试与发布文档变成可验证的工作流门禁"
  actions:
    - theme: brand
      text: 快速开始
      link: /00-overview/
    - theme: alt
      text: 查看架构
      link: /03-architecture/

features:
  - title: 工作流门禁 (Workflow Gates)
    details: 变更必须依次完成 PRD、影响分析、架构设计与测试用例，并在实现前通过验证。
  - title: 签名校验与防腐化 (Anti-Rot)
    details: 上游文档或影响范围变更后，下游阶段自动失效（stale），拒绝未完成的占位符。
  - title: 自动化同步与归档 (Sync & Archive)
    details: 变更完成合并后，自动化沉淀至 current-state 文档树并归档历史工作流。
---

## 工作流全景

```mermaid
flowchart LR
    Change[创建 change] --> Artifacts[完成阶段文档]
    Artifacts --> Gate[通过 Cabbage gate]
    Gate --> Implement[实现并验证]
    Implement --> Current[更新当前状态文档]
    Current --> CI[CI 与合并门禁]
```
