# 组件详述

> **状态:** 当前实现文档（与代码同步）。本文是 [ARCHITECTURE.md](ARCHITECTURE.md) 的组件级细节，涵盖 engine / config / capture / VAD / pipeline / ASR / 队列。

## 1. 输入法引擎 (`src/addon/engine.cpp/.h`)

```cpp
class VoiceInputEngine : public fcitx::InputMethodEngineV2 {
    // Fcitx5 生命周期
    void reloadConfig() override;     // 重载配置（全局 + 3 个后端子配置）
    void setConfig() override;        // 配置变更时保存并热更新 pipeline
    void activate() override;         // 切换到语音输入 → pipeline.Start()（复用保留的录音流）
    void deactivate() override;       // 切出 → 按事件类型 Pause() 或 Stop()
    void keyEvent() override;         // 按键时提交待上屏文本
    string subModeLabelImpl() override; // 状态文字

    // 结果处理（主线程）
    void OnAsrResult(const string& text);  // 回调 → schedule PollResults()
    void OnVadStatus(bool speaking);       // VAD 起止 → 状态区提示
    void PollResults();                    // 轮询 ResultQueue → commitString()
    void ClearUI();                        // 同步提交/清空原输入框

    // 生命周期
    void DeactivateContext(bool keepCapture); // true=Pause 保流，false=Stop 释放
    void InitializeIfNeeded();        // 延迟初始化 ASR 引擎

private:
    Instance* instance_;
    unique_ptr<Pipeline> pipeline_;
    EventDispatcher eventDispatcher_;
    vector<unique_ptr<HandlerTableEntry<EventHandler>>> eventWatchers_;
    VoiceInputConfig config_;
    OpenAIAsrConfig openaiConfig_;
    VolcengineAsrConfig volcengineConfig_;
    MistralAsrConfig mistralConfig_;
    TrackableObjectReference<InputContext> activeIc_; // 仅主线程访问
    TrackableObjectReference<InputContext> captureOwnerIc_; // 失焦保留，隔离其他上下文的关闭事件
    atomic<uint64_t> activeGeneration_;   // 当前有效 generation
    atomic<uint64_t> sessionGeneration_;  // 当前 session generation（失活置 0）
    bool initialized_ = false;
    string statusText_;
    string pendingPreeditText_;
    uint64_t pendingPreeditUtteranceId_;
};
```

**关键设计：**
- `activeGeneration_` 随每次 activate/deactivate 递增，用于过滤过期结果；`sessionGeneration_` 在失活时置 0，使在途回调与状态更新一并作废。
- 输入框存活由 `watch()` 引用保证可检查；generation 只判断结果归属，不能代替生命周期检查。worker 仅排队值数据，主线程检查 generation 后才读取引用。
- `deactivate()` 在主线程同步提交/清空原输入框的待提交文本并解除引用，避免延迟 UI 回调访问已销毁输入框或把文本提交给新输入框（[ADR-0002](../adr/ADR-0002-input-context-lifetime.md)）。
- 失焦不再使用延迟定时停止：`DeactivateContext()` 按事件类型选择保流（`Pause`）或释放（`Stop`），详见 ARCHITECTURE 的「输入法激活与录音流生命周期」。
- 保流期间只接受采集所属上下文的明确关闭事件，其他上下文即使从 Voice Input 切走也不关闭本流；输入法组变更/退出为全局释放。所属上下文销毁后，回到 Voice Input 重新取得归属；组变更或退出仍可释放。
- `setConfig()` 保存到 `conf/voiceinput.conf`，热更新 pipeline。

## 2. 配置 (`src/addon/config/`)

单层配置，全部通过 fcitx5 `FCITX_CONFIGURATION` 宏定义，由 fcitx5-configtool 可视化编辑。配置按后端分为 4 个子配置块（`fcitx://config/addon/voiceinput/asr/...` 子路径）：

