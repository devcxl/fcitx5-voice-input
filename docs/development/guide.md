# 开发指南

本指南面向希望参与 fcitx5-voice-input 开发的贡献者，覆盖环境准备、构建、测试与代码结构导航。架构细节见 [ARCHITECTURE](/architecture/)，用户侧安装配置见 [快速入门](/guide/getting-started)。

## 项目本质

Fcitx5 Addon（共享库），多线程架构（主线程 / Capture / VAD Worker / ASR Worker），无独立守护进程、无 Qt 依赖。许可证为 LGPL v3。

## 环境依赖

**必需：**

| 依赖 | 说明 |
|------|------|
| fcitx5 | pkg-config 名 `fcitx5` / CMake 目标 `Fcitx5Core` |
| jsoncpp | 配置与协议解析 |
| libcurl (>= 7.86.0) | HTTP/WebSocket 客户端 |
| zlib | 压缩 |
| onnxruntime | Silero VAD 推理 |

**录音后端（至少其一）：** `pipewire-0.3`（libpipewire-0.3）或 `libpulse-simple`。缺少任一只失去对应捕获后端，addon 仍可构建；两个都缺则 CMake 报错。运行期两个库均为 dlopen 延迟加载，无链接期依赖。

各发行版安装命令可参考 [`.github/actions/build/distro/*.sh`](https://github.com/devcxl/fcitx5-voice-input/tree/main/.github/actions/build/distro)（CI 在 7 个发行版容器中验证过）。

## 获取源码

```bash
git clone https://github.com/devcxl/fcitx5-voice-input.git
cd fcitx5-voice-input
git submodule update --init --recursive
```

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_TESTS=OFF
cmake --build build -j"$(nproc)"
```

构建产物为 `build/voice-input-addon.so`（无 `lib` 前缀）。

**CMake 选项：**

| 选项 | 默认 | 说明 |
|------|------|------|
| `BUILD_TESTS` | `OFF` | 编译 `tests/` 下的测试 |
| `HAVE_PULSEAUDIO` / `HAVE_PIPEWIRE` | 自动检测 | 音频捕获后端，仅编译进来的后端可用 |

::: warning 不要随意安装
除非明确需要，构建即可，不要执行 `cmake --install`——安装会写入 `/usr` 等系统路径，覆盖现有 fcitx5 环境。
:::

## 测试

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

测试覆盖 WS 帧重组、结果保序、Realtime 终态判定、会话生命周期等关键路径，完整清单见[测试概览](/testing/)。CI 中 `ci.yml` 的 tests job 在 Ubuntu 26.04 上开启 `BUILD_TESTS` 并执行 `ctest`。

## 代码结构导航

```
src/addon/
├── engine.cpp/.h          # Fcitx5 InputMethodEngineV2 入口
├── types.h                # AudioFrame/SpeechEvent/AsrResult 类型定义
├── config/                # FCITX_CONFIGURATION 配置定义（4 个子配置块）
├── capture/               # 音频捕获：PulseAudio（优先）/ PipeWire（fallback）
├── vad/                   # Silero ONNX VAD + Idle/Speaking 状态机
├── pipeline/              # 管道编排、结果汇聚（保序 + LLM 后处理）
├── asr/                   # AsrEngine/AsrSession 抽象与各后端实现
│   └── utils/             # Base64、WS 帧收发、超时预算
├── llm/                   # LLM 后处理（流式/非流式，generation 取消）
└── utils/                 # Lock-free ring buffer、线程安全队列
```

数据流：`FrameQueue → VADWorker → SpeechEventQueue → AsrSession worker → ResultCoordinator → eventDispatcher → 主线程上屏`。

深入阅读：
- 系统组成与线程模型 → [ARCHITECTURE](/architecture/)
- AsrSession 生命周期设计 → [v4 会话模型](/architecture/v4-asr-session-model)
- 各模块实现细节 → [组件细节](/architecture/component-details)
- 历史设计决策 → [ADR 目录](/architecture/adr/ADR-0001-gpt-realtime-asr)

## 关键约定（改代码前必读）

- **Addon 注册**：`FCITX_ADDON_FACTORY(...)` 必须在 `namespace fcitx` 外部。
- **PipeWire 回调**：`on_process` 内 ≤100μs，只写 ring buffer，禁止阻塞/VAD/分配。
- **音频格式统一**：16kHz mono、int16、512 samples/window（32ms）。
- **WS 收发有界**：收包用 `WsFrameReceiver`，发送有单次/总预算（`WsDeadline`），保证 `End()` 在 SessionReaper 的 15s join 内收敛。
- **Ring buffer**：故意不提供 `Clear()`（避免与回调 data race），清空用 `Read()` drain 模式。
- **配置热加载**：`setConfig()` → 保存 `voiceinput.conf` + `pipeline_->SetConfig()`。

## CI 与发布

- **`ci.yml`**：PR/push 触发，6 发行版容器矩阵（ubuntu 24.04/26.04、debian 12/13、fedora、opensuse）构建原生包（DEB/RPM），另含链接校验与 build-no-pipewire 回归防护。
- **`release.yml`**：打 `v*` tag 触发，生成 draft release（全部发行版 DEB/RPM 产物）。
- **AUR 包**：[aur.archlinux.org/packages/fcitx5-voice-input](https://aur.archlinux.org/packages/fcitx5-voice-input)，在仓库外部单独维护，不随 CI/Release 构建。
- **`docs-deploy.yml`**：`docs/` 变更推送主分支后自动构建并部署 GitHub Pages 文档站。
- onnxruntime 双策略：`system`（发行版系统包）或 `download`（upstream 1.28.0，带缓存）。

## 提交变更

1. 从最新的 `main` 切出分支。
2. 改动保持手术式：每行 diff 能追溯到要解决的问题；不顺手重构无关代码。
3. 行为修复先补能复现失败的测试，再修复使其通过。
4. 若改动涉及架构（新增线程、修改管道阶段、更换协议），同步更新 `docs/architecture/.md`，必要时新增 ADR。
5. 提交 PR 后 CI 会跑全发行版矩阵与测试，全绿后等待评审。
