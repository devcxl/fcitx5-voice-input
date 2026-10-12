# fcitx5-voice-input 架构设计

> **状态:** 当前实现文档（与代码同步）。

## 设计目标

1. **无独立进程（daemon）** — 全部逻辑跑在 Fcitx5 addon 内部，线程隔离
2. **无 CLI 二进制** — 配置靠 fcitx5-configtool
3. **无 Qt GUI 依赖** — 不引入 Qt，避免 50MB+ 的依赖膨胀
4. **默认云端 ASR** — OpenAI 兼容 API（whisper-1），预留本地 ASR 扩展接口
5. **最小依赖** — Fcitx5 + PipeWire/PulseAudio + jsoncpp + libcurl + onnxruntime

---

## 一句话架构

> **一个 Fcitx5 Addon（共享库），内部三个工作线程（Capture / VAD Worker / ASR Dispatcher + 会话线程）。**

```
Fcitx5 进程 (voice-input-addon.so)
├── [主线] 输入法激活/停用、状态显示、上屏（EventDispatcher 轮询结果）
├── [音频线] PulseAudio 优先 → PipeWire fallback → AudioFrame → FrameQueue
├── [VAD线] 消费 FrameQueue → Silero ONNX VAD → SpeechEvent (Begin/Audio/End/Cancel) → SpeechEventQueue
└── [ASR线] AsrDispatcherLoop 消费 SpeechEventQueue → 创建 AsrSession（OpenAI/Volcengine/Mistral）→ ResultCoordinator → ResultQueue → 回调主线程
```

没有 daemon、没有 D-Bus、没有 CLI 二进制、没有 Qt GUI。

---

## 为什么可以砍掉 daemon？

### 传统方案（fcitx5-vinput）的选择

```
Fcitx5 Addon ← D-Bus IPC → vinput-daemon (独立进程)
                              ├── PipeWire 音频
                              └── ASR 推理
```

理由是："ASR 推理会卡 UI，放另一个进程里安全。"

### 实际问题

ASR 推理是 CPU/网络密集型操作，在**同一进程的另一个线程**里跑和在**另一个进程**里跑，对 UI 响应的影响是完全一样的——都不阻塞主线程。区别只有：

| 维度 | 独立进程 | 独立线程 |
|------|---------|---------|
| 隔离性 | 更强（崩溃不影响 Fcitx5） | 弱一些（线程崩溃拖整个进程） |
| 复杂度 | ❌ systemd 管理、D-Bus 定义、进程通信 | ✅ 零额外开销 |
| 模型加载 | 重复加载（addon 一份、daemon 一份） | ✅ 共享内存 |
| 延迟 | ❌ 加一次 D-Bus 序列化+反序列化 | ✅ 直接内存访问 |
| 用户操作 | ❌ `systemctl --user start vinput-daemon` | ✅ 装好即用 |
| 调试 | ❌ 跨进程追踪困难 | ✅ 单进程 GDB 一把梭 |

