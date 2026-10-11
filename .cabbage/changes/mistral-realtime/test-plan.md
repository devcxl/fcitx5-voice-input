---
change: mistral-realtime
cabbage_stage: tests
change_type: bugfix
---

# Strategy

通过真实 MistralAsrSession 的公开方法注入本地 WS 响应，验证 callback 内容/次数、workerDone、可回收和请求时序，而不是只验证解析器。

| Level | Scope | Test seam | Owner |
|---|---|---|---|
| Integration | 握手、推流、收尾、异常 | StartWorker/FeedAudio/End 与 callbacks | 维护者 |
| Manual | 真实 HTTP 拒绝原因与正常转写 | 用户云端账户 | 用户 |

# Test Environment and Data

127.0.0.1 socket + test-key，绕过代理，超时与 Stop 打断 IO，不访问云端或用户配置。curl 不支持 WS 时返回 77，跳过不能当作通过。

# Cases

| ID | Scenario | Level | Expected result | Priority |
|---|---|---|---|---|
| T-1 | HTTP 401/404 | Integration | 2s 内结束，一次带状态码错误与空 final | High |
| T-2 | created/update/append/flush/end/done | Integration | 正确 PCM 配置、正常全文、不重复 | High |
| T-3 | 官方嵌套 error / 合法字典 message | Integration | 数值 code、一次错误与空 final | High |
| T-4 | 非法 text/root/error | Integration | 固定协议错误，不崩溃 | High |
| T-5 | callback 抛异常 | Integration | 不 abort、一次终态尝试、workerDone | High |
| T-6 | End 后关闭与取消 | Integration | 累积文本兜底、取消不产 final | High |

# Regression Coverage

- 全套现有 UI、ASR 保序、WS 分片与发送/End 预算测试。

# Non-functional Testing

| Quality attribute | Method | Threshold |
|---|---|---|
| 内存安全 | ASan/UBSan | 无错误报告 |
| 收敛性 | socket/CTest 超时 + workerDone | 不 detach、不挂起 |
| 兼容性 | 七发行版 CI | 全部通过 |

# Entry and Exit Criteria

- Entry: 保留 baseline、复现 HTTP/协议故障；原错误日志已脱敏。
- Exit: 新回归与全套测试无失败/无跳过，CI 通过；云端复测状态单独报告。

# Risks

- 本地夹具不能证明用户凭据有效、模型权限或云端实时可用性；须人工复测，不声称云端故障已验证消失。
