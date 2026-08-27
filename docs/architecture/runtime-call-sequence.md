# fcitx5-voice-input 运行时调用序列

> **状态:** 与代码同步的实现文档。
> **关联:** [ARCHITECTURE.md](ARCHITECTURE.md)（总体架构）、[v4-asr-session-model.md](v4-asr-session-model.md)（会话模型）。
> **目的:** 把「录音 → VAD 分段 → ASR 转写 → LLM 后处理 → 提交文本」整条数据流，按线程与时间轴展开为可追溯的调用时序。

---

## 1. 一句话链路

```
录音 ─▶ FrameQueue ─▶ VAD(Silero) ─▶ SpeechEvent ─▶ ASR后端(OpenAI/Realtime/火山)
   │                                                                          │
   └───────────────────── AudioFrame ◀── DrainLoop ◀── on_process ◀── PipeWire ──┘
                                                                          ▼
                                              ResultQueue ─▶ 主线程 eventDispatcher ─▶ commit / preedit
```

一次用户说话，数据沿此链路单向流动；回传路径（ASR 结果 → 主线程）通过 `generation` 与 `utteranceId` 做隔离和排序。

---

## 2. 线程模型

| 线程 | 职责 | 关键约束 |
|---|---|---|
| **主线程** | Fcitx5 事件循环、`eventDispatcher`、`PollResults`、UI 更新 | 只做 UI 提交，绝不阻塞 |
| **VADWorker** | 消费 `FrameQueue`，跑 Silero ONNX，产出 `SpeechEvent` | 独立状态机（Idle/Speaking） |
| **AsrDispatcherLoop** | 消费 `SpeechEventQueue`，起/停 ASR session worker，跑 LLM | 唯一的 ASR 结果生成者 |
| **Session worker × N** | 每个活跃 session 一个 curl/WS 转写线程 | ≤3 并发（`kMaxActiveSessions`） |
| **SessionReaper** | 后台 `JoinWithTimeout(15s)` 回收 session worker | 可能晚于管线析构，靠 `resultGuard_` 检查 |
| **Capture thread** | Pulse: 1 个；PipeWire: `on_process` 回调线程 + `DrainLoop` 线程 | `on_process` ≤100μs 不阻塞 |

> 参考时序图 3 中的并发上限与回收逻辑。

---



## 3. 三种 ASR 模式的关键差异

| 维度 | OpenAI whisper | OpenAI Realtime | 火山引擎豆包 |
|---|---|---|---|
| 传输 | HTTP multipart WAV | WebSocket 流 | WebSocket 二进制帧(gzip) |
| 触发 | VAD End 后一次性转写 | 建立持久会话，流式 append | 同上，流式 |
| preedit | 无（整句 final） | delta 实时刷新 | utterances 增量 |
| 重连 | 无 | 断线重连保 sessionId，30min 上限 | — |
| 并发单元 | 每句一个 session | 每句一个 session | 每句一个 session |

---

## 4. 详细时序图

### 4.1 单次语音段完整流程（OpenAI Whisper 模式）