---

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     Fcitx5 进程                              │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                voice-input-addon.so                   │   │
│  │                                                      │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │         主线程 (Fcitx5 事件循环)              │     │   │
│  │  │  ├── activate() → pipeline.Start()            │     │   │
│  │  │  ├── deactivate() → Pause() / Stop()         │     │   │
│  │  │  ├── PollResults() → commitString()           │     │   │
│  │  │  └── 状态同步 → subModeLabel 显示状态文字      │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ FrameQueue                  │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │        音频捕获线程 (Capture Thread)          │     │   │
│  │  │  ├── PulseAudio 优先，失败后 PipeWire fallback │     │   │
│  │  │  ├── 读取音频 → 封装 AudioFrame               │     │   │
│  │  │  └── 推入 FrameQueue                         │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ FrameQueue                  │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │         VAD Worker 线程                      │     │   │
│  │  │  ├── 消费 FrameQueue                         │     │   │
│  │  │  ├── Silero ONNX predict() 返回概率           │     │   │
│  │  │  ├── Idle/Speaking 状态机                    │     │   │
│  │  │  ├── pre-roll 缓冲 + 静音超时分段             │     │   │
│  │  │  └── SpeechEvent (Begin/Audio/End/Cancel)     │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ SpeechEventQueue          │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │       ASR Dispatcher 线程 + 会话 worker       │     │   │
│  │  │  ├── 消费 SpeechEventQueue → 创建 AsrSession │     │   │
│  │  │  ├── OpenAI (whisper/chat/realtime)          │     │   │
│  │  │  ├── Volcengine Doubao / Mistral Realtime    │     │   │
│  │  │  ├── SessionReaper 超时回收                  │     │   │
│  │  │  └── ResultCoordinator 保序 + LLM 后处理      │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ ResultQueue                │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │       EventDispatcher（主线程轮询）           │     │   │
│  │  │  ├── OnAsrResult() → schedule PollResults()  │     │   │
│  │  │  └── PollResults() → commitString()          │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │           fcitx5-configtool 配置界面                  │   │
│  │  ├── Active Backend: [openai ▼]                       │   │
│  │  ├── OpenAI: BaseUrl / Key / Model / ApiMode          │   │
│  │  ├── Volcengine: Endpoint / Auth / Key / Resource     │   │
│  │  ├── Mistral: BaseUrl / Key / Model / CommitInterval  │   │
│  │  ├── LLM 后处理: Model / Prompt / Stream (可选)         │   │
│  │  └── VAD: Threshold / PreRoll / Min/Max Speech        │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 组件详述

engine / config / capture / VAD / pipeline / ASR 与线程安全队列的接口、参数与实现细节已移至 [组件详述](component-details.md)。

---

## Fcitx5 集成细节

### 录音状态指示

通过 `subModeLabelImpl()` 返回当前状态文字，fcitx5 会显示在候选词区上方：

```
🎙 录音中...     ← 录音时
🤖 识别中...     ← ASR 推理时
你好世界         ← 识别完成，自动上屏
```

状态通过 `activeIc_->updateUserInterface(UserInterfaceComponent::StatusArea)` 更新。

### 输入法激活与录音流生命周期

```cpp
void VoiceInputEngine::activate(...) {
    InitializeIfNeeded();
    activeIc_ = event.inputContext()->watch();
    uint64_t generation = activeGeneration_.fetch_add(1) + 1;
    sessionGeneration_.store(generation);
    pipeline_->SetGeneration(generation);
    pipeline_->Start();            // 已有保留流则复用，不重新打开设备
}

void VoiceInputEngine::deactivate(const InputMethodEntry&, InputContextEvent& event) {
    if (activeIc_.get() && activeIc_.get() != event.inputContext()) return;
    DeactivateContext(event.type() == EventType::InputContextFocusOut);
}

void VoiceInputEngine::DeactivateContext(bool keepCapture) {
    activeGeneration_.fetch_add(1);
    sessionGeneration_.store(0);
    ClearUI();
    activeIc_.unwatch();
    if (keepCapture) pipeline_->Pause();   // 失焦：停止识别，保留录音流
    else             pipeline_->Stop();    // 显式切走：释放录音流
}
```

- **失焦（`InputContextFocusOut`）→ `Pause()`**：关闭取帧门闸后停止 VAD 与 ASR 分发线程，取消当前会话与引擎全部会话，清空队列并作废 generation；麦克风流保留给音频服务管理，但不再提交新的应用帧。
- **回焦 → `activate()` → `Start()`**：复用保留的录音流，清空旧应用缓存（结果队列 / 保序缓冲）并恢复识别。
- **显式切换到其他输入法 / 更改输入法组 / 关闭输入法 / 退出 fcitx5 → `Stop()`**：释放录音流。由事件监听兜底（`InputContextSwitchInputMethod` 与 `InputMethodGroupAboutToChange`，`PostInputMethod`，非 `voiceinput` 或捕获中即触发）。
- **原输入框销毁（`InputContextDestroyed`，`PreInputMethod`）**：清空待提交文本并 `DeactivateContext(true)` 保流。
- 析构时 `Abort()`（`Stop()`）释放；不再有 200ms 延迟停止定时器。

