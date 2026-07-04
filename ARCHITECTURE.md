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

> **一个 Fcitx5 Addon（共享库），内部三个工作线程。**

```
Fcitx5 进程 (voice-input-addon.so)
├── [主线] 输入法激活/停用、状态显示、上屏（EventDispatcher 轮询结果）
├── [音频线] PulseAudio 优先 → PipeWire fallback → AudioFrame → FrameQueue
├── [VAD线] 消费 FrameQueue → Silero ONNX VAD → SpeechEvent (Begin/Audio/End/Cancel) → SpeechEventQueue
└── [ASR线] 消费 SpeechEventQueue → OpenAI / Volcengine ASR → ResultQueue → 回调主线程
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
│  │  │  ├── deactivate() → 延迟 Stop()               │     │   │
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
│  │  │  └── 完整说话段 → UtteranceQueue              │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ UtteranceQueue              │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │         ASR Worker 线程                      │     │   │
│  │  │  ├── 消费 UtteranceQueue                     │     │   │
│  │  │  ├── int16 → float32 转换                   │     │   │
│  │  │  ├── OpenAI 兼容 API（HTTP multipart）   │     │   │
│  │  │  └── 结果 → ResultQueue + 回调主线程          │     │   │
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
│  │  ├── ASR Backend: [openai ▼]                         │   │
│  │  ├── OpenAI: Endpoint / API Key / Model               │   │
│  │  ├── Volcengine: Endpoint / Auth / Key / Resource     │   │
│  │  ├── Audio Source: [Default (Auto) ▼]                 │   │
│  │  ├── VAD Threshold / Silence Threshold                │   │
│  │  └── LLM Model / System Prompt (可选)                 │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 组件详述

### 1. 输入法引擎 (`src/addon/engine.cpp/.h`)

```cpp
class VoiceInputEngine : public fcitx::InputMethodEngineV2 {
    // Fcitx5 生命周期
    void reloadConfig() override;     // 重载配置
    void setConfig() override;        // 配置变更时保存
    void activate() override;         // 切换到语音输入 → pipeline.Start()
    void deactivate() override;       // 切出 → 延迟 200ms Stop()
    void keyEvent() override;         // 不消费按键（直接忽略）
    string subModeLabelImpl() override; // 显示 "🎙 录音中..." 状态

    // 结果处理（主线程）
    void OnAsrResult(const string& text);  // 回调 → schedule PollResults()
    void PollResults();                    // 轮询 ResultQueue → commitString()
    void CommitText(const string& text);   // activeIc_->commitString()

