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

单层配置，全部通过 fcitx5 `FCITX_CONFIGURATION` 宏定义，由 fcitx5-configtool 可视化编辑。配置按后端分为 4 个子配置块（`fcitx://config/addon/voiceinput/asr/...` 子路径）：

```
src/addon/config/
├── config.h                  # 桥接头文件 → voiceinput-config.h
└── voiceinput-config.h       # FCITX_CONFIGURATION 宏定义全部配置键
```

**VoiceInputConfig（全局）：**

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `ActiveBackend` | Enum | `openai` | ASR 后端（openai / volcengine / mistral） |
| `VADThreshold` | Int 0-100 | `20` | VAD 阈值百分比 |
| `SilenceThresholdMs` | Int | `800` | 静音超时毫秒数 |
| `StartFrames` | Int 1-10 | `2` | 连续几帧说话触发 onset |
| `PreRollMs` | Int 0-1000 | `300` | onset 前保留音频 |
| `MinSpeechMs` | Int | `300` | 最短说话段（太短丢弃） |
| `MaxSpeechMs` | Int | `30000` | 最长说话段（超长强制结束） |

**OpenAIAsrConfig（`ActiveBackend=openai`）：**

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `BaseUrl` | String | `https://api.openai.com/v1` | OpenAI 兼容 API Endpoint |
| `ApiKey` | String | `""` | API Key |
| `Model` | String | `whisper-1` | 模型名 |
| `Language` | Enum | `auto` | 输出语言（auto / en / zh） |
| `ApiMode` | Enum | `whisper` | whisper（批量）/ chat / realtime（流式） |
| `CommitIntervalMs` | Int 1000-30000 | `5000` | realtime 周期 commit 兜底间隔 |
| `LLMEnabled` | Bool | `false` | 启用 LLM 后处理 |
| `LLMModel` | String | `""` | 后处理 LLM 模型 |
| `LLMSystemPrompt` | String | `""` | 后处理系统提示词 |
| `LLMStream` | Bool | `true` | LLM 流式输出 |
| `AutoCommit` | Bool | `true` | 无 LLM 时自动上屏 |

**MistralAsrConfig（`ActiveBackend=mistral`）：**

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `BaseUrl` | String | `https://api.mistral.ai/v1` | API Endpoint |
| `ApiKey` | String | `""` | API Key |
| `Model` | String | `voxtral-mini-transcribe-realtime-2602` | 语音模型 |
| `CommitIntervalMs` | Int | `5000` | 实时提交间隔 |
| `TargetStreamingDelayMs` | Int 0-1000 | `0` | 流式延迟目标（trade-off 准确性） |

**VolcengineAsrConfig（`ActiveBackend=volcengine`）：**

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `Endpoint` | String | `wss://openspeech.bytedance.com/...` | WebSocket 地址 |
| `AuthMode` | Enum | `api_key` | api_key / app_access_key |
| `ApiKey` / `AppKey` / `AccessKey` | String | `""` | 认证凭据 |
| `ResourceId` | String | `volc.seedasr.sauc.duration` | 资源 ID |
| `ChunkMs` | Int 100-200 | `200` | 音频分片（毫秒） |
| `EnableITN` / `EnablePunc` / `EnableDDC` / `EnableNonstream` | Bool | 见代码 | ITN / 标点 / 语义顺滑 / 二次识别 |
| `EndWindowMs` | Int | `800` | 判停窗口（毫秒） |

`AudioSource` 通过 `pactl list sources short` 动态枚举 PulseAudio 输入设备（排除 monitor source，见 capture 章节）。无高级 JSON 配置层。

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

| 参数 | 默认值 | 配置键 | 说明 |
|------|--------|--------|------|
| `speechThreshold` | 0.2 | `VADThreshold`（20%） | 说话判定阈值（= vadThreshold/100） |
| `silenceThreshold` | 0.14 | —（固定） | 静音判定阈值（= speechThreshold × 0.7） |
| `startFrames` | 2 | `StartFrames` | 连续几帧说话触发 onset |
| `preRollMs` | 300 | `PreRollMs` | onset 前保留音频（毫秒） |
| `endSilenceMs` | 800 | `SilenceThresholdMs` | 连续静音多久结束说话段 |
| `minSpeechMs` | 300 | `MinSpeechMs` | 最短说话段（太短丢弃） |
| `maxSpeechMs` | 30000 | `MaxSpeechMs` | 最长说话段（超长强制结束） |

VAD 不再产出完整 Utterance，而是每帧 Audio 事件 + Begin/End 边界信号。Pipeline 负责将 32ms 粒度帧批量聚合到 200ms 再送入 ASR。

VAD 模型从 `third_party/silero-vad/` 子模块编译时复制到安装目录。

### 5. 音频流水线 (`src/addon/pipeline/`)

Pipeline 是核心编排器，管理所有队列和工作线程（v4 会话模型，详见 [v4-asr-session-model.md](v4-asr-session-model.md)）。