> **契约与风险**：录音流由音频服务管理，插件不枚举/筛选设备、不保存设备 ID；PulseAudio `dev=nullptr`、原生 PipeWire 无显式 target，尊重系统默认或该应用的设备偏好（见 [ADR-0004](../adr/ADR-0004-system-managed-capture-routing.md)）。`Pause()` 禁止新的应用帧进入识别链路，并在停止 VAD/dispatcher 后取消当前会话；但已上传到服务端的 ASR/LLM 请求无法撤回。已删除的 200ms 延迟也从未是可靠的隐私上界；PulseAudio 阻塞停止的既有缺陷仍由 issue #14 / PR #17 跟踪，热切换的真实体验也尚未完成硬件与桌面验收。

### 结果过滤机制

每轮 activate/deactivate 递增 `activeGeneration_`，pipeline 中每个 AsrResult 携带当前的 `generation_` 值。主线程 `PollResults()` 只提交匹配当前 generation 的结果，避免异步结果错乱。

---

## 线程安全模型

```
主线程 (Fcitx5 事件循环)
    │  队列写入: activeGeneration_, sessionGeneration_
    │  队列读取: ResultQueue (TryPop)
    │  事件驱动: EventDispatcher::schedule()
    ▼
┌──────────────────────────────────────────┐
│           ThreadSafeQueue<T>              │
│  - std::mutex + condition_variable        │
│  - 多生产者多消费者安全                    │
└──────────────────────────────────────────┘
    ▲                ▲               ▲
    │                │               │
  FrameQueue     SpeechEventQueue  ResultQueue
     │                │               │
 捕获线程          VAD Worker       ASR Worker
```

### 关键线程安全策略

1. **Generation 同步** — `std::atomic<uint64_t>` 隔离过期结果
2. **音频传递** — `ThreadSafeQueue<AudioFrame>`（mutex+cv 通用队列）
3. **语音事件** — `ThreadSafeQueue<SpeechEvent>`（VAD → ASR 事件流）
4. **PipeWire 回调** — Lock-free `AudioRingBuffer`（SPSC，仅 <100μs 写入）
5. **结果回传** — `EventDispatcher::schedule()` 调度到主线程轮询
6. **配置热更新** — `setConfig()` 只更新配置对象，不重启线程

---

## 错误处理策略

