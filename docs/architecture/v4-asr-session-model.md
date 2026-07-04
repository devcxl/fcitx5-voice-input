# fcitx5-voice-input 架构设计 v4

> **状态:** 提案文档。对应代码位于 feature/volcengine-streaming-asr 分支。
> **上一版:** [ARCHITECTURE.md](/ARCHITECTURE.md) (v3)

---

## 设计原则

| 原则 | 含义 |
|------|------|
| **Pipeline 从不阻塞** | 所有 I/O（HTTP/WS/本地推理）跑在引擎自有线程上，Pipeline 是纯事件分发器 |
| **会话即对象** | 每句语音是一个 `AsrSession`，有明确的创建/Feed/结束生命周期，不跟引擎绑定 |
| **接口表达语义，不表达协议** | `AsrSession` 只有 `FeedAudio`/`End`/`Cancel`，不知道什么是 WS、HTTP、本地解码 |
| **有限资源边界** | 所有异步线程有硬超时保障，不会变成僵尸 |

---

## 整体架构

```
Fcitx5 进程 (voice-input-addon.so)
│
├── [主线程] Fcitx5 事件循环
│   ├── activate/deactivate → pipeline.Start/Stop
│   ├── PollResults → commitString
│   └── setConfig → 热更新
│
├── [捕获线程] PulseAudio/PipeWire → AudioFrame → FrameQueue
│
├── [VAD 线程] FrameQueue → Silero ONNX → SpeechEvent (Begin/Audio/End/Cancel)
│
├── [ASR 分发线程] SpeechEvent → AsrSession (非阻塞)
│   ├── Begin   → asrEngine->StartSession() → 返回 unique_ptr<AsrSession>
│   ├── Audio   → session->FeedAudio(pcm, frames)
│   ├── End     → session->End()
│   └── Cancel  → session->Cancel()
│
└── [ASR 引擎自有线程] (每句语音可能有独立线程)
    ├── OpenAI:  HTTP POST worker (积累→发送→等待→回调)
    ├── Volcengine: WS worker (握手→流式发送→接收→回调)
    └── 未来: 本地解码器线程 / 其他 WS 客户端线程
```

**关键变化（对比 v3）：**

| v3 | v4 |
|----|----|
| `AsrEngine` + `Start/FeedAudio/Stop` 绑死在一起 | `AsrEngine` 工厂 + `AsrSession` 独立会话对象 |
| Pipeline 阻塞在 `Stop()` → `join()` | Pipeline 只调 `End()`/`Cancel()`，永不阻塞 |
| 线程生命周期混合管理（Pipeline 创建、引擎 join） | 每个 `AsrSession` 自管理线程，超时兜底 |
| 队列无界 | 队列有界（待实现） |

---

## 核心接口

### AsrSession — 一句话的抽象

```cpp
/// 一次语音识别会话，对应 VAD 检测到的一句话。
/// FeedAudio / End / Cancel 必须线程安全，可从 Pipeline 分发线程调用。
class AsrSession {
public:
    virtual ~AsrSession() = default;

    /// 送入音频数据（16kHz mono F32）。
    /// 非阻塞：内部缓冲或推入队列，异步处理。
    virtual void FeedAudio(const float* pcm, size_t frames) = 0;

    /// 标记语音结束。永不阻塞。
    /// 引擎内部启动最终识别流程，完成后通过 resultCb_ 回调。
    virtual void End() = 0;

    /// 强制取消当前会话。永不阻塞。
    /// 用于：新话语开始时旧会话未结束 / deactivate / backend 切换。
    /// 引擎需保证有限时间内（CURLOPT_TIMEOUT）线程自行退出。
    virtual void Cancel() = 0;
};
```

### AsrEngine — 会话工厂

