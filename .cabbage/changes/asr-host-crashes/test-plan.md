---
change: asr-host-crashes
cabbage_stage: tests
change_type: bugfix
---

# Strategy

以真实公开接口进行红/绿验证：引擎 activate/deactivate/keyEvent，AsrSession 注册 callback / StartWorker / End / Cancel / GetState / JoinWithTimeout。不访问私有成员、不连接真实云端、不录音。UI 测试链接本次构建的 addon，以 ELF 替换仅隔离公开 Pipeline 启动与注册边界；独立 XDG 目录防止读取用户配置。

# Cases

| Scenario | Expected / Observed |
|---|---|
| 输入框销毁后排队清空 | 修复前 SIGSEGV；修复后不访问已销毁输入框 |
| 有效输入框、worker 排队后销毁、快速激活 | 正确清空 / 不崩溃 / 不污染新上下文 |
| 待提交文本快速切换 | 仅提交给原输入框；该用例先失败后修复 |
| Realtime delta 数组 / 豆包 code 对象 | 修复前均 SIGABRT；修复后一次协议错误、一次空 final、无非法文本 |
| JSON 根/类型/数值溢出、递归深度、非法 gzip | worker 在 2s 内自行完成（不能先 Cancel），join 在 500ms 内完成 |
| 可空 Realtime 字段 / 空豆包确认帧 | 后续正常文本继续交付，不误失败；空确认帧初版先失败后修复 |
| partial/error/final callback 抛异常与非标准异常 | 旧基线 8 个场景均 SIGABRT；新边界不崩溃、不重试 final、可回收 |
| 现有分片、commit、发送预算 | 不回归 |

# Verification

- Release：`cmake --build build --parallel 4` 与 `ctest --test-dir build --output-on-failure`，28/28 通过。
- 独立 Debug + `-fsanitize=address,undefined -fno-omit-frame-pointer` 构建，全套 28/28 通过。
- UI 5 个核心场景连续 10 轮通过；独立暂存树中的 Realtime 阶段 22/22 通过，证明提交可单独构建。
- 新 WS 用例缺少 ws 协议时返回 77，CTest 记录 SKIP；fixture NO_PROXY 指向回环。服务端关闭/join、测试观察对象生命周期与 CTest TIMEOUT 均受控。

# Limits

本地结果不是云端账号兼容性、七发行版 WS 运行矩阵或远端 CI 结果；推送后须另外核实 CI。Mistral/构造失败/native 故障隔离不在本次验收范围。
