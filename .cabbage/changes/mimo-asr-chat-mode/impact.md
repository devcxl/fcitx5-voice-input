---
change: mimo-asr-chat-mode
cabbage_stage: impact
change_type: bugfix
---

# Change Summary

小米 MiMo-V2.5-ASR 云 API（OpenAI 兼容 `POST /v1/chat/completions`）需要正确的
`asr_options` 请求体才能接入。现有 OpenAI `chat` 模式存在缺陷：`body["asr_options"]
= asrOpts` 整体覆盖导致 `language` 从未发送，且 DashScope 专用字段 `enable_itn`
被硬编码为 `true`。修复后 MiMo 可直接通过 `ActiveBackend=openai` + `ApiMode=chat`
接入，无需新增后端（调研与决策见 `docs/06-development/research/xiaomi-mimo-asr.md`）。

# Impact Matrix

| Area | Impact | Notes |
|---|---|---|
| Product | No | 复用既有 chat 模式能力接入另一家兼容供应商，无产品需求或交互变化 |
| Architecture | Yes | 请求体构造从 `openai_asr.cpp` 抽为纯函数 `asr/utils/chat_asr_request`；`AsrEngine::Config` 与 `OpenAIAsrConfig` 新增 `enableItn` |
| API | No | 无对外接口变化 |
| Database | No | |
| Security | No | 凭据仍由 `Authorization: Bearer` 发送、TLS 校验不变；未新增敏感信息日志 |
| Testing | Yes | 新增 `tests/chat_asr_request_test.cpp`，覆盖 4 种 language/ITN 组合 |
| Deployment | No | 无打包或依赖变化 |
| Operations | No | 无新增运行期故障模式；接入说明进 README |
| Data | No | |
| Performance | No | 不改变上传体积、线程模型与超时 |

# Impact Details

- **Architecture**: `chat_asr_request.h/.cpp` 提供
  `BuildChatAsrRequestBody(model, language, enableItn, audioDataUrl)`，唯一职责是
  构造请求 JSON；`OpenaiAsrSession` 继续负责 WAV 编码与传输。`auto`/空 language 与
  `enableItn=false` 时不发送对应字段，两者皆无时整个 `asr_options` 省略，避免向仅
  接受 `language` 的上游发送无关字段。
- **Testing**: 单元用例断言字段存在/缺失的四种组合与 messages/data URL 形状；
  由 `ctest` 及 CI `tests` job 执行。
- **文档**: ARCHITECTURE 配置表与引擎表、README 供应商清单与 MiMo 示例、
  CHANGELOG、调研文档 §9。

# Risks

| Risk | Likelihood | Impact | Mitigation | Owner |
|---|---|---|---|---|
| MiMo 对未定义参数的处理策略 | Low | 官方 ASR 文档未定义 `enable_itn`，平台声明未定义参数可能被过滤或报错，可能导致 chat 模式 400 | 接入 MiMo 时关闭 `EnableItn`（README 示例 + 调研 §9）；DashScope 默认行为不变 | maintainer |
| `language` 行为变更影响 DashScope 用户 | Low | 此前从未发送 language，修复后按用户配置发送；上游若对 language 敏感可能改变输出 | 仅在显式设置非 auto 语言时发送；默认 auto 行为不变 | maintainer |
| 实网验证未全部完成 | Medium | Bearer 已实测通过（402 而非 401）；真实转写文本与 30s 超时待账户余额充值后补测 | 调研 §9 记录进展与补测清单；不影响请求形态正确性判定 | maintainer |

# Documentation Updates

- `docs/03-architecture/system-design/ARCHITECTURE.md`：`OpenAIAsrConfig` 配置表、引擎实现表
- `docs/08-testing/README.md`：登记 `chat_asr_request_test`
- `README.md`：供应商清单与 MiMo chat 模式配置示例
- `CHANGELOG.md`：Unreleased 条目
- `docs/06-development/research/xiaomi-mimo-asr.md`：决策与实施记录（§9）