    // 线程管理
    void InitializeIfNeeded();        // 延迟初始化 ASR 引擎

private:
    Instance* instance_;
    unique_ptr<Pipeline> pipeline_;
    EventDispatcher eventDispatcher_;
    VoiceInputConfig config_;
    InputContext* activeIc_;
    atomic<uint64_t> activeGeneration_;   // 当前有效 generation
    atomic<uint64_t> sessionGeneration_;  // 当前 session generation
};
```

**关键设计：**
- `activeGeneration_` 随每次 activate/deactivate 递增，用于过滤过期结果
- `deactivate()` 延迟 200ms 才真正停止，防止快速切换窗口时误停
- `setConfig()` 保存到 `conf/voiceinput.conf`，热更新 pipeline

### 2. 配置 (`src/addon/config/`)

单层配置，全部通过 fcitx5 `FCITX_CONFIGURATION` 宏定义，由 fcitx5-configtool 可视化编辑。

```
src/addon/config/
├── config.h                  # 桥接头文件 → voiceinput-config.h
└── voiceinput-config.h       # FCITX_CONFIGURATION 宏定义全部配置键
```

**配置键：**

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `ASRBackend` | String | `openai` | ASR 后端（openai / volcengine） |
| `OpenAIEndpoint` | String | `https://api.openai.com/v1` | OpenAI 兼容 API Endpoint |
| `OpenAIApiKey` | String | `""` | API Key |
| `OpenAIModel` | String | `whisper-1` | 模型名 |
| `OpenAILanguage` | String | `""` | 输出语言（空=自动检测） |
| `VolcengineEndpoint` | String | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` | 火山引擎 WebSocket 地址 |
| `VolcengineAuthMode` | String | `api_key` | 认证模式 |
| `VolcengineApiKey` | String | `""` | API Key（新版控制台） |
| `VolcengineAppKey` | String | `""` | App Key（旧版） |
| `VolcengineAccessKey` | String | `""` | Access Key（旧版） |
| `VolcengineResourceId` | String | `volc.seedasr.sauc.duration` | 资源 ID |
| `VolcengineChunkMs` | Int | `200` | 音频分包（ms） |
| `VolcengineEnableITN` | Bool | `true` | ITN 文本规范化 |
| `VolcengineEnablePunc` | Bool | `true` | 标点符号 |
| `VolcengineEnableDDC` | Bool | `false` | 语义顺滑 |
| `VolcengineEnableNonstream` | Bool | `true` | 二遍识别 |
| `VolcengineEndWindowMs` | Int | `800` | 判停窗口（ms） |
| `AudioSource` | String | `""` | 音频源（空=自动，动态枚举系统设备） |
| `LLMModel` | String | `""` | LLM 模型（空=禁用） |
| `LLMSystemPrompt` | String | `""` | LLM 系统提示词 |
| `VADThreshold` | Int 0-100 | `50` | VAD 阈值百分比 |
| `SilenceThresholdMs` | Int 100-10000 | `800` | 静音超时毫秒数 |

`AudioSource` 下拉框通过 `pactl list sources short` 动态枚举 PulseAudio 输入设备（排除 monitor source）。

无高级 JSON 配置层。

### 3. 音频捕获 (`src/addon/capture/`)

```cpp
// 捕获后端抽象接口
class AudioCapture {
public:
    virtual ~AudioCapture() = default;
    virtual bool Start() = 0;      // 开始捕获，返回成功失败
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual const char* Name() const = 0;
    virtual void SetSourceName(const string& name) {}
    virtual void SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue);
};
```

**PulseAudio 后端（优先）：**
- 使用 `libpulse-simple` 同步读取 API
- 创建独立线程 `CaptureLoop()` 循环读取
- 兼容传统 PulseAudio 和 pipewire-pulse 模拟层
- 支持 `SetSourceName()` 指定输入设备

**PipeWire 后端（fallback）：**
- 使用 `pw_thread_loop` + `pw_stream` 实时音频回调
- `on_process` 回调（≤100μs）只写 lock-free ring buffer
- 独立 `DrainLoop()` 线程从 ring buffer 读取 → 封装 AudioFrame → FrameQueue
- 不依赖 WirePlumber 的特定版本

**选择策略：** Pipeline 中先尝试 PulseAudio，失败后 fallback 到 PipeWire 直连。

### 4. VAD (`src/addon/vad/`)

```
src/addon/vad/
├── silero_vad.cpp/.h    # Silero ONNX 封装
└── vad.cpp/.h           # VADWorker 状态机
```

**SileroVAD：** 轻量 ONNX Runtime 封装，输入 512 samples int16，返回 0~1 概率。维护内部状态（context + h/c），调用 `Reset()` 重置 session。

**VADWorker（独立线程）：**

```
Input:  FrameQueue → AudioFrame (512 int16, 32ms)
Output: SpeechEventQueue → SpeechEvent (Begin / Audio / End / Cancel)

状态机: Idle ──(连续 speechFrames ≥ startFrames)──→ Speaking
        Speaking ──(silenceFrames ≥ endSilenceFrames || 超长)──→ End / Cancel → Idle
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `speechThreshold` | 0.5 | 说话判定阈值 |
| `silenceThreshold` | 0.35 | 静音判定阈值（= speechThreshold × 0.7） |
| `startFrames` | 2 | 连续几帧说话触发 onset |
| `preRollMs` | 300 | onset 前保留音频（毫秒） |
| `endSilenceMs` | 700 | 连续静音多久结束说话段 |
| `minSpeechMs` | 300 | 最短说话段（太短丢弃） |
| `maxSpeechMs` | 30000 | 最长说话段（超长强制结束） |