```cpp
class Pipeline {
    ThreadSafeQueue<AudioFrame> frameQueue_{1024};        // 捕获 → VAD（≈32s 缓冲）
    ThreadSafeQueue<SpeechEvent> speechEventQueue_{256};  // VAD → ASR（≈256 段）

    unique_ptr<VADWorker> vadWorker_;         // VAD 工作线程
    unique_ptr<thread> asrThread_;            // AsrDispatcherLoop
    unique_ptr<AudioCapture> capture_;        // 捕获后端

    shared_ptr<AsrEngine> asrEngine_;         // ASR 引擎工厂（会话由 Dispatcher 创建）
    shared_ptr<AsrSession> activeSession_;    // 当前语音段会话
    unique_ptr<SessionReaper> reaper_;        // 完成会话回收线程
    shared_ptr<ResultCoordinator> results_;   // 结果保序 + LLM 后处理 + generation 过滤

    atomic<bool> running_;
    atomic<uint64_t> generation_;             // 与 engine 的 activeGeneration 同步
    atomic<uint64_t> utteranceCounter_;       // 语音段序号（保序依据）
};
```

**生命周期：**
1. `Init(config)` — 配置 VADWorker、ResultCoordinator，连接队列
2. `SetAsrEngine(engine)` — 设置 ASR 引擎工厂并注册结果回调
3. `Start()` — 启动捕获 → 启动 VADWorker → 启动 AsrDispatcherLoop
4. `Stop()` — 反向停止：捕获 → VAD → ASR → 清空队列
5. `Abort()` — 暴力终止（析构用），同上完整清理过程

**AsrDispatcherLoop（单分发线程）：** 消费 SpeechEventQueue，把 VAD 语音段映射为会话生命周期：

```
Begin  → asrEngine_->StartSession() → activeSession_ = session（容量满则引擎自动取消最旧会话，kMaxActiveSessions=3）
Audio  → 32ms 帧聚合到 200ms 批 → activeSession_->FeedAudio()
End    → activeSession_->End()（触发最终结果）→ 交给 SessionReaper 回收
Cancel → activeSession_->Cancel()
```

**结果流转：**
```
会话回调 → ResultCoordinator::HandleAsrResult(text, isFinal, sessionId)
              │  OrderedResultBuffer 按 utteranceId 保序
              │  （可选）LLMClient 后处理
              ▼
        Push(AsrResult) → ResultQueue
              ▼
engine::OnAsrResult() → eventDispatcher_.schedule() → PollResults()
              ▼
        commitString(text)  （仅当 generation 匹配当前激活）
```

`ResultCoordinator` 持有每组语音的 `SessionMetadata`（generation + utteranceId），保证并发会话的最终结果按创建顺序交付；`SessionReaper` 用 `JoinWithTimeout` 回收已结束会话的 worker 线程，杜绝僵尸线程。

### 6. ASR 引擎 (`src/addon/asr/`)

v4 起 ASR 抽象拆为「引擎工厂 + 会话」两层：

```cpp
// 会话抽象 — 一句话语音的生命周期
class AsrSession {
    virtual void FeedAudio(const float* pcm, size_t frames) = 0;
    virtual void End() = 0;
    virtual void Cancel() = 0;
    virtual void StartWorker() = 0;   // 必须已存在 shared_ptr 才可调用
    virtual void JoinWithTimeout(chrono::milliseconds timeout) = 0;
    shared_ptr<State> GetState();     // cancelled / finished / workerDone / sessionId
    void SetResultCallback(function<void(text, isFinal, sessionId)> cb);
};

// 引擎工厂 — 全局单例，由 Pipeline 持有；weak_ptr 追踪活跃会话
class AsrEngine {
    static constexpr size_t kMaxActiveSessions = 3;
    virtual bool Init(const Config& config) = 0;   // 可多次调用热更新
    virtual AsrSessionStart StartSession() = 0;    // 容量满时返回被取消的最旧会话 ID
    virtual void CancelAllSessions() = 0;
    virtual const char* Name() const = 0;
};
```

**引擎实现：**

| 引擎 | 后端 | 协议 | 特点 |
|------|------|------|------|
| `OpenaiAsrEngine` | openai | OpenAI 兼容 API | `apiMode=whisper` 批量 multipart WAV；`chat` Chat Completions；`realtime` GPT Realtime WS 流式（16k→24k 线性上采样、周期 commit 兜底） |
| `VolcengineAsrEngine` | volcengine | 火山豆包 WS | 200ms chunk 实时发送，服务端 partial/final 回调；ITN/标点/DDC/二次识别选项 |
| `MistralAsrEngine` | mistral | Mistral Realtime WS | 16kHz PCM 直推（无需重采样）；`transcription.text.delta` 增量 preedit；周期 flush + 断线重连（保持同一 sessionId）；`TargetStreamingDelayMs` 权衡延迟/准确性 |