```mermaid
sequenceDiagram
    participant FM as Fcitx5 主线程
    participant IC as InputContext
    participant PE as Pipeline (AsrDispatcherLoop)
    participant VAD as VADWorker
    participant CAP as Capture (Pulse/PW)
    participant SE as OpenaiAsrSession
    participant API as OpenAI API

    Note over FM,API: ===== 阶段0: 激活（activate） =====
    FM->>PE: activate() → Pipeline::Start()
    PE->>CAP: StartCapture() → dlopen + 开录音流
    CAP->>CAP: 采集线程 loop 读取麦克风
    CAP->>PE: Push AudioFrame → FrameQueue

    Note over FM,API: ===== 阶段1: VAD 分段 =====
    loop 每 32ms (一帧)
        PE->>VAD: TryPop AudioFrame
        VAD->>VAD: Silero ONNX predict()
        alt 概率 >= speechThreshold
            VAD->>PE: Push SpeechEvent{Begin}
            VAD->>VAD: speechFrames 计数
        end
        VAD->>PE: Push SpeechEvent{Audio}  (含 int16 pcm)
        VAD->>VAD: 累计静音帧 silenceFrames++
    end

    Note over FM,API: ===== 阶段2: Dispatcher 起 session =====
    PE->>PE: TryPop Begin
    PE->>PE: 取消旧 session / 检查 ≤3 并发
    PE->>AsrEngine: StartSession() → sid
    PE->>SE: StartWorker() (whisper 不发起请求,仅占位)
    PE->>PE: sessionGenerationMap_[sid] = {gen, uid}
    PE->>SE: StartWorker() 再次调用 → 起 curl worker 线程

    Note over FM,API: ===== 阶段3: 喂音频 + 攒批 =====
    loop 说话中
        PE->>VAD: TryPop Audio
        PE->>PE: 转 float → pendingAsrAudio 累积
        alt pending ≥ 200ms 样本
            PE->>SE: FeedAudio(批次, ~200ms)
        end
    end

    Note over FM,API: ===== 阶段4: VAD 结束 + 触发转写 =====
    VAD->>VAD: 静音或超时 → duration ≥ minSpeechMs
    VAD->>PE: Push SpeechEvent{End}
    PE->>SE: End() → state.finished=true
    SE->>SE: 起 TranscribeWorker 线程
    SE->>SE: 攒 buffer → 构建 WAV (float→int16)
    SE->>API: POST audio/transcriptions (multipart WAV)
    alt 被取消
        API-->>SE: (传输中断)
    else 成功
        API-->>SE: JSON{ "text": "..." }
    end
    SE->>PE: resultCb_(text, isFinal=true, sid)

    Note over FM,API: ===== 阶段5: 有序缓冲 + LLM + 回主线程 =====
    PE->>PE: SetResultCallback 回调
    PE->>OResult: SubmitOrderedResult(result, terminal=true)
    OResult-->>PE: 按 uid 排序后 Push → ResultQueue
    PE->>PE: isFinal 且无 LLM → 直接回调
    alt LLM 启用
        PE->>LLM: Process(text) → chat/completions
        LLM-->>PE: 修正后文本
        PE->>PE: result.isLLMRefined=true
    end
    PE->>FM: OnAsrResult → eventDispatcher schedule

    Note over FM,API: ===== 阶段6: 主线程提交 =====
    FM->>FM: PollResults() 过滤当前 generation
    alt autoCommit
        FM->>IC: commitString("...")  → 提交
    else 仅 preedit
        FM->>IC: setPreedit("...") → 显示
    end
    FM->>FM: 更新 InputPanel / StatusArea
```

### 4.2 Realtime 流式模式（长句实时）

```mermaid
sequenceDiagram
    participant PE as Pipeline
    participant SE as RealtimeAsrSession
    participant WS as OpenAI Realtime (wss)
    participant VAD as VADWorker

    Note over PE,WS: 16k int16 → 24k float 线性上采样 (3/2) → 24k int16
    Note over PE,WS: kAppendChunkSamples≈2400 (~100ms@24k)

    VAD->>PE: Push Begin
    PE->>SE: StartSession + StartWorker()
    SE->>WS: session.update (language/model)

    Note over PE,WS: 循环: 取音频 chunk
    loop 持续说话
        PE->>SE: FeedAudio → audioChunks_ 队列
        SE->>SE: 16k→24k 上采样 → pending24k
        alt pending24k ≥ 2400 样本
            SE->>WS: input_audio_buffer.append (base64)
        end
        Note right of SE: lastCommitTime 不刷新
    end

    Note over PE,WS: 周期 commit 兜底
    alt 距上次 commit ≥ commitIntervalMs 且 append 过
        SE->>WS: input_audio_buffer.commit
        SE->>SE: commitsInFlight++
    end

    Note over PE,WS: 服务端事件处理
    loop 每 20ms handleServer
        WS-->>SE: transcription.delta
        SE->>SE: currentTranscript += delta
        SE->>PE: cb(partial, false) → preedit 刷新整句
    end

    Note over PE,WS: 长句无停顿
    alt 持续说话,无 delta
        SE->>WS: commit (兜底,保证出增量)
    end

    VAD->>PE: Push End
    PE->>SE: End() → 起 worker
    SE->>WS: flush pending24k (尾部)
    SE->>WS: commit (最终段)
    Note over SE: endCommitSent=true

    loop 等待最终 completed
        WS-->>SE: transcription.completed
        SE->>SE: 判定 isFinalItem = endCommitSent && commitsInFlight≤1
        alt 是最终 item
            SE->>PE: cb(fullTranscript, true) → final
        else 周期 commit
            SE->>PE: cb(fullTranscript, false) → partial
        end
    end
    SE->>SE: gotFinal=true → 退出
    SE->>WS: WS close
```

### 4.3 会话生命周期 & 并发控制（deactivate / generation 隔离）