VAD 不再产出完整 Utterance，而是每帧 Audio 事件 + Begin/End 边界信号。Pipeline 负责将 32ms 粒度帧批量聚合到 200ms 再送入 ASR。

VAD 模型从 `third_party/silero-vad/` 子模块编译时复制到安装目录。

### 5. 音频流水线 (`src/addon/pipeline/`)

Pipeline 是核心编排器，管理所有队列和工作线程。

```cpp
class Pipeline {
    ThreadSafeQueue<AudioFrame> frameQueue_;       // 捕获 → VAD
    ThreadSafeQueue<SpeechEvent> speechEventQueue_; // VAD → ASR
    ThreadSafeQueue<AsrResult> resultQueue_;        // ASR → 主线程

    unique_ptr<AudioCapture> capture_;   // 捕获后端
    unique_ptr<VADWorker> vadWorker_;    // VAD 工作线程
    unique_ptr<thread> asrThread_;       // ASR 工作线程
    unique_ptr<AsrEngine> asrEngine_;    // ASR 引擎实例
    vector<float> pendingAsrAudio_;      // 200ms 批处理缓冲

    atomic<bool> running_;
    atomic<uint64_t> generation_;  // 与 engine 的 activeGeneration 同步
};
```

**生命周期：**
1. `Init(config)` — 配置 VADWorker，连接队列
2. `SetAsrEngine(engine)` — 设置 ASR 引擎并注册结果回调
3. `Start()` — 启动捕获 → 启动 VADWorker → 启动 ASR 线程
4. `Stop()` — 反向停止：捕获 → VAD → ASR → 清空队列
5. `Abort()` — 暴力终止（析构用），同上完整清理过程

**结果流转：**
```
ASR 引擎回调 → Pipeline 内部闭包 → Push(AsrResult) → ResultQueue
                                                      ↓
engine::OnAsrResult()  → eventDispatcher_.schedule() → PollResults()
                                                      ↓
                                              commitString(text)
```

### 6. ASR 引擎 (`src/addon/asr/`)

```cpp
class AsrEngine {
public:
    struct Config {
        string modelName;
        string apiEndpoint;
        string apiKey;
        string language = "zh";
    };

    virtual bool Init(const Config& config) = 0;
    virtual void Start() = 0;                              // 开始识别 session
    virtual void FeedAudio(const float* pcm, size_t frames) = 0;
    virtual void Stop() = 0;                               // 结束 & 触发最终结果
    virtual const char* Name() const = 0;

    void SetResultCallback(function<void(string text, bool isFinal)> cb);
};
```

**OpenaiCompatAsrEngine（`ASRBackend=openai`，默认）：**
- 构建 WAV 文件（16000Hz mono S16LE）
- HTTP POST multipart/form-data 到 OpenAI 兼容 API
- 独立 worker 线程执行 HTTP 请求
- 支持 Groq / SiliconFlow 等兼容服务
- 依赖 libcurl

**VolcengineStreamingAsrEngine（`ASRBackend=volcengine`）：**
- 使用火山引擎豆包流式语音识别 WebSocket API
- `Start()` 时建立 WebSocket 连接并发送请求首包
- `FeedAudio()` 将 float32 PCM 转为 int16 后推入内部队列
- 独立 worker 线程从队列读取音频，按配置 chunk 大小（100-200ms）实时发送
- 服务端返回 partial 结果时回调 `resultCb_(text, false)`
- 服务端返回 final 结果或连接关闭时回调 `resultCb_(text, true)`
- 每个语音段建立一个 WebSocket 连接，使用完毕后关闭

**线程模型对比：**

| 引擎 | Start | FeedAudio | Stop |
|------|-------|-----------|------|
| OpenAI | 清空 buffer | 追加到 buffer | 启动 HTTP worker → 发送整个 buffer |
| Volcengine | 启动 WS worker → 建连 | 入队 audioChunks_ → WS worker 实时发送 | 发 final 包 → 等 final 响应 → 关闭 WS |