```
src/addon/config/
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
| `ApiMode` | Enum | `whisper` | whisper（批量）/ chat（OpenAI 兼容 Chat Completions 音频）/ realtime（流式） |
| `EnableItn` | Bool | `true` | chat 模式是否发送 DashScope 扩展字段 `asr_options.enable_itn`；仅接受 `language` 的端点（如小米 MiMo）应关闭 |
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

音频设备无手动配置项：插件不枚举、筛选、指定或保存物理录音设备，路由由系统音频服务与会话管理器负责（默认输入或该应用的设备偏好，见 capture 章节）。无高级 JSON 配置层。

## 3. 音频捕获 (`src/addon/capture/`)

```cpp
// 捕获后端抽象接口
class AudioCapture {
public:
    virtual ~AudioCapture() = default;
    virtual bool Start() = 0;      // 开始捕获，返回成功失败
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual const char* Name() const = 0;
    // 主线程：静音取帧，但不关闭音频服务可见的录音流（epoch 门闸）
    void SetDeliveryEnabled(bool enabled);
    virtual void SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue);
};
```

**PulseAudio 后端（优先）：**
- 使用 `libpulse-simple` 同步读取 API
- 创建独立线程 `CaptureLoop()` 循环读取
- 兼容传统 PulseAudio 和 pipewire-pulse 模拟层
- `pa_simple_new` 的 `dev=nullptr`：不指定物理录音源，路由交给音频服务/音频管理器（默认源或按应用偏好）；不再执行 `pactl` 枚举、名称过滤或排序猜测

**PipeWire 后端（fallback）：**
- 使用 `pw_thread_loop` + `pw_stream` 实时音频回调
- 无显式 target（`PW_ID_ANY` + `AUTOCONNECT`），默认路由交给会话管理器
- `on_process` 回调（≤100μs）只写 lock-free ring buffer
- 独立 `DrainLoop()` 线程从 ring buffer 读取 → 封装 AudioFrame → FrameQueue
- 不依赖 WirePlumber 的特定版本

**取帧门闸（Pause 语义）：** `SetDeliveryEnabled(false)` 立刻封住取帧入口；已在阻塞中的读取完成后也不会入队，且以 epoch 校验拒绝跨越一次 pause/resume 的旧帧。这样失焦后不会再有新的应用帧进入 VAD/ASR。两个后端都遵循同一门闸。

PipeWire 的生产者 epoch 在每个已完成回调出口发布（包括空 buffer）；取得的 buffer 先归还再发布。Drain 在屏障后有界清 ring 与组帧，避免旧回调补写或空回调导致恢复后首帧丢失。

**选择策略：** Pipeline 中先尝试 PulseAudio，失败后 fallback 到 PipeWire 直连。

## 4. VAD (`src/addon/vad/`)

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

模型实例跨会话复用：切换输入法不重建 ONNX Session；每次 `Start()` 都调用 `Reset()` 清空 Silero 内部状态并重置分段状态机，避免复用上次残留的 context。

VAD 模型从 `third_party/silero-vad/` 子模块编译时复制到安装目录。

## 5. 音频流水线 (`src/addon/pipeline/`)

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
3. `Start()` — 复用保留的 capture（若已失效则重建）；清空队列并重启结果协调器；启动 VADWorker → 启动 AsrDispatcherLoop → 打开取帧门闸（`SetDeliveryEnabled(true)`）
4. `Pause()` — 先关闭取帧门闸，再停止识别（VAD → ASR 分发），取消当前会话与引擎全部会话，清空队列并作废 generation；**保留**可路由的录音流
5. `Stop()` — `Pause()` + `capture_->Stop()` 释放录音流
6. `Abort()` — `results_->Close()` + `Stop()`（析构用）

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

## 6. ASR 引擎 (`src/addon/asr/`)

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
| `OpenaiAsrEngine` | openai | OpenAI 兼容 API | `apiMode=whisper` 批量 multipart WAV；`chat` Chat Completions（DashScope qwen3-asr-flash、小米 MiMo mimo-v2.5-asr 等，请求体由 `asr/utils/chat_asr_request.h` 构造）；`realtime` GPT Realtime WS 流式（16k→24k 线性上采样、周期 commit 兜底） |
| `VolcengineAsrEngine` | volcengine | 火山豆包 WS | 200ms chunk 实时发送，服务端 partial/final 回调；ITN/标点/DDC/二次识别选项 |
| `MistralAsrEngine` | mistral | Mistral Realtime WS | 16kHz PCM 直推（无需重采样）；`transcription.text.delta` 增量 preedit；周期 flush + 断线重连（保持同一 sessionId）；`TargetStreamingDelayMs` 权衡延迟/准确性 |

- OpenAI `realtime` 与 Mistral 的流式会话在 `StartWorker()` 即建立持久 WS 连接，断线自动重连并保持 sessionId
- Realtime 的终态判定按服务端确认的 `item_id` 追踪在途 item（`utils/realtime_commit_tracker.h`）：`error` 事件不再造成计数漂移，最终 `completed` 到达即上屏；`error` 事件的内容（message/code）进入日志与错误回调；End 等待以「距最后服务端事件 5s」的空闲超时为上界（见错误处理策略表）
- 三个流式 WS 后端共用 `src/addon/asr/utils/ws_frame_receiver.h` 的 `WsFrameReceiver` + `ReceiveWsMessage()` 收包：跨 TCP 分片（含 WS `CURLWS_CONT`）的事件消息跨调用保留已收前缀，单条累计超过 16MB 时整条丢弃且在消息边界恢复解析（`Skipped` 不触发重连）
- Realtime/豆包的 `asr/protocol/` 模块先校验已知事件再转换为会话事件，不把 JsonCpp 对象传入编排层；错误类型、非法 gzip 或过深结果立即失败并发布一次空 final，不重连。两者共用 worker 异常边界与 curl 资源所有权，异常路径也设置 `finished/workerDone`；final callback 抛异常不重试（[ADR-0003](../adr/ADR-0003-ws-session-failure-boundary.md)）。Mistral 的同类防护仍是独立后续任务。
- 发送与建连同样有界：`src/addon/asr/utils/ws_deadline.h` 的 `WsDeadline`（单次发送预算，默认 10s）、`WsEndBudget`（End 路径总预算，默认 8s）与 `WsAbort`（`cancelled` 始终生效，`finished` 用于非 End 路径的推流与建连）；`ws_frame_sender.h` 的 `SendWsMessage()` 在 `CURLE_AGAIN`/零进展时按预算失败。上游不读 socket 时 `End()` 在 End 预算内收敛并交出兜底终态，`SessionReaper::JoinWithTimeout(15s)` 不再超时，且 `End()` 后不再重连
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

## 7. 线程安全队列 (`src/addon/utils/` + `src/addon/pipeline/ordered_result_buffer.h`)

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
// 停滞放行：某段超过 maxStall（默认 40s）无新进展/阻塞后续段时，清掉该段缓存并
// 下推一条 isError 超时结果，随后推进闸门；Pipeline 分发循环每 500ms 检查一次
// （被阻塞段可能不再有任何回调，需要主动检查），避免单段故障演变为整链路静默
```

**为什么 ThreadSafeQueue 而不是 ring buffer 贯穿全局？**
- ThreadSafeQueue 更通用，支持多生产者多消费者
- VADWorker 和 ASRWorker 需要条件等待（有数据才醒来），mutex+cv 更自然
- Ring buffer 零拷贝优势在 PipeWire 回调场景才真正需要（≤100μs 约束）