```mermaid
sequenceDiagram
    participant FM as Fcitx5 主线程
    participant PE as Pipeline
    participant RE as SessionReaper
    participant SE as AsrSession worker

    Note over FM,RE: ===== 并发上限 =====
    activate PE
    loop 每次 Begin
        PE->>AsrEngine: StartSession()
        AsrEngine-->>PE: ≤3 并发,否则 CancelOldestSession
    end

    Note over FM,RE: ===== deactivate: 分级停止 =====
    FM->>PE: deactivate() → generation++
    PE->>PE: 安排 200ms 延迟 Stop 事件
    Note right of PE: 200ms 内未 deactivate 则取消

    FM->>FM: 用户按键 → keyEvent()
    FM->>FM: commitString(pendingPreedit) 立即提交
    FM->>PE: (延迟 Stop 被取消)

    Note over FM,RE: ===== Stop: 级联停止 =====
    FM->>PE: 200ms 后 Stop()
    PE->>PE: running_=false
    PE->>CAP: capture_->Stop() (Pulse 先 join 阻塞线程)
    PE->>VAD: VADWorker::Stop() (join worker)
    PE->>PE: activeSession_->Cancel()
    PE->>RE: reaper_->Add(session)
    PE->>AsrEngine: engine->CancelAllSessions()
    PE->>LLM: llmClient_->Cancel()
    PE->>PE: 排空 frame/speech/result 队列 (防幽灵)

    Note over FM,RE: ===== Reaper 回收 =====
    activate RE
    loop 后台
        RE->>SE: JoinWithTimeout(15s)
        SE-->>RE: 线程 join 完成
        RE->>RE: pending_--
    end

    Note over FM,RE: ===== 异步回调安全 =====
    rect rgb(#e8f5e9)
        Note over PE: resultGuard_ 已置 false → 丢弃
        SE->>PE: resultCb_(text,...) [可能晚于析构]
        PE->>PE: guard->load()==false → return
    end

    Note over FM,RE: ===== generation 隔离 =====
    rect rgb(#fff3e0)
        Note over FM: 下次 activate → generation++
        PE->>PE: 所有结果带旧 generation
        FM->>FM: PollResults 过滤 gen≠activeGeneration
        FM->>FM: 旧结果被丢弃,仅当前会话生效
    end
```

---

## 5. 关键数据流与并发保护点

| 环节 | 传递载体 | 关键并发保护 |
|---|---|---|
| 采集→VAD→Dispatcher | `FrameQueue` / `SpeechEventQueue`（mutex+cv） | SPSC 生产者-消费者 |
| Dispatcher→后端 | `engine->StartSession/FeedAudio/End/Cancel` | `engineMutex_` 保护 `sessions_` |
| 后端→Pipeline | `resultCb_(text,isFinal,sid)` | `SessionReaper` `JoinWithTimeout` |
| 乱序回调→主线程 | `OrderedResultBuffer`（按 `utteranceId`） | 排序保证 UI 顺序 |
| 异步回调 vs 析构 | `resultGuard_` + `sessionGenerationMap_` | 双重校验丢弃残留 |
| 跨会话切换 | `generation` 原子计数 | `PollResults` 过滤幽灵结果 |

### 5.1 generation 隔离详解

每次 `activate`/`deactivate` 都 `fetch_add` 递增 `generation`。所有 `AsrResult` 携带 `generation`，主线程 `PollResults()` 用 `activeGeneration_` 过滤。这样快速切换输入法时，上一个会话的在途回调（VAD 结束延迟、LLM 慢响应、Reaper 回收）全部被静默丢弃，不会污染当前输入。

### 5.2 异步回调安全

`SessionReaper` 可能晚于 `Pipeline` 析构退出（超时 15s）。其 `resultCb_` 回调前先检查 `resultGuard_`，若已置 `false` 则直接 return，避免访问已析构的 `Pipeline`（use-after-free）。

### 5.3 OrderedResultBuffer

并发 session 的回调可能乱序到达。`OrderedResultBuffer` 以 `utteranceId` 为 key 按序归并（partial 同段覆盖），仅在 `utteranceId == nextUtteranceId_` 时交付主线程，保证 UI 上屏顺序与语音段顺序一致。

---

## 6. 与 ARCHITECTURE.md 的关系

本文件聚焦**运行期调用时序**（谁在什么时刻调用谁），补充 ARCHITECTURE.md 的静态架构图，与 v4 会话模型文档互相印证。涉及的关键实现：

- 采集后端 dlopen 运行时加载、优雅降级 —— 见 capture 代码
- Silero VAD 状态机与 pre-roll —— 见 vad 代码
- 三种后端 WS/HTTP 协议细节 —— 见 asr 代码
- LLM 后处理（非流式 `Process` / 流式 `ProcessStream`）—— 见 llm_client 代码