- OpenAI `realtime` 与 Mistral 的流式会话在 `StartWorker()` 即建立持久 WS 连接，断线自动重连并保持 sessionId
- Volcengine 每个语音段建立独立 WebSocket 连接，使用完毕后关闭
- `SessionReaper`（独立回收线程）对完成/取消的会话做超时 join，防止僵尸线程（详见 [v4-asr-session-model.md](v4-asr-session-model.md)）
- `LLMClient`（`src/addon/llm/`）：可选 LLM 后处理（纠错/格式化），支持流式与非流式，按 generation 取消过期请求

**线程模型对比（会话视角）：**

| 后端 | 建连时机 | FeedAudio | End |
|------|---------|-----------|-----|
| OpenAI whisper/chat | End 后才启动 HTTP worker | 追加到会话 buffer | 启动 worker 发送整段 |
| OpenAI realtime | StartWorker 即建 WS | 重采样 24k → base64 推送 | 发 `input_audio_buffer.commit` → 收 final |
| Volcengine | StartWorker 即建 WS | 入队 → 按 chunkMs 实时发送 | 发结束包 → 等 final → 关 WS |
| Mistral | StartWorker 即建 WS | int16 → base64 直推 | 周期 flush / `transcription.done` |

### 7. 线程安全队列 (`src/addon/utils/` + `src/addon/pipeline/ordered_result_buffer.h`)

```cpp
// ThreadSafeQueue<T> — 通用 mutex+cv 队列（有界，Push 满时丢弃最旧）
// 用于 FrameQueue(1024) / SpeechEventQueue(256)
// 支持: Push / TryPop / Pop(阻塞) / Empty / Size / Stop

// AudioRingBuffer — Lock-free SPSC ring buffer
// 仅 PipeWire 内部使用: on_process 回调写(生产者) → DrainLoop 读(消费者)
// float32 数据，固定容量 65536 samples
// 无 Clear() 方法（与 PipeWire 回调存在 data race），清空用 Read() drain 模式

// OrderedResultBuffer — 并发会话结果按语音段创建顺序重排（ResultCoordinator 内部）
// 以 utteranceId 为序键，序号不连续/乱序到达时先暂存后交付
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
| 录音库缺失/soname 变更（dlopen 失败） | 对应后端优雅降级，addon 本体不受影响 |
| VAD 模型加载失败 | VADWorker 不启动，日志错误 |
| ASR HTTP 请求失败 | 丢弃当前段，继续监听下一段 |
| ASR API Key 未配置 | 对应引擎 Init 返回 false |
| Volcengine 认证失败 / WS 断开 | Init 返回 false；断开则丢弃当前段继续下一段 |
| Volcengine 服务端错误 | 日志打印响应中的 `message` 和 `X-Tt-Logid` |
| OpenAI realtime / Mistral WS 断线 | 自动重连（保持同一 sessionId），周期 commit/flush 兜底 |
| 会话 worker 未按时退出 | SessionReaper `JoinWithTimeout` 超时后丢弃，不阻塞清理 |
| 结果乱序/迟到（并发会话） | OrderedResultBuffer 按 utteranceId 保序，旧 generation 结果丢弃 |
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

> 录音库（libpulse-simple/libpipewire）**不链接**（无 DT_NEEDED）：各 capture 后端在 Start 时 dlopen + dlsym 函数指针表延迟加载，库缺失/soname 变更/符号不完整时优雅降级，addon 本体不受影响（详见过往 PR：Arch 构建 `-z,now` 兼容）。

### 构建选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `BUILD_TESTS` | OFF | 构建测试（目前没有测试源文件） |
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
│       │   ├── config.h                    # 桥接头文件
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
│       │   └── utils/                           # ASR 内部工具（重采样/base64 等）
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
- [x] LLM 后处理（flow/非流式，generation 取消）
- [x] fcitx5-configtool 配置界面（多后端子配置）
- [x] 多发行版打包（7 发行版 CI 矩阵：DEB / RPM / pkg.tar.zst）

### 待实现 ⏳
- [ ] 本地 ASR 引擎（sherpa-onnx 子模块已预留，未接入构建）
- [ ] Command 引擎（外部命令云 ASR）
- [ ] 场景系统
- [ ] 热词优化
- [ ] 单元测试

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
A: 不会。v4 的 `ResultCoordinator + OrderedResultBuffer` 以 utteranceId 为序键重排并发会话的最终结果，并按 generation 过滤过期激活；连续语音段的最终结果永远按说话顺序上屏。

---

*—— 架构设计 v4，反映实际代码（与 `src/addon/` 同步）。*

## 相关文档

- [v4 会话模型设计](v4-asr-session-model.md)（AsrSession 抽象与线程模型的详细设计）
- [ADR-0001：GPT-Realtime 流式实时转录](../adr/ADR-0001-gpt-realtime-asr.md)
- [架构评审（2026-07-05）](../reviews/2026-07-05-architecture-review.md)