| 场景 | 行为 |
|------|------|
| PulseAudio 启动失败 | 日志警告，自动 fallback 到 PipeWire |
| PipeWire 启动失败 | 日志错误，capture_ 置空，pipeline 不启动 |
| 录音库缺失/soname 变更（dlopen 失败） | 对应后端优雅降级，addon 本体不受影响 |
| 默认输入源静音 / 错误处理链 / 无信号 | 不擅自改抓其他麦克风；由用户在音频管理器修正默认输入或本应用偏好 |
| 默认输入源为回放 monitor | 可能把播放内容当作语音；提醒用户确认输入源与电平（见排查文档） |
| VAD 模型加载失败 | `Pipeline::Start` 回滚（`Stop()` 并释放 capture），日志错误 |
| 失焦（FocusOut） | 保留录音流，仅 `Pause()`：停止 VAD/ASR 分发、取消当前会话与引擎全部会话、清空队列、作废 generation；不再提交新的应用帧 |
| 失焦后回焦 | 复用保留录音流恢复识别，清空旧应用缓存与在途结果 |
| 显式切换其他输入法 / 更改输入法组 / 退出 | `Stop()` 释放录音流 |
| 在途 ASR/LLM 请求 | 取消只作用于尚未完成的本地请求；已上传服务端的请求无法撤回，200ms 不是可靠隐私上界 |
| PulseAudio 读取阻塞（设备挂起/服务断开） | 既有缺陷：`Stop()` 的 join 可能阻塞主线程；跟踪于 issue #14 / PR #17，不承诺可靠停止上界 |
| ASR HTTP 请求失败 | 丢弃当前段，继续监听下一段 |
| ASR API Key 未配置 | 对应引擎 Init 返回 false |
| Volcengine 认证失败 / WS 断开 | Init 返回 false；断开则丢弃当前段继续下一段 |
| Volcengine 服务端错误 | 日志打印响应中的 `message` 和 `X-Tt-Logid` |
| WS 事件消息跨 TCP 分片 | `WsFrameReceiver` 跨 `CURLE_AGAIN` 保留已收前缀，消息边界到齐后才解析（不丢 delta/completed） |
| WS 单条消息超过 16MB | 整条丢弃（`Skipped`），在消息边界恢复解析；不当作传输错误，不触发重连 |
| Realtime/豆包已知事件字段类型错误或 worker 异常 | 固定协议/线程错误通知 + 一次空 final；释放资源并完成 worker，不让 C++ 异常越过线程入口；正常 Realtime `error` 不改变 commit 追踪 |
| OpenAI realtime / Mistral WS 断线 | 自动重连（保持同一 sessionId），周期 commit/flush 兜底；`End()` 后不再重连，改以已累积文本兜底 final |
| 上游不读 socket（发送持续 `CURLE_AGAIN`） | `SendWsMessage` 按单次预算（默认 10s）失败；`End` 路径受 `WsEndBudget`（默认 8s）约束，预算内交出兜底 final，不会拖到 `JoinWithTimeout(15s)` 超时 |
| 发送超时 / 上游卡死 | 日志打印待发字节数与预算；调用方走重连（仅非 End 路径）或兜底终态 |
| 会话 worker 未按时退出 | SessionReaper `JoinWithTimeout` 超时后丢弃，不阻塞清理 |
| 结果乱序/迟到（并发会话） | OrderedResultBuffer 按 utteranceId 保序，旧 generation 结果丢弃 |
| 服务端 `error` 事件（如空 buffer 的 `commit_empty`） | 日志 `server error session=N: <message>` + 错误回调上报；不产生转写 item，不影响最终 item 判定 |
| End 后服务端无响应 | 空闲超时 5s（距最后一个服务端事件）触发，以已累积文本兜底 final |
| 某段 ASR 永久无终态（会话卡死） | 保序闸门停滞放行：超过 40s 时该段以超时错误上屏（`语音识别失败` 提示），后续段结果照常交付，不再被永久扣住 |
| OpenAI API 返回空结果 | 丢弃不上屏 |

---

## 构建与打包

### 依赖

```
fcitx5-voice-input
├── fcitx5                      # 输入法框架
├── pipewire-0.3                # PipeWire 音频捕获 (fallback，运行时 dlopen)
├── libpulse-simple             # PulseAudio 音频捕获 (优先，运行时 dlopen)
├── jsoncpp                     # JSON 解析
├── libcurl                     # HTTP/WebSocket 客户端（ASR 必需，>= 7.86.0）
├── zlib                        # Gzip 压缩（火山引擎后端必需）
└── onnxruntime                 # Silero VAD ONNX Runtime（v1.28.0 / 系统包双策略）
```