```cpp
/// ASR 后端引擎，全局单例（由 Pipeline 持有）。
/// 生命周期：Init → (StartSession / CancelSession)* → 析构。
class AsrEngine {
public:
    struct Config {
        std::string modelName;
        std::string apiEndpoint;
        std::string apiKey;
        // ... 各后端私有配置 ...
    };

    /// 初始化引擎，加载配置。可多次调用以热更新。
    /// 调用时若有活跃会话，应先 CancelSession。
    virtual bool Init(const Config& config) = 0;

    /// 创建新的识别会话。返回的 Session 处于"待 Feed 音频"状态。
    /// 若有未结束的旧会话，内部自动 Cancel（不阻塞调用方）。
    virtual std::unique_ptr<AsrSession> StartSession() = 0;

    /// 取消所有活跃会话。用于 deactivate / backend 切换。
    virtual void CancelSession() = 0;

    virtual const char* Name() const = 0;

    void SetResultCallback(ResultCallback cb);
    void SetErrorCallback(ErrorCallback cb);

protected:
    ResultCallback resultCb_;
    ErrorCallback errorCb_;
};
```

### Pipeline — 纯事件分发器

Pipeline 不再创建/管理 ASR 线程，只做事件路由：

```
while (running_) {
    SpeechEvent ev = speechEventQueue_.Pop();  // 阻塞等待
    switch (ev.type) {
    case Begin:
        // 旧会话自动取消（后端内部处理）
        activeSession_ = asrEngine_->StartSession();
        break;

    case Audio:
        if (activeSession_)
            activeSession_->FeedAudio(ev.pcm.data(), ev.pcm.size());
        break;

    case End:
        if (activeSession_) {
            activeSession_->End();       // 永不阻塞
            activeSession_.reset();      // 释放句柄，旧线程后台自行结束
        }
        break;

    case Cancel:
        if (activeSession_) {
            activeSession_->Cancel();    // 永不阻塞
            activeSession_.reset();
        }
        break;
    }
}
```

---

## 线程模型

```
                  ┌──────────────────────┐
                  │    Pipeline 线程      │  ← 只做事件分发，从不 I/O
                  │  (ASR Event Loop)    │
                  └──────┬───────┬───────┘
                         │       │
                  Begin/End     Audio
                         │       │
                         ▼       ▼
                  ┌──────────────────┐
                  │  AsrSession 实现  │  ← 引擎自有线程
                  │  (HTTP/WS/本地)   │
                  └────────┬─────────┘
                           │
                     回调 resultCb_
                           │
                           ▼
                  ┌──────────────────┐
                  │   ResultQueue    │  → PollResults → commitString
                  └──────────────────┘
```

### 线程保障

| 线程 | 功能 | 阻塞点 |
|------|------|--------|
| 主线程 | Fcitx5 事件循环、PollResults | 无 I/O 阻塞 |
| 捕获线程 | 读取音频硬件 | PulseAudio `pa_simple_read` / PipeWire 回调 |
| VAD 线程 | Silero ONNX 推理 | ONNX Runtime `Run()`（<1ms/帧） |
| ASR 分发线程 | 消费 SpeechEvent，调用 Session 方法 | `speechEventQueue_.Pop()` 等待 |
| Session 线程 | HTTP/WS I/O、本地推理 | `curl_easy_perform` / `ws_recv` |

### 超时保障（必须实现）

每个 `AsrSession` 的自有线程必须设置硬超时，确保 `Cancel()` 后有限退出：

| 后端 | 超时参数 |
|------|----------|
| OpenAI | `CURLOPT_TIMEOUT=10s`, `CURLOPT_CONNECTTIMEOUT=5s` |
| Volcengine | `CURLOPT_TIMEOUT=15s`, `CURLOPT_CONNECTTIMEOUT=5s`, recv 内循环 1s 超时 |
| 本地 ASR | 每帧推理 <10ms，无需额外超时 |

---

## 队列架构

```
Capture → [FrameQueue] → VAD → [SpeechEventQueue] → Pipeline → [ResultQueue] → commit
          Bounded(512)          Bounded(64)                     Bounded(128)
```

所有队列有界，超载时丢弃最旧数据：

