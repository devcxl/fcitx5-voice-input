import { defineConfig } from 'vitepress'
import { withMermaid } from 'vitepress-plugin-mermaid'
import { fileURLToPath, URL } from 'node:url'

const base = (process.env.BASE_URL || (process.env.GITHUB_REPOSITORY ? `/${process.env.GITHUB_REPOSITORY.split('/')[1]}/` : '/')) as `/${string}/` | '/'

const dayjsEsm = fileURLToPath(new URL('../node_modules/dayjs/esm/index.js', import.meta.url))
const mermaidEsm = fileURLToPath(new URL('../node_modules/mermaid/dist/mermaid.esm.mjs', import.meta.url))

export default withMermaid(
  defineConfig({
    base,
    rewrites: {
      'README.md': 'index.md',
      ':pkg/README.md': ':pkg/index.md',
      ':pkg/:sub/README.md': ':pkg/:sub/index.md',
    },
    lang: 'zh-CN',
    title: 'fcitx5-voice-input',
    vite: {
      resolve: {
        alias: {
          dayjs: dayjsEsm,
          mermaid: mermaidEsm,
        },
      },
    },
    description: 'Fcitx5 语音输入插件 —— 架构、需求、CI/CD 项目文档',
    themeConfig: {
      logo: 'https://raw.githubusercontent.com/devcxl/fcitx5-voice-input/main/data/fcitx_voiceinput.svg',
      nav: [
        { text: '首页', link: '/' },
        { text: '入门与配置', link: '/00-overview/getting-started' },
        { text: '系统架构', link: '/03-architecture/system-design/ARCHITECTURE' },
        { text: '开发指南', link: '/06-development/guide' },
        { text: 'CI/CD', link: '/11-ci-cd/multi-distro-build-analysis' },
      ],
      sidebar: [
        {
          text: '入门与概览 (00-overview)',
          collapsed: false,
          items: [
{ text: '项目概览', link: '/00-overview/' },
            { text: '快速入门', link: '/00-overview/getting-started' },
            { text: '快速安装指南', link: '/00-overview/installation' },
            { text: '详细配置指南', link: '/00-overview/configuration' },
            { text: '常见问题与排查 (FAQ)', link: '/00-overview/troubleshooting' },
          ],
        },
        {
          text: '产品需求 (01-product)',
          collapsed: false,
          items: [
            { text: '产品级 PRD（现状基线）', link: '/01-product/prd/product-prd-v1' },
            { text: 'PRD：输入法图标', link: '/01-product/prd/add-input-method-icon' },
            { text: 'Out of Scope', link: '/01-product/out-of-scope' },
          ],
        },
        {
          text: '系统架构 (03-architecture)',
          collapsed: false,
          items: [
            { text: 'ARCHITECTURE（当前）', link: '/03-architecture/system-design/ARCHITECTURE' },
            { text: '组件细节', link: '/03-architecture/system-design/component-details' },
            { text: 'v4 会话模型设计', link: '/03-architecture/system-design/v4-asr-session-model' },
            { text: 'ADR-0001 GPT-Realtime', link: '/03-architecture/adr/ADR-0001-gpt-realtime-asr' },
            { text: 'ADR-0002 输入上下文生命周期', link: '/03-architecture/adr/ADR-0002-input-context-lifetime' },
            { text: 'ADR-0003 WS 会话失败边界', link: '/03-architecture/adr/ADR-0003-ws-session-failure-boundary' },
            { text: 'ADR-0004 系统音频路由', link: '/03-architecture/adr/ADR-0004-system-managed-capture-routing' },
            { text: '架构评审 (2026-07-05)', link: '/03-architecture/reviews/2026-07-05-architecture-review' },
          ],
        },
        {
          text: '开发 (06-development)',
          collapsed: false,
          items: [
            { text: '开发指南', link: '/06-development/guide' },
            { text: 'Specs：GPT-Realtime', link: '/06-development/specs/gpt-realtime-asr' },
            { text: 'Tasks：GPT-Realtime', link: '/06-development/tasks/gpt-realtime-asr' },
            { text: 'Research：GPT-Realtime Whisper', link: '/06-development/research/gpt-realtime-whisper' },
          ],
        },
        {
          text: '测试 (08-testing)',
          collapsed: false,
          items: [
            { text: '测试概览与清单', link: '/08-testing/' },
          ],
        },
        {
          text: 'CI/CD (11-ci-cd)',
          collapsed: false,
          items: [
            { text: '多发行版构建分析', link: '/11-ci-cd/multi-distro-build-analysis' },
          ],
        },
        {
          text: '运维 (13-operations)',
          collapsed: false,
          items: [
            { text: '运行期排查', link: '/13-operations/troubleshooting' },
          ],
        },
      ],
      search: {
        provider: 'local',
      },
      footer: {
        message: 'Fcitx5 Voice Input Documentation',
        copyright: 'Copyright © 2026',
      },
    },
  })
)