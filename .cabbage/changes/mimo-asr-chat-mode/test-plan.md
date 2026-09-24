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
| Manual | MiMo 实网转录（待 API Key） | fcitx5 实际输入 | maintainer |

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
| T-5 | MiMo 实网短句转录 | Manual | 单段识别文本正确上屏；记录 HTTP 状态与耗时 | High |

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
- Exit: `ctest` 全绿、代码审查通过；T-5 在取得 API Key 后补测并回填结果。

# Risks

- 无 API Key：T-5 无法立即执行，MiMo 兼容性（尤其 `enable_itn`）存在未验证假设；
  缓解：提供关闭该字段的开关，并在调研 §9 记录实网验证顺序。
- 上游 OpenAI 兼容层差异不在单元测试覆盖内；缓解：T-5 覆盖真实端点。