| 队列 | 容量 | 满时策略 | 理由 |
|------|------|----------|------|
| `FrameQueue` | 512 帧 (16s) | 丢弃最旧 | 音频实时，旧帧无意义 |
| `SpeechEventQueue` | 64 事件 | 丢弃最旧 | VAD 是实时的，积压意味着用户早已停止说话 |
| `ResultQueue` | 128 结果 | 丢弃最旧 | 只有最新结果有价值 |

---

## 如何扩展一个新后端

加一个新 ASR 后端 = 两个类：

### 步骤

1. **创建 `AsrSession` 子类** — 实现 `FeedAudio/End/Cancel`
2. **创建 `AsrEngine` 子类** — 实现 `Init/StartSession/CancelSession`

### 示例：Azure Speech WS

```cpp
// azure_asr_session.h
class AzureAsrSession : public AsrSession {
    void FeedAudio(const float* pcm, size_t frames) override;
    void End() override;    // 发结束帧，等最终结果（不阻塞调用方）
    void Cancel() override; // 设 cancelled_ + close WS（5s 超时兜底）
private:
    void WorkerLoop();
    CURL* curl_ = nullptr;
    std::thread workerThread_;
    std::atomic<bool> cancelled_{false};
    ThreadSafeQueue<std::vector<int16_t>> audioQueue_;
};

// azure_asr_engine.h
class AzureAsrEngine : public AsrEngine {
    bool Init(const Config& config) override;
    std::unique_ptr<AsrSession> StartSession() override {
        CancelSession();  // 取消旧会话
        return std::make_unique<AzureAsrSession>(config_, resultCb_);
    }
    void CancelSession() override;
};
```

### 注册

引擎实例化在 `engine.cpp` 的 `CreateAsrEngine()` 中根据 `activeBackend` 选择：

```cpp
if (backend == "azure") {
    asr = std::make_unique<AzureAsrEngine>();
} else if (backend == "openai") {
    asr = std::make_unique<OpenaiCompatAsrEngine>();
}
```

后端列表在 `AsrBackendAnnotation` 中注册 Enum + SubConfigPath。

---

## 错误处理

| 场景 | 行为 |
|------|------|
| Session `Cancel()` 后线程未退出（超时） | 日志 Error，析构时 `detach()` 释放资源（已知风险：curl 没有线程安全的中断） |
| WS 连接失败 | `errorCb_` 通知 Pipeline，丢弃当前段，VAD 继续监听 |
| HTTP 请求超时 | 同上 |
| Init 失败（API Key 为空） | `CreateAsrEngine` 返回 nullptr，Pipeline 不启动 |
| 队列满 | 丢弃最旧数据，日志 Warning |
| VAD 模型加载失败 | Pipeline 不启动，日志 Error |

---

## 目录结构（新增）

```
docs/
├── architecture/               # ← 本文档位置
│   └── v4-asr-session-model.md
└── reviews/
    └── 2026-07-05-architecture-review.md
```

---

## 从 v3 迁移路径

### Phase 1: 接口对齐（目标：Pipeline 永不阻塞）

1. 新增 `AsrSession` 基类
2. 改造 `VolcengineStreamingAsrEngine`：
   - `StartSession()` 返回新的 `VolcengineAsrSession` 实例
   - `CancelSession()` 遍历取消所有活跃会话
3. 改造 `OpenaiCompatAsrEngine`：
   - `StartSession()` 返回新的 `OpenaiAsrSession` 实例
   - Session End 时启动 HTTP worker
4. `Pipeline::AsrWorkerLoop` 改为纯分发模式

### Phase 2: 资源保障

5. 所有队列加容量上限
6. Volcengine WS 加 5s recv 超时自检 `cancelled_`
7. OpenAI HTTP 加 `CURLOPT_TIMEOUT`

### Phase 3: 可观测性

8. 每个 Session 加唯一 ID，贯穿日志
9. Pipeline 健康状态 / 队列积压日志

---

*—— 架构设计 v4，反映 feature/volcengine-streaming-asr 分支的架构方向。*