### 7. 线程安全队列 (`src/addon/utils/`)

```cpp
// ThreadSafeQueue<T> — 通用 mutex+cv 队列
// 用于 FrameQueue / UtteranceQueue / ResultQueue
// 支持: Push / TryPop / Pop(阻塞) / Empty / Size / Stop

// AudioRingBuffer — Lock-free SPSC ring buffer
// 仅 PipeWire 内部使用: on_process 回调写(生产者) → DrainLoop 读(消费者)
// float32 数据，固定容量 65536 samples
// 无 Clear() 方法（与 PipeWire 回调存在 data race）
```

**为什么 ThreadSafeQueue 而不是 ring buffer 贯穿全局？**
- ThreadSafeQueue 更通用，支持多生产者多消费者
- VADWorker 和 ASRWorker 需要条件等待（有数据才醒来），mutex+cv 更自然
- Ring buffer 零拷贝优势在 PipeWire 回调场景才真正需要（≤100μs 约束）

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

### 输入法激活与延迟停止

```cpp
void VoiceInputEngine::activate(...) {
    activeIc_ = event.inputContext();
    activeGeneration_++;
    pipeline_->SetGeneration(generation);
    pipeline_->Start();
}

void VoiceInputEngine::deactivate(...) {
    pendingStopGeneration_ = generation;
    // 200ms 延迟停止，期间若重新 activate 则取消
    delayedStopEvent_ = instance_->eventLoop().addTimeEvent(...);
}
```

窗口快速切换时，activate 会取消 pending 的延迟停止任务，避免不必要的重启。

### 结果过滤机制

每轮 activate/deactivate 递增 `activeGeneration_`，pipeline 中每个 AsrResult 携带当前的 `generation_` 值。主线程 `PollResults()` 只提交匹配当前 generation 的结果，避免异步结果错乱。

---

## 线程安全模型

```
主线程 (Fcitx5 事件循环)
    │  队列写入: activeGeneration_, pendingStopGeneration_
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
| VAD 模型加载失败 | VADWorker 不启动，日志错误 |
| ASR HTTP 请求失败 | 丢弃当前段，继续监听下一段 |
| ASR API Key 未配置 | OpenaiCompatAsrEngine Init 返回 false |
| Volcengine 认证失败 | VolcengineStreamingAsrEngine Init 返回 false |
| Volcengine WebSocket 断开 | 丢弃当前段，继续下一个语音段 |
| Volcengine 服务端错误 | 日志打印响应中的 `message` 和 `X-Tt-Logid` |
| OpenAI API 返回空结果 | 丢弃不上屏 |

---

## 构建与打包

### 依赖

```
fcitx5-voice-input
├── fcitx5                      # 输入法框架
├── pipewire-0.3                # PipeWire 音频捕获 (fallback)
├── libpulse-simple             # PulseAudio 音频捕获 (优先)
├── jsoncpp                     # JSON 解析
├── libcurl                     # HTTP/WebSocket 客户端（ASR 必需，>= 7.86.0）
├── zlib                        # Gzip 压缩（火山引擎后端必需）
└── onnxruntime                 # Silero VAD ONNX Runtime
```

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_TESTS` | OFF | 构建测试（目前没有测试源文件） |
| `ONNXRUNTIME_ROOT` | "" | ONNX Runtime 自定义路径 |

### 构建产物

```
build/
└── voice-input-addon.so        # 唯一的输出产物
```

### 打包

- **Arch Linux**: `aur/PKGBUILD`（依赖 fcitx5 / pipewire / jsoncpp / curl / onnxruntime-cpu / zlib）
- **DEB**: CPack 自动生成（依赖 libcurl >= 7.86.0 + zlib）

---

## 项目目录结构

