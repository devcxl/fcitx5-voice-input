import { defineConfig } from 'vitepress'
import { withMermaid } from 'vitepress-plugin-mermaid'

const base = (process.env.BASE_URL || (process.env.GITHUB_REPOSITORY ? `/${process.env.GITHUB_REPOSITORY.split('/')[1]}/` : '/')) as `/${string}/` | '/'

export default withMermaid(
  defineConfig({
    base,
    rewrites: {
      'README.md': 'index.md',
      ':pkg/README.md': ':pkg/index.md',
      ':pkg/:sub/README.md': ':pkg/:sub/index.md',
    },
    lang: 'zh-CN',
    title: 'Project Documentation',
    description: 'Project documentation managed by Cabbage',
    themeConfig: {
      nav: [
        { text: '首页', link: '/' },
        { text: '概览', link: '/00-overview/' },
        { text: '产品需求', link: '/01-product/' },
        { text: '系统架构', link: '/03-architecture/' },
        { text: 'API 接口', link: '/05-api/' },
        { text: '测试计划', link: '/08-testing/' },
        { text: 'CI/CD', link: '/11-ci-cd/' },
        { text: '发布与变更', link: '/12-release/' },
      ],
      sidebar: [
        {
          text: '项目概览',
          collapsed: false,
          items: [
            { text: '概览', link: '/00-overview/' },
          ],
        },
        {
          text: '规范与设计',
          collapsed: false,
          items: [
            { text: '产品需求 (01-product)', link: '/01-product/' },
            { text: '系统架构 (03-architecture)', link: '/03-architecture/' },
            { text: '数据设计 (04-data)', link: '/04-data/' },
            { text: 'API 接口 (05-api)', link: '/05-api/' },
          ],
        },
        {
          text: '质量与交付',
          collapsed: false,
          items: [
            { text: '测试计划 (08-testing)', link: '/08-testing/' },
            { text: '安全评审 (09-security)', link: '/09-security/' },
            { text: 'CI/CD 流程 (11-ci-cd)', link: '/11-ci-cd/' },
            { text: '发布计划 (12-release)', link: '/12-release/' },
            { text: '运维与事故 (13/15)', link: '/13-operations/' },
          ],
        },
      ],
      search: {
        provider: 'local',
      },
      footer: {
        message: 'Managed by Cabbage Documentation System',
        copyright: 'Copyright © 2026',
      },
    },
  })
)
