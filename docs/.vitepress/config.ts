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
    title: 'fcitx5-voice-input',
    description: 'Fcitx5 语音输入插件 —— 架构、需求、CI/CD 项目文档',
    themeConfig: {
      logo: 'https://raw.githubusercontent.com/devcxl/fcitx5-voice-input/main/data/fcitx_voiceinput.svg',
      nav: [
        { text: '首页', link: '/' },
        { text: '概览', link: '/00-overview/' },
        { text: '产品需求', link: '/01-product/' },
        { text: '系统架构', link: '/03-architecture/' },
        { text: '开发记录', link: '/06-development/' },
        { text: 'CI/CD', link: '/11-ci-cd/' },
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
          text: '产品需求 (01-product)',
          collapsed: false,
          items: [
            { text: '索引', link: '/01-product/' },
            { text: 'PRD：输入法图标', link: '/01-product/prd/add-input-method-icon' },
            { text: 'Out of Scope', link: '/01-product/out-of-scope' },
          ],
        },
        {
          text: '系统架构 (03-architecture)',
          collapsed: false,
          items: [
            { text: '索引', link: '/03-architecture/' },
            { text: 'ARCHITECTURE（当前）', link: '/03-architecture/system-design/ARCHITECTURE' },
            { text: 'v4 会话模型设计', link: '/03-architecture/system-design/v4-asr-session-model' },
            { text: 'ADR-0001 GPT-Realtime', link: '/03-architecture/adr/ADR-0001-gpt-realtime-asr' },
            { text: '架构评审 (2026-07-05)', link: '/03-architecture/reviews/2026-07-05-architecture-review' },
          ],
        },
        {
          text: '开发记录 (06-development)',
          collapsed: false,
          items: [
            { text: '索引', link: '/06-development/' },
            { text: 'Specs：GPT-Realtime', link: '/06-development/specs/gpt-realtime-asr' },
            { text: 'Tasks：GPT-Realtime', link: '/06-development/tasks/gpt-realtime-asr' },
            { text: 'Research：GPT-Realtime Whisper', link: '/06-development/research/gpt-realtime-whisper' },
          ],
        },
        {
          text: 'CI/CD (11-ci-cd)',
          collapsed: false,
          items: [
            { text: '索引', link: '/11-ci-cd/' },
            { text: '多发行版构建分析', link: '/11-ci-cd/multi-distro-build-analysis' },
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