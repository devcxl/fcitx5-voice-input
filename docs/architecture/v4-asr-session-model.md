# fcitx5-voice-input 架构设计 v4.1

> **状态:** 提案文档（经审查修订版）。
> **审查:** docs/reviews/2026-07-05-architecture-review.md
> **上一版:** [ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) (v3)

---

## 设计原则

| 原则 | 含义 |
|------|------|
| **Pipeline 从不阻塞** | 所有 I/O 跑在引擎自有线程上，Pipeline 只做事件分发 |
| **会话即对象** | 每句语音是一个 `AsrSession`，有明确生命周期，不跟引擎绑死 |
| **接口表达语义，不表达协议** | `AsrSession` 只有 `FeedAudio`/`End`/`Cancel`，不知道 WS、HTTP、本地解码 |
| **有限资源边界** | 线程有上限 + 超时兜底，不出现僵尸线程 |
| **所有权清晰** | Pipeline 用 `shared_ptr` 持有 Session，Engine 用 `weak_ptr` 追踪，绝不悬空 |

---

## 新老架构对比

### 线程流对比

```
v3（当前）:
┌────────────────────────────────────────────────────────────────────┐
│ ASR Worker Loop (单线程)                                            │
│  while running:                                                    │
│    event = speechEventQueue_.Pop()                                 │
│    switch event.type:                                              │
│      Begin:                                                        │
│        pendingAsrAudio_.clear()                                    │
│        asrEngine_->Start()  ──→ 创建 WS 子线程                     │
│      Audio:                                                        │
│        pendingAsrAudio_ += event.pcm                               │
│        if pendingAsrAudio_ >= 200ms:                               │
│          asrEngine_->FeedAudio(data)  ──→ 入队 audioChunks_        │
│      End:                                                          │
│        flush remaining audio                                       │
│        asrEngine_->Stop()  ──→ set finished_=true                  │
│                              ──→ push empty sentinel               │
│                              ──→ workerThread_->join()  ←─ 阻塞    │
│                                            │                      │
│                                            │ (最多等 30s)          │
│                                            ▼                      │
│                                ←─ join 返回，才能处理下一句       │
└────────────────────────────────────────────────────────────────────┘

v4.1（提案）:
┌────────────────────────────────────────────────────────────────────┐
│ ASR Event Dispatcher (单线程，永不阻塞)                             │
│  while running:                                                    │
│    event = speechEventQueue_.Pop()                                 │
│    switch event.type:                                              │
│      Begin:                                                        │
│        if activeSession_:                                          │
│          activeSession_->Cancel()                                  │
│          drainingSessions_.push(std::move(activeSession_))         │
│        activeSession_ = asrEngine_->StartSession()                 │
│        // 立即返回，新 WS 握手在后台                                │
│      Audio:                                                        │
│        if activeSession_:                                          │
│          activeSession_->FeedAudio(data)                           │
│      End:                                                          │
│        if activeSession_:                                          │
│          activeSession_->End()    // 立即返回                       │
│          drainingSessions_.push(std::move(activeSession_))         │
│      Cancel:                                                       │
│        if activeSession_:                                          │
│          activeSession_->Cancel()  // 立即返回                      │
│          drainingSessions_.push(std::move(activeSession_))         │
│                                                                    │
│  // drainingSessions_ 由 SessionReaper 线程处理: join + 清理       │
└────────────────────────────────────────────────────────────────────┘
```

### 生命周期对比

```
v3:
Pipeline ─→ unique_ptr<AsrEngine>
                │
                └─ Start() → 创建线程
                   FeedAudio()
                   Stop() → join() → 线程结束     ← 同步等
                              │
                              └ 线程结束后 Pipeline 才能继续

v4.1:
Pipeline ─→ shared_ptr<AsrSession> activeSession_
                │
                ├─ StartSession() → shared_ptr → 新线程跑
                ├─ FeedAudio() → 入队
                ├─ End() → 设标志，立即回 ← 不等
                ├─ Cancel() → 设标志，立即回  ← 不等
                │
                └ SessionReaper → join + 析构     ← 后台等

Engine ─→ weak_ptr<AsrSession> 追踪活跃 Session
                │
                └─ CancelAllSessions(): 遍历 weak_ptr.lock() → Cancel()
```

---

## 核心接口

### AsrSession — 一句话的抽象

```cpp
/// 一次语音识别会话。FeedAudio / End / Cancel 必须线程安全。
/// 实现必须启用 enable_shared_from_this，worker 线程捕获 shared_ptr<State>，
/// 绝不捕获裸 this。
class AsrSession : public std::enable_shared_from_this<AsrSession> {
public:
    struct State {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> finished{false};
        uint64_t sessionId{0};
    };

    virtual ~AsrSession();

    virtual void FeedAudio(const float* pcm, size_t frames) = 0;

    /// 标记语音结束，启动最终识别流程。永不阻塞。
    /// 结果通过 resultCb_ 回调，回调参数携带 sessionId。
    virtual void End() = 0;

    /// 强制取消。永不阻塞。
    /// 设 cancelled 标志 + 主动 close socket + 设短超时轮询。
    /// 线程在有限时间内自行退出，由 Reaper 线程 join。
    virtual void Cancel() = 0;

    /// 获取共享状态（用于 Engine 追踪 + Reaper 判断）
    std::shared_ptr<State> GetState() const { return state_; }

    /// 用于 Reaper 线程 join
    virtual void JoinWithTimeout(std::chrono::milliseconds timeout) = 0;

protected:
    std::shared_ptr<State> state_ = std::make_shared<State>();
    ResultCallback resultCb_;
};
```

### AsrEngine — 会话工厂

```cpp
class AsrEngine {
public:
    virtual ~AsrEngine();

    virtual bool Init(const Config& config) = 0;

    /// 创建新会话。返回 shared_ptr，内部用 weak_ptr 追踪。
    /// 若当前有活跃会话未结束，内部自动 Cancel（不阻塞）。
    /// 超过 maxActiveSessions 限制时，取消最旧会话。
    virtual std::shared_ptr<AsrSession> StartSession() = 0;

    /// 取消所有由本 Engine 创建的活跃 Session。
    /// 用于 deactivate / backend 切换 / 配置热更新前。
    virtual void CancelAllSessions() = 0;

    virtual const char* Name() const = 0;

    void SetResultCallback(ResultCallback cb);
    void SetErrorCallback(ErrorCallback cb);

protected:
    ResultCallback resultCb_;
    ErrorCallback errorCb_;

    std::mutex sessionsMutex_;
    std::unordered_map<uint64_t, std::weak_ptr<AsrSession>> sessions_;
    uint64_t nextSessionId_{1};
    size_t maxActiveSessions_{3};
};
```

### SessionReaper — 会话回收器

```cpp
/// 独立线程，负责 join 已 End/Cancel 的 Session，释放资源。
/// 避免 Pipeline 线程被阻塞，也避免 main 线程析构时 detach。
///
/// Pipeline 将已结束的 Session 移入 drainingSessions_，
/// Reaper 在其后台线程中 JoinWithTimeout，完成后移除。
class SessionReaper {
public:
    SessionReaper();
    ~SessionReaper();

    void Add(std::shared_ptr<AsrSession> session);

    /// 回收所有剩余 Session（析构时调用）
    void DrainAll();

private:
    void ReaperLoop();
    std::thread thread_;
    ThreadSafeQueue<std::shared_ptr<AsrSession>> reapQueue_;
    std::atomic<bool> running_{false};
};
```

---

## 线程模型

```
主线程 (Fcitx5 事件循环)
  │  PollResults → commitString (带 sessionId 过滤)
  │
事件分发线程 (ASR Event Dispatcher)
  │  消费 SpeechEvent → 调用 Session 方法（永不 I/O）
  │
Session 线程 (每个 Session 一个)
  │  HTTP/WS I/O，完成后 resultCb_
  │
SessionReaper 线程
  │  后台 join 已结束的 Session，清理资源
  │
  └─ 所有线程都是有限生命周期 + 超时兜底
```

### 超时保障

| 后端 | 参数 | 确保 |
|------|------|------|
| OpenAI | `CURLOPT_TIMEOUT=10s`, `CONNECTTIMEOUT=5s` | 网络请求 10s 内结束 |
| Volcengine | `CURLOPT_TIMEOUT=15s`, `CONNECTTIMEOUT=5s`, recv 轮询 ≤500ms | 线程 15s 内退出 |
| Cancel | 主动 `curl_easy_cleanup` + close socket | 不再阻塞在 recv |

### 线程上限

| 类型 | 上限 | 超限策略 |
|------|------|----------|
| 活跃 Session | 3 | 取消最旧的 Session |
| draining Session | 8 | 阻塞 Pipeline（罕见，仅 Reaper 积压时） |

---

## 结果过滤

每个 `AsrResult` 携带 `sessionId`，Pipeline 在 commit 前按 session 过滤：

```cpp
struct AsrResult {
    uint64_t generation;      // 同 v3，过滤 activate/deactivate 过期
    uint64_t sessionId;       // 新增，过滤 cancelled session 的残留结果
    uint64_t utteranceId;
    std::string text;
    bool isLLMRefined;
    bool isPartial;
};
```

commit 逻辑：

```cpp
// 只有当前 activeSession_ 对应 sessionId 的结果才 commit
if (result.sessionId != activeSessionId_) {
    FCITX_DEBUG() << "Dropped stale result from session "
                  << result.sessionId;
    return;
}
```

---

## 队列架构

```
Capture → [FrameQueue] → VAD → [SpeechEventQueue] → Pipeline → [ResultQueue] → commit
          Bounded(512)          Bounded(64)                     Bounded(128)
```

### SpeechEventQueue 满时策略

普通 FIFO 丢弃对语音事件不安全。改为事件感知策略：

| 事件 | 队列满策略 |
|------|-----------|
| `Begin` | 保留。先插入隐式 `Cancel` 通知当前 Session |
| `Audio` | 丢弃最旧的 Audio 帧（可接受，VAD 是实时的） |
| `End` | **必须保留**。丢失 End 会导致 Session 永不结束 |
| `Cancel` | **必须保留，优先级最高**。丢失 Cancel 会导致旧 Session 残留 |

实现：`SpeechEventQueue` 内部维护两个优先级子队列，`End`/`Cancel` 走高优先级通道，`Audio` 可丢。

---

## Pipeline 伪代码（完整版）

```cpp
void Pipeline::AsrDispatcherLoop() {
    while (running_) {
        SpeechEvent ev = speechEventQueue_.Pop();
        switch (ev.type) {

        case Begin:
            // 取消当前会话
            if (activeSession_) {
                activeSession_->Cancel();
                reaper_->Add(std::move(activeSession_));
            }
            // 创建新会话
            activeSession_ = asrEngine_->StartSession();
            activeSessionId_ = activeSession_->GetState()->sessionId;
            break;

        case Audio:
            if (activeSession_) {
                activeSession_->FeedAudio(ev.pcm.data(), ev.pcm.size());
            }
            break;

        case End:
            if (activeSession_) {
                activeSession_->End();
                reaper_->Add(std::move(activeSession_));
                activeSessionId_ = 0;
            }
            break;

        case Cancel:
            if (activeSession_) {
                activeSession_->Cancel();
                reaper_->Add(std::move(activeSession_));
                activeSessionId_ = 0;
            }
            break;
        }
    }
}
```

---

## 状态迁移（配置热更新 & backend 切换）

```
用户修改配置
    │
    ▼
setConfig(rawConfig)
    │
    ├─ 1. pausePipeline()    ← 停止事件分发，不释放资源
    │       asrDispatcherRunning_ = false;
    │       speechEventQueue_.Clear();
    │
    ├─ 2. CancelAllSessions()
    │       asrEngine_->CancelAllSessions();
    │       activeSession_.reset();
    │       reaper_.DrainAll();    ← 等所有旧 Session 结束
    │
    ├─ 3. ReplaceAsrEngine()   ← 仅 backend 切换时
    │       asrEngine_ = CreateAsrEngine();
    │
    ├─ 4. asrEngine_->Init(newConfig)
    │
    └─ 5. resumePipeline()
            asrDispatcherRunning_ = true;
```

---

## 暂不实现的功能（v4.1 范围外）

| 功能 | 原因 | 未来版本 |
|------|------|---------|
| ConnectionPool（WS 连接复用） | WS 协议的 reset 语义不统一，引入状态污染风险 | v4.2 |
| 每句一个 WS 连接 | 简单可靠，稳定后评估复用 | v4.1 ✅ |
| 自动重试 | 先保证基础正确，再考虑可靠性 | v4.3 |
| 本地 ASR sherpa-onnx | 接口已预留，实现待后续 | v5 |

---

## 从 v3 迁移路径

### Phase 1: 接口定义 + 生命周期安全（P0）

1. 新增 `AsrSession` 基类（`shared_from_this` + `State` + `JoinWithTimeout`）
2. 新增 `SessionReaper` 类
3. `AsrEngine` 改为工厂模式，`StartSession()` 返回 `shared_ptr`
4. Engine 内部 `weak_ptr` 追踪活跃 Session
5. Pipeline 改用 `shared_ptr` + `drainingSessions_` + `SessionReaper`
6. 所有 `resultCb_` 结果带 `sessionId`

### Phase 2: 资源保障

7. Volcengine recv 加 500ms 轮询超时 + Cancel 时 close socket
8. 队列加容量上限 + 事件感知策略
9. `maxActiveSessions=3` 限制

### Phase 3: 配置热更新安全

10. `pause/resume` Pipeline 状态
11. `DrainAll` 等待旧 Session 结束
12. 配置更新顺序: pause → cancel → drain → replace → init → resume

---

## 附录：审查中发现的 v4 问题及修复

| # | 问题 | v4 原文 | v4.1 修复 |
|---|------|---------|-----------|
| 1 | UAF | `unique_ptr` + `reset()` 后线程还在访问 `this` | `shared_ptr` + `drainingSessions_` + Reaper |
| 2 | 析构 `detach` 危险 | `Cancel()` 超时后直接 `detach()` | `JoinWithTimeout` + Reaper 线程统一回收 |
| 3 | 所有权矛盾 | `unique_ptr` 给 Pipeline 但 Engine 又要取消 | `shared_ptr` + Engine 内部 `weak_ptr` 追踪 |
| 4 | 取消后结果误提交 | 无 sessionId 过滤 | 所有 `AsrResult` 带 `sessionId`，commit 前检查 |
| 5 | 队列丢弃不安全 | 统一"丢弃最旧" | 事件感知队列，`End`/`Cancel` 优先保留 |
| 6 | 连接池过早 | 引入 ConnectionPool | 暂不实现，一句话一个 WS 连接 |
| 7 | Cancel 只设标志不够 | 仅 `cancelled_ = true` | + 短超时轮询 + 主动 close socket + curl 超时 |
| 8 | 线程峰值 | 无限制 | `maxActiveSessions=3` |
| 9 | 热更新竞态 | `Init` 可多次调用 | pause → cancel → drain → replace → init → resume |

---

*—— 架构设计 v4.1，经审查修订。*