```
fcitx5-voice-input/
├── ARCHITECTURE.md              # ← 本文档
├── README.md
├── LICENSE
├── CMakeLists.txt               # 顶配 CMake（包含所有源文件）
├── AGENTS.md
├── .gitmodules
│
├── src/
│   └── addon/                   # 唯一的代码目录
│       ├── engine.cpp/.h       # VoiceInputEngine Fcitx5 入口
│       ├── types.h             # AudioFrame / Utterance / AsrResult 类型定义
│       ├── voiceinput.conf.in  # Fcitx5 addon 配置模板（@PROJECT_VERSION@ 替换）
│       ├── config/
│       │   ├── config.h              # 桥接头文件
│       │   └── voiceinput-config.h   # FCITX_CONFIGURATION 宏定义
│       ├── capture/
│       │   ├── audio_capture.h              # 捕获后端抽象接口
│       │   ├── pulse_audio_capture.cpp/.h  # PulseAudio 优先
│       │   └── pipewire_capture.cpp/.h     # PipeWire fallback
│       ├── vad/
│       │   ├── silero_vad.cpp/.h  # Silero ONNX 封装
│       │   └── vad.cpp/.h         # VADWorker 状态机
│       ├── pipeline/
│       │   └── pipeline.cpp/.h    # 管道编排（3 队列 + 3 线程）
│       ├── asr/
│       │   ├── asr_engine.h      # 抽象接口（可扩展本地 ASR）
│       │   ├── openai_asr.cpp/.h # OpenAI 兼容 ASR（后端 openai）
│       │   └── volcengine_asr.cpp/.h # 火山引擎豆包 ASR（后端 volcengine）
│       └── utils/
│           ├── audio_buffer.h         # Lock-free SPSC ring buffer
│           └── thread_safe_queue.h    # mutex+cv 通用队列
│
├── third_party/
│   └── silero-vad/              # git submodule，VAD ONNX 模型 + 原始 Python 实现
│
├── po/
│   └── zh_CN.po                # 中文翻译
│
├── aur/
│   └── PKGBUILD                # Arch Linux 打包脚本
│
├── cmake/                       # 自定义 FindXXX.cmake
├── dist/                        # CPack 输出目录
├── build/                       # 构建输出
├── .github/workflows/
│   └── build.yml               # CI: Ubuntu 构建 + CPack DEB + Docker Arch
│
└── docs/                        # （留空）
```

---

## Route Map

### Phase 1: 核心 ✅
- [x] CMake 构建框架 + Fcitx5 Addon 骨架
- [x] PulseAudio + PipeWire 音频捕获
- [x] Silero ONNX VAD 分段
- [x] OpenAI 兼容 API ASR 引擎
- [x] 录音→VAD→ASR→上屏完整流水线
- [x] fcitx5-configtool 配置界面

### Phase 2: 扩展 ⏳
- [ ] 本地 ASR 引擎（通过 AsrEngine 接口扩展）
- [ ] Command 引擎（外部命令云 ASR）
- [ ] LLM 后处理（纠错/翻译/格式化）
- [ ] 场景系统

### Phase 3: 打磨 ⏳
- [ ] 多发行版打包（Debian、Fedora RPM）
- [ ] 热词优化
- [ ] 单元测试

---

## FAQ

### Q: Fcitx5 线程崩溃不就输入法没了？
A: ASR 线程独立运行，崩溃后 pipeline 会自动停止。但当前实现**没有** `std::async` + 超时兜底机制，崩溃会拖垮整个进程。这是已知风险。

### Q: 主线程不是还卡？
A: 主线程只负责事件分发和上屏。音频捕获（实时音频）在独立线程，HTTP ASR 请求（网络 IO 密集型）也在独立线程。唯一的"卡"是主线程的 `PollResults()` 轮询，但只做队列读取和 `commitString()`，微秒级操作。

### Q: 为什么默认用 OpenAI API 而不是本地 ASR？
A: 当前阶段以中文语音输入为主，云端 Whisper 在中文准确率上优于可用的开源离线方案。未来通过 `AsrEngine` 接口扩展本地引擎。

### Q: 没有高级 JSON 配置层了？
A: 是的。`FCITX_CONFIGURATION` 宏已能满足当前所有配置需求（ASR 后端 + OpenAI 参数 + VAD 参数）。将来场景系统需要复杂结构时可能重新引入 JSON 配置。

---

*—— 架构设计 v3，反映实际代码。*
