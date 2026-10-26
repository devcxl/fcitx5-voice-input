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
    description: 'Fcitx5 语音输入插件 —— 用户指南、后端配置、架构与开发文档',
    themeConfig: {
      logo: 'https://raw.githubusercontent.com/devcxl/fcitx5-voice-input/main/data/fcitx_voiceinput.svg',
      nav: [
        { text: '首页', link: '/' },
        { text: '用户指南', link: '/guide/getting-started' },
        { text: '后端配置', link: '/guide/providers/openai' },
        { text: '路线图', link: '/roadmap' },
        { text: '架构', link: '/architecture/' },
        { text: '开发指南', link: '/development/guide' },
      ],
      sidebar: [
        {
          text: '用户指南',
          collapsed: false,
          items: [
            { text: '指南总览', link: '/guide/' },
            { text: '快速开始', link: '/guide/getting-started' },
            { text: '安装指南', link: '/guide/installation' },
            { text: '配置总览', link: '/guide/configuration' },
            {
              text: 'ASR 后端配置',
              collapsed: false,
              items: [
                { text: 'OpenAI 兼容（含 DashScope / MiMo / Groq / 硅基流动）', link: '/guide/providers/openai' },
                { text: '火山引擎豆包', link: '/guide/providers/volcengine' },
                { text: 'Mistral Realtime', link: '/guide/providers/mistral' },
              ],
            },
            { text: '常见问题 (FAQ)', link: '/guide/troubleshooting' },
            { text: '流式后端运行排查', link: '/guide/ws-troubleshooting' },
          ],
        },
        {
          text: '路线图',
          collapsed: false,
          items: [
            { text: '交付进展与规划', link: '/roadmap' },
          ],
        },
        {
          text: '产品',
          collapsed: false,
          items: [
            { text: '产品级 PRD（现状基线）', link: '/product/prd' },
            { text: 'PRD：输入法图标', link: '/product/prd-input-method-icon' },
            { text: 'Out of Scope', link: '/product/out-of-scope' },
          ],
        },
        {
          text: '系统架构',
          collapsed: false,
          items: [
            { text: 'ARCHITECTURE（当前）', link: '/architecture/' },
            { text: '组件细节', link: '/architecture/component-details' },
            { text: 'v4 会话模型设计', link: '/architecture/v4-asr-session-model' },
            { text: 'ADR-0001 GPT-Realtime', link: '/architecture/adr/ADR-0001-gpt-realtime-asr' },
            { text: 'ADR-0002 输入上下文生命周期', link: '/architecture/adr/ADR-0002-input-context-lifetime' },
            { text: 'ADR-0003 WS 会话失败边界', link: '/architecture/adr/ADR-0003-ws-session-failure-boundary' },
            { text: 'ADR-0004 系统音频路由', link: '/architecture/adr/ADR-0004-system-managed-capture-routing' },
            { text: '架构评审 (2026-07-05)', link: '/architecture/reviews/2026-07-05-architecture-review' },
          ],
        },
        {
          text: '开发',
          collapsed: false,
          items: [
            { text: '开发指南', link: '/development/guide' },
            { text: 'Specs：GPT-Realtime', link: '/development/specs/gpt-realtime-asr' },
            { text: 'Tasks：GPT-Realtime', link: '/development/tasks/gpt-realtime-asr' },
            { text: 'Tasks：ASR 崩溃跟进', link: '/development/tasks/asr-crash-follow-ups' },
            { text: 'Research：GPT-Realtime Whisper', link: '/development/research/gpt-realtime-whisper' },
            { text: 'Research：音频源路由 (#54)', link: '/development/research/issue-54-audio-source-routing' },
            { text: 'Research：小米 MiMo ASR', link: '/development/research/xiaomi-mimo-asr' },
          ],
        },
        {
          text: '测试',
          collapsed: false,
          items: [
            { text: '测试概览与清单', link: '/testing/' },
          ],
        },
        {
          text: 'CI/CD',
          collapsed: false,
          items: [
            { text: '多发行版构建分析', link: '/ci/multi-distro-build-analysis' },
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
