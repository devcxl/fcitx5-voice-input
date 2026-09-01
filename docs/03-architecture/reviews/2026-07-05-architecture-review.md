# 架构审查报告

日期：2026-07-05  
审查范围：src/addon/ 全量代码  
Commit：935794c + 74ae2f8

---

## P0 严重

### 1. Volcengine Stop() 阻塞 ASR Worker 线程

**位置**：`src/addon/asr/volcengine_asr.cpp:420-428`  
**文件**：`src/addon/pipeline/pipeline.cpp:333-341`（调用方）

`VolcengineStreamingAsrEngine::Stop()` 调用 `workerThread_->join()`，而 Stop() 是在 Pipeline 的
AsrWorkerLoop 线程中同步调用的。这意味着一次 WS 请求从连接到结束的整个周期内，
ASR Worker 线程完全被阻塞——VAD 检测到的下一条语音事件无法被处理。

**影响**：网络抖动时流水线卡死，用户连续说话中间可能丢失片段。

**建议**：改为 detach 线程 + 异步回调通知结果，Stop() 仅发信号不 join。
Pipeline 侧也需要改为非阻塞的事件驱动模式。

---

## P1 高危

### 2. OpenAI ASR Start/Stop 不幂等

**位置**：`src/addon/asr/openai_asr.h:47-51`

`OpenaiCompatAsrEngine` 的 Start() 不等待旧线程结束。如果 VAD 在短时间内
连续触发两次（说话→静音→说话），旧线程可能仍在执行 HTTP 请求，
新线程与旧线程同时操作 `pcmBuffer_`（存在 data race）。

**建议**：Start() 中 join 旧线程，或对 `pcmBuffer_` 加互斥锁保护。

### 3. Pipeline 无背压机制

**位置**：`src/addon/pipeline/pipeline.cpp` 全局

四个核心队列（`frameQueue_` / `speechEventQueue_` / `audioChunks_` / `resultQueue_`）
均为无界 `ThreadSafeQueue`。如果任一环节慢于上游（如 WS 网络延迟），
内存将持续增长直至 OOM。

**建议**：改为有界队列（`BoundedThreadSafeQueue`），满时丢弃旧帧或阻塞生产者。
PipeWire 回调的 ≤100μs 约束下应优先丢旧帧。

### 4. 缺少会话生命周期守卫

**位置**：`src/addon/asr/asr_engine.h:55-64`

`AsrEngine` 接口文档定义了 `Start()/FeedAudio()/Stop()` 的 session 语义，
但没有任何实现进行状态校验：
- 连续调用两次 Start() 未定义
- FeedAudio() 在 Start() 之前调用未定义
- Stop() 在不活跃 session 上调用可能崩溃

当前正确性完全依赖调用方（Pipeline），容易出错。

**建议**：ASR 引擎内部加状态机（Idle → Active → Stopped），非法调用直接返回或 log。

---

## P2 中危

### 5. LLM 后处理阻塞 ASR 回调线程

**位置**：`src/addon/pipeline/pipeline.cpp:113-150`

LLM 的 `Process()`/`ProcessStream()` 直接在 ASR 引擎的结果回调中执行。
OpenAI 模式下这层嵌套在 HTTP worker 线程内，Volcengine 模式下嵌套在
WS 接收线程内——LLM 又发起一次 HTTP 请求，进一步加深阻塞。

**建议**：LLM 处理投递到独立线程或线程池，不阻塞 ASR 回调。

### 6. 配置路径硬编码魔法字符串

**位置**：`src/addon/engine.cpp:38-40,50,77,81`

`"conf/voiceinput.conf"`、`"conf/voiceinput-openai.conf"` 等路径以字面量
分散在多处。日后支持 XDG 规范或自定义配置目录需修改多处。

**建议**：提取为常量，或通过 `StandardPath` 类型查找。

### 7. VAD 迟滞系数硬编码

**位置**：`src/addon/pipeline/pipeline.cpp:30-31,57-58`

`slienceThreshold = speechThreshold × 0.7`，不可独立调节。某些场景
（如高噪音环境）需要更窄的迟滞带。

**建议**：作为独立配置项暴露在 `VoiceInputConfig` 中。

### 8. Silero VAD 模型路径不可配置

**位置**：`src/addon/vad/vad.cpp:62-64`

模型路径优先取 `VADWorker::Config::sileroModelPath`，空时回退到
`VOICE_INPUT_MODEL_DIR + "/silero_vad.onnx"`。但 `sileroModelPath` 未暴露到
配置 UI 中。如果用户自定义安装路径或想换模型，只能改代码。

**建议**：在 `VoiceInputConfig` 中增加 `SileroModelPath` 配置项。

---

## P3 低危

### 9. 无测试覆盖

**位置**：全局

`BUILD_TESTS=OFF`，项目无任何测试文件。核心逻辑（VAD 状态机、
ASR 引擎切换、PollResults 结果分发）均无自动化验证。

**建议**：优先为 VAD Worker 和 Pipeline 的队列通信添加单元测试。

### 10. 关闭 Capture 未清空 FrameQueue

**位置**：`src/addon/pipeline/pipeline.cpp:219-221`

`Stop()` 关闭 capture 但没有清空 `frameQueue_`。下次 Start() 时
旧数据会被 VAD 处理（虽然 VAD ResetSession 会重置状态，
但 Silero 模型本身有内部状态可能受影响）。

**建议**：Stop() 中清空 frameQueue_ 和 speechEventQueue_。