> 录音库（libpulse-simple/libpipewire）**不链接**（无 DT_NEEDED）：各 capture 后端在 Start 时 dlopen + dlsym 函数指针表延迟加载，库缺失/soname 变更/符号不完整时优雅降级，addon 本体不受影响（详见 PR #20：Arch 构建 `-z,now` 兼容）。

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_TESTS` | OFF | 构建单元/集成测试（`tests/`，`ctest` 运行；CI verify job 开启） |
| `ONNXRUNTIME_ROOT` | "" | ONNX Runtime 自定义路径（onnxruntime 双策略：pkg-config 系统包或 download 1.28.0，CI 缓存加速） |

### 构建产物

```
build/
└── voice-input-addon.so        # 唯一的输出产物（无 lib 前缀）
```

### 打包

- **Arch Linux**: `aur/PKGBUILD`（依赖 fcitx5 / pipewire / jsoncpp / curl / onnxruntime-cpu / zlib）
- **DEB**: CPack 自动生成（dpkg-shlibdeps 自动依赖）
- **RPM**: CPack / rpmbuild 自动依赖
- **CI**: 7 发行版容器矩阵（ubuntu-24.04 / ubuntu-26.04 / debian-12 / debian-13 / fedora-44 / opensuse-tumbleweed / archlinux），各发行版原生容器内构建原生包 + 链接校验（nm/readelf/dlopen smoke）

---

## 项目目录结构

```
fcitx5-voice-input/
├── docs/                           # 项目文档（Cabbage 标准树，见 docs/README.md）
├── README.md / README.zh-CN.md     # 用户使用文档（中英文）
├── AGENTS.md                       # 开发助手约定
├── CHANGELOG.md
├── LICENSE
├── CMakeLists.txt                  # 顶配 CMake
├── .gitmodules
│
├── src/
│   └── addon/                      # 唯一的代码目录
│       ├── engine.cpp/.h           # VoiceInputEngine Fcitx5 入口
│       ├── types.h                 # AudioFrame / SpeechEvent / AsrResult 类型定义
│       ├── voiceinput.conf.in      # Fcitx5 addon 配置模板（@PROJECT_VERSION@ 替换）
│       ├── config/
│       │   └── voiceinput-config.h         # FCITX_CONFIGURATION 宏定义（4 个子配置块）
│       ├── capture/
│       │   ├── audio_capture.h             # 捕获后端抽象接口
│       │   ├── pulse_audio_capture.cpp/.h  # PulseAudio 优先（dlopen）
│       │   └── pipewire_capture.cpp/.h     # PipeWire fallback（dlopen）
│       ├── vad/
│       │   ├── silero_vad.cpp/.h           # Silero ONNX 封装
│       │   └── vad.cpp/.h                  # VADWorker 状态机（Idle/Speaking）
│       ├── pipeline/
│       │   ├── pipeline.cpp/.h             # 管道编排（2 队列 + 分发/回收线程）
│       │   ├── result_coordinator.cpp/.h   # 结果汇聚：保序 + LLM 后处理 + generation 过滤
│       │   └── ordered_result_buffer.h     # 并发会话结果保序缓冲
│       ├── asr/
│       │   ├── asr_engine.h / asr_engine.cpp   # 引擎工厂抽象
│       │   ├── asr_session.h                    # 会话抽象（FeedAudio/End/Cancel）
│       │   ├── session_reaper.cpp/.h            # 会话回收线程
│       │   ├── openai_asr.cpp/.h                # OpenAI 兼容引擎（whisper/chat/realtime）
│       │   ├── realtime_asr.cpp/.h              # OpenAI Realtime WS 会话
│       │   ├── volcengine_asr.cpp/.h            # 火山引擎豆包引擎
│       │   ├── mistral_asr.cpp/.h               # Mistral Realtime 引擎
│       │   ├── utils/base64.cpp/.h              # Base64 编解码
│       │   ├── utils/ws_frame_receiver.h        # WS 消息重组（跨 TCP 分片保留前缀）
│       │   ├── utils/ws_frame_sender.h          # WS 发送（单次预算内失败）
│       │   └── utils/ws_deadline.h              # 发送/建连截止时间与中止条件
│       ├── llm/
│       │   ├── llm_client.cpp/.h               # LLM 后处理客户端（流式/非流式）
│       │   └── llm_request_cancellation.h      # generation 取消
│       └── utils/
│           ├── audio_buffer.h                  # Lock-free SPSC ring buffer
│           └── thread_safe_queue.h             # mutex+cv 通用队列（有界）
│
├── third_party/
│   ├── silero-vad/              # git submodule，VAD ONNX 模型
│   └── sherpa-onnx/             # git submodule，本地 ASR（预留，未构建）
│
├── po/
│   └── zh_CN.po                 # 中文翻译
│
├── aur/
│   └── PKGBUILD                 # Arch Linux 打包脚本
│
├── cmake/                       # 自定义 FindXXX.cmake
├── data/                        # 图标 + 输入法注册文件
├── scripts/                     # 辅助脚本
├── .github/workflows/
│   ├── ci.yml                   # PR/push：7 发行版矩阵 + 链接校验 + build-no-pipewire
│   ├── release.yml              # tag v*：draft release（含 AUR 源码包）
│   └── cabbage.yml              # 文档门禁 + GitHub Pages 部署
```

> 本文档位于 `docs/03-architecture/system-design/ARCHITECTURE.md`（Cabbage 标准树）。

---

## Route Map

### 已交付 ✅
- [x] CMake 构建框架 + Fcitx5 Addon 骨架
- [x] PulseAudio + PipeWire 音频捕获（dlopen 延迟加载）
- [x] Silero ONNX VAD 分段
- [x] OpenAI 兼容 API ASR 引擎（whisper / chat / realtime）
- [x] Volcengine Doubao 流式 ASR 引擎
- [x] Mistral Realtime 流式 ASR 引擎（v0.5.0）
- [x] v4 会话模型：AsrSession / SessionReaper / ResultCoordinator 保序交付
- [x] LLM 后处理（流式/非流式，generation 取消）
- [x] fcitx5-configtool 配置界面（多后端子配置）
- [x] 多发行版打包（7 发行版 CI 矩阵：DEB / RPM / pkg.tar.zst）

### 待实现 ⏳
- [ ] 本地 ASR 引擎（sherpa-onnx 子模块已预留，未接入构建）
- [ ] Command 引擎（外部命令云 ASR）
- [ ] 场景系统
- [ ] 热词优化

---

## FAQ

### Q: Fcitx5 线程崩溃不就输入法没了？
A: ASR 会话 worker 独立运行，且 v4 起有完整的资源兜底：会话超时 `JoinWithTimeout`（默认 2s）、`SessionReaper` 独立回收线程、引擎容量上限 `kMaxActiveSessions=3`（超限取消最旧会话）。会话崩溃不会阻塞 dispatcher，pipeline 可继续处理后续语音段。

### Q: 主线程不是还卡？
A: 主线程只负责事件分发和上屏。音频捕获（实时音频）在独立线程，HTTP/WS ASR 请求（网络 IO 密集型）在会话 worker 线程。唯一的"卡"是主线程的 `PollResults()` 轮询，但只做队列读取和 `commitString()`，微秒级操作。

### Q: 为什么默认用 OpenAI API 而不是本地 ASR？
A: 当前阶段以中文语音输入为主，云端 Whisper 在中文准确率上优于可用的开源离线方案。`third_party/sherpa-onnx` 子模块已预留，未来通过 `AsrEngine` 工厂接口接入本地引擎。

### Q: 没有高级 JSON 配置层了？
A: 是的。`FCITX_CONFIGURATION` 宏已能满足当前所有配置需求（ASR 后端 + OpenAI/Mistral/Volcengine 子配置 + VAD 参数 + LLM 后处理）。将来场景系统需要复杂结构时可能重新引入 JSON 配置。

### Q: 会话结果会乱序吗？
A: 不会。v4 的 `ResultCoordinator + OrderedResultBuffer` 以 utteranceId 为序键重排并发会话的最终结果，并按 generation 过滤过期激活；连续语音段的最终结果永远按说话顺序上屏。若某段会话永久无终态，闸门会在其停滞超过 40s 后以超时错误放行该段并推进，后续段仍按序上屏（不会出现「后面全部消失」）。

---

*—— 架构设计 v4，反映实际代码（与 `src/addon/` 同步）。*

## 相关文档

- [组件详述](component-details.md)（engine / config / capture / VAD / pipeline / ASR / 队列的接口与实现细节）
- [v4 会话模型设计](v4-asr-session-model.md)（AsrSession 抽象与线程模型的详细设计）
- [ADR-0001：GPT-Realtime 流式实时转录](../adr/ADR-0001-gpt-realtime-asr.md)
- [架构评审（2026-07-05）](../reviews/2026-07-05-architecture-review.md)
