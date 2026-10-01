---
change: mimo-asr-chat-mode
cabbage_stage: tests
change_type: bugfix
---

# Strategy

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| Unit | 请求体四种 language/ITN 组合与消息形状 | `BuildChatAsrRequestBody` | maintainer |
| Regression | 既有 ctest 用例不回归 | `ctest` | maintainer |
| Manual | MiMo 实网转录（已完成 2026-10-01，复用真实请求构造路径） | 真实 HTTP 200 与转写文本 | maintainer |

# Test Environment and Data

- 单元测试：仅依赖 jsoncpp，无网络、无 API Key；`-DBUILD_TESTS=ON` + `ctest`。
- 手工验收：Xiaomi MiMo API Key（`sk-` 前缀）与可联网环境；配置
  BaseUrl/Model/ApiMode/Language/EnableItn 后口述中文短句。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | language=zh、EnableItn=true | Unit | `asr_options.language=zh` 且 `enable_itn=true`；model/messages/data URL 正确 | High |
| T-2 | language=auto、EnableItn=true | Unit | 无 language 字段，`enable_itn` 存在 | High |
| T-3 | language=zh、EnableItn=false | Unit | 仅 language，无 `enable_itn` | High |
| T-4 | language=auto、EnableItn=false | Unit | 整个 `asr_options` 省略 | Medium |
| T-5 | MiMo 实网短句转录 | Manual | 实测 HTTP 200；转写与 Wikibooks 原文一致；49.2s 音频 2.39s（远低于 30s 超时） | High |

# Regression Coverage

- 改动后既有 ctest 全量 12 项全部通过。
- `chat_asr_request_test` 固定四种组合，防止 language 覆盖缺陷回归。
- DashScope 默认路径由 T-1 的字段存在性间接保护（`enable_itn` 默认仍发送）。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| 时延 | T-5 记录 End 到结果耗时 | 无硬阈值，仅记录基线（HTTP 超时 30s 内） |
| 安全 | 代码审查日志输出 | 不含 API Key 与音频内容 |

# Entry and Exit Criteria

- Entry: `cmake -B build -DBUILD_TESTS=ON` 配置成功且 `chat_asr_request_test` 可执行。
- Exit: `ctest` 全绿、代码审查通过；T-5 已于 2026-10-01 完成并回填结果。

# Risks

- 账户余额：已充值，T-5 完成（2026-10-01）：三组 `asr_options` 组合与 49.2s 音频全部 HTTP 200，转写与原文一致。
- `enable_itn`：实网被容忍（V1 HTTP 200）；但官方文档未定义该字段，接入 MiMo 仍按文档关闭，不依赖未定义行为。
- 未覆盖：`stream=true` 的 SSE 增量（当前实现不使用）。
- 上游 OpenAI 兼容层差异不在单元测试覆盖内；缓解：T-5 覆盖真实端点。
