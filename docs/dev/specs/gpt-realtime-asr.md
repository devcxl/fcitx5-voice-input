# 技术方案：OpenAI GPT-Realtime 流式实时转录

> 面向 fcitx5-voice-input（C++ Fcitx5 语音输入插件）。为 OpenAI 后端增加 GPT-Realtime 流式实时转录（边说边出增量 preedit），作为现有 whisper-1 HTTP 一次性转录之外的实时方案。
> 日期：2026-08-07。作者：@architect。
> 前置调研：`docs/dev/research/gpt-realtime-whisper.md`。

---

## 1. 目标与范围

### 1.1 目标
在 OpenAI 后端下，新增一种「实时转录」API 模式：通过 WebSocket 持久会话把采集到的 PCM 音频持续推送到 OpenAI Realtime 转录端点，**边说边出**增量文本（通过 `isPartial` 实时刷入 preedit），语音段结束后提交最终结果上屏。

### 1.2 范围
- **包含**：新 AsrEngine/AsrSession 实现、16k→24k 重采样、WS 客户端线程、commit 时机、断线重连、配置项、工厂接入。
- **不包含**（YAGNI）：翻译会话、WebRTC、多语种检测（`gpt-transcribe`）、本地 ASR 改造、LLM 后处理在实时路径的特殊优化（沿用现有通道）。

### 1.3 非目标（明确排除）
- 不改变现有 whisper-1 / chat 模式与 Volcengine 引擎的行为。
- 不引入任何新依赖库（复用 libcurl 7.86+ 内置 WebSocket）。
- 不做 HTTP/SSE 流式替代（Realtime 增量必须持久 WS 连接，调研 C4）。

---

## 2. 架构决策

### 2.1 新引擎类 vs 改造现有 openai_asr —— **新增独立引擎类**

推荐：**新增 `RealtimeAsrEngine` / `RealtimeAsrSession`（新文件 `realtime_asr.cpp/.h`）**，而非在 `openai_asr.cpp` 里继续塞分支。

理由（SRP）：
- 现有 `OpenaiAsrSession` 是「一次性上传、End 时才起 worker 线程」的批处理模型；Realtime 是「StartWorker 即建持久 WS 会话、持续收发、手动 commit」的流式模型。两者 worker 生命周期、commit 语义、断线重连逻辑完全不同。
- 若并入 openai_asr.cpp，该文件会有「whisper 批量」「chat」与「realtime 流式」三个互不相关的修改理由，违反 SRP。
- 与项目既有模式一致：`volcengine_asr.cpp/.h` 已是独立引擎类，同样走 WS 流式。Realtime 与其架构高度相似，独立成类可完全复用其模式。

接入方式（保持配置段复用）：`CreateAsrEngine()` 的 OpenAI 分支中，按 `apiMode` 分流——`apiMode == "realtime"` 时创建 `RealtimeAsrEngine`，否则创建现有 `OpenaiAsrEngine`。**复用 `OpenAIAsrConfig` 配置段**（baseUrl/apiKey/model/language/apiMode），不改配置结构、不动其他引擎。

```cpp
// engine.cpp CreateAsrEngine() openai 分支
auto apiMode = *openaiConfig_.apiMode;
asrConfig.apiMode = apiMode;
if (apiMode == "realtime") {
    asr = std::make_unique<RealtimeAsrEngine>();
} else {
    asr = std::make_unique<OpenaiAsrEngine>();
}
```

### 2.2 重采样 16k→24k —— 线性上采样，放引擎内部

**位置**：`RealtimeAsrSession` worker 线程内，发送 base64 之前（16k float → 24k int16 → base64）。

理由：
- Realtime API 硬性要求 pcm16/mono/**24kHz**，16k 会被拒（调研 C6）。本项目采集统一 16kHz。
- 只在 realtime 路径需要，whisper/chat/volcengine 均保持 16k 不动。放引擎内部隔离影响，**不污染共享的 pipeline 音频路径**与 VAD（VAD 仍用 16k）。
- 上采样比例固定 1.5×（24k/16k=3/2），**线性插值**足够（每 2 个源样本线性插出 3 个目标样本），CPU 开销可忽略，符合 KISS。无需引入重采样库。

实现要点：
- `FeedAudio` 收到 float（16k），与现状一样经队列送 worker；worker 内做 `float(16k) → float(24k) 线性插值 → int16 → Base64Encode`。
- 复用 `openai_asr.cpp` 已存在的 `Base64Encode`（提升到公共位置或独立小工具，见 4.4）。

### 2.3 WS 客户端线程模型 —— 完全复用 volcengine 模式

`RealtimeAsrSession` 生命周期与 `VolcengineAsrSession` 完全对齐：

| 阶段 | 行为 |
|------|------|
| `StartSession()` | 分配 sessionId，创建 session，`StartWorker()` 立即启动 worker 线程（**与 whisper 不同：建会话即连 WS**） |
| `StartWorker()` | worker 线程：`curl_easy_init` + `CURLOPT_CONNECT_ONLY=2L` + `curl_easy_perform` 完成 WS Upgrade → `curl_ws_send/recv` 持续收发 |
| `FeedAudio()` | 转 int16 推入 `audioChunks_` 队列 |
| `End()` | 置 finished，发 `input_audio_buffer.commit`，收 `.completed`，关连接退出 |
| `Cancel()` | 置 cancelled，关连接退出 |
| 会话结束 | pipeline 把 `shared_ptr` 交给 `SessionReaper` 后台清理（与 volcengine 一致） |

**与 pipeline/SessionReaper 衔接**：
- pipeline 只感知一个 `AsrSession`，`activeSessionId_`/`sessionGenerationMap_` 逻辑零改动。
- **重连不改变 session 对象与 sessionId**：断线/30min 时 worker 在**自身循环内**重建底层 WS 连接，`AsrSession::State::sessionId` 保持不变 → `resultCb_(text, isFinal, sid)` 的 sid 稳定，pipeline 无需感知重连。
- 结果回调走现有 `resultCb_`，`SessionReaper` 负责 join/清理（防 worker 泄漏）。

线程安全：`audioChunks_` 用现有 `ThreadSafeQueue<std::vector<int16_t>>`；`state_` 用 `AsrSession::State` 的 atomic `cancelled/finished`。与 volcengine 完全一致，无新增同步原语。

### 2.4 实时中间结果经 isPartial 到 preedit —— 现有链路零改动

pipeline 的 `SetAsrEngine` 回调已内置完整分发逻辑：

```
RealtimeAsrSession.resultCb_(delta, false, sid)        // isFinal=false
  → Pipeline SetAsrEngine lambda → AsrResult{isPartial=true}
  → resultQueue_ → engine::PollResults()
  → inputPanel().setPreedit(Text(result.text))         // 实时刷入 preedit
```

- delta 在引擎内**按 item/会话累加**为当前累计文本，非空时回调 `isFinal=false`。
- final：`.completed` 回调 `isFinal=true` → 走现有 final 分支（`autoCommit` → commitString，或 LLM 后处理 / 预编辑待确认）。
- **pipeline / engine 的分发代码无需修改**（`isPartial` 字段与 preedit 分支已存在并验证）。

### 2.5 commit 时机策略 —— VAD 静音触发 + 周期性兜底

**主提交时机（VAD 静音）**：Silero VAD 检测到语音结束（`SpeechEventType::End`）→ pipeline 调 `activeSession_->End()` → 引擎发 `input_audio_buffer.commit` → 收 `.completed`（final）。这是「语音输入法」的语义主点：一次说话结束即提交上屏。

**周期性兜底（长句无停顿）**：
- 对 `gpt-live-transcribe`（支持真连续 delta），worker 循环中**无需 commit 也能持续收到 delta**，实时增量天然满足。
- 对 `gpt-realtime-whisper`（无 server_vad、需手动 commit 才出结果），长句不中断则拿不到增量。因此 worker 内置**周期性 commit 兜底**（如每 5s 发一次 `input_audio_buffer.commit`），保证长句也能持续出结果。

**pipeline 单-final 契约（实现约束）**：pipeline 的 `SetAsrEngine` 回调在收到任意 `isFinal=true` 后会 `sessionGenerationMap_.erase(sid)`，其后该会话所有结果被丢弃。因此**每个会话只能上报一次 final**：
- **只有 VAD End 触发的 `.completed` 上报 final**（`isFinal=true`）。
- **周期性 commit 产生的 `.completed` 一律作为增量 partial 上报**（`isFinal=false`，仅刷新 preedit，不上屏）。这是审查（2026-08-07）修正的关键点，避免首个周期 commit 后后续结果全被丢弃。

**实现**：worker 发送循环里记录「距上次 commit 的时长」，超过 `commitIntervalMs`（配置，默认 5000ms）且当前有累积音频时自动 commit；`.completed` 事件到达时按 `state_->finished` 判断：已 End → final，否则 → partial。对 live-transcribe 可将兜底视为纯增量刷新。

### 2.6 配置项设计 —— 复用 apiMode，新增枚举值

| 配置键（现有 `OpenAIAsrConfig`） | 变更 |
|------|------|
| `apiMode` | 新增枚举值 `"realtime"`（`ApiModeAnnotation` 增加 Enum/2），触发 Realtime 引擎 |
| `model` | 复用；填 `gpt-live-transcribe`（推荐）或 `gpt-realtime-whisper` |
| `baseUrl` | 复用；WS 端点由 baseUrl 派生：`https://`→`wss://` 后拼 `/v1/realtime?model=<model>` |
| `apiKey` | 复用（`Authorization: Bearer`） |
| `language` | 复用（`session.update` 的 `transcription.language` hint） |
| 新增 | `commitIntervalMs`（周期性兜底 commit 间隔，默认 5000，IntConstrain 1000~30000） |

**WS 端点派生**：`baseUrl` 是 `https://api.openai.com/v1` → 替换 scheme 为 `wss://` 得到 `wss://api.openai.com/v1`，拼 `/realtime?model=<model>`。若用户填的 baseUrl 已是 `wss://`（自建兼容端点）则直接用。**不引入新配置键存放端点**（KISS）。

### 2.7 断线重连、30min 会话、错误处理

- **断线重连**：worker 主循环内，若 `curl_ws_recv` 返回连接关闭/错误，且 `state_->cancelled` 为 false 且未 End，则等待退避（如 1s）后**重建 WS 连接**（保持同一 session/sessionId），从当前音频缓冲区续推。
- **30min 会话上限**：worker 内计时，到点强制 commit 一次并重建连接（同一 sessionId）。官方约 ≤30min 上限（调研 §3.3/§5.3）。
- **错误处理**：
  - 连接失败/鉴权失败 → `errorCb_` 记日志；对活动说话段回调 `resultCb_("", true, sid)`（空 final 触发 pipeline 的 error 分支，清 preedit、提示失败）。
  - 转录失败事件（`.failed`）→ 同上。
  - 重连失败超过 `maxReconnectAttempts`（如 3 次）→ 放弃，按错误结束会话。

---

## 3. 模块与文件

### 3.1 新增文件
| 文件 | 职责 |
|------|------|
| `src/addon/asr/realtime_asr.h` | `RealtimeAsrEngine` + `RealtimeAsrSession` 声明 |
| `src/addon/asr/realtime_asr.cpp` | 实现：WS 连接、16k→24k 重采样、delta/completed 解析、commit/重连 |

### 3.2 修改文件
| 文件 | 改动 |
|------|------|
| `src/addon/asr/openai_asr.cpp` | 将 `Base64Encode` 提升为可在 realtime 复用的公共函数（或移至独立工具），自身逻辑不动 |
| `src/addon/config/voiceinput-config.h` | `ApiModeAnnotation` 增加 `"realtime"`；`OpenAIAsrConfig` 增加 `commitIntervalMs` |
| `src/addon/engine.cpp` | `CreateAsrEngine()` openai 分支按 `apiMode=="realtime"` 分流创建 `RealtimeAsrEngine` |
| `CMakeLists.txt` | `ADDON_SOURCES` 增 `realtime_asr.cpp`；`ADDON_HEADERS` 增 `realtime_asr.h` |

### 3.3 依赖影响 —— **零新依赖**
- `curl >= 7.86.0` 内置 WebSocket（`curl_ws_send/recv` + `CURLOPT_CONNECT_ONLY=2L`），CMake 第 52-54 行已强制要求。
- `jsoncpp`（事件序列化/解析）、`Base64Encode`（复用）均已有。
- 无新 pkg-config、无新链接库、无需打包改动（AUR/CPack 依赖不变）。

---

## 4. 接口设计

### 4.1 复用既有接口
- `AsrEngine` / `AsrSession`（`asr_engine.h` / `asr_session.h`）：`RealtimeAsrSession` 实现 `FeedAudio/End/Cancel/JoinWithTimeout/StartWorker`，回调 `resultCb_(text, isFinal, sessionId)`。**对外接口零新增**。
- `AsrResult.isPartial`：已存在，用于增量 preedit。

### 4.2 RealtimeAsrSession 内部接口（私有）
```cpp
class RealtimeAsrSession : public AsrSession {
public:
    // 构造/析构/Override 同 VolcengineAsrSession
private:
    void WorkerLoop();                    // 主线程：WS 收发 + commit/重连
    bool ConnectWebSocket(CURL** curl);   // CONNECT_ONLY + upgrade，返回是否成功
    bool SendAppend(const std::vector<int16_t>& pcm24k); // input_audio_buffer.append
    bool SendCommit();                    // input_audio_buffer.commit
    void HandleServerEvent(const Json::Value& ev); // delta/completed/failed
    void Upsample16kTo24k(const std::vector<int16_t>& in,
                          std::vector<int16_t>& out); // 线性插值
    std::string currentTranscript_;       // 当前 item 累计文本
    std::string itemId_;                  // 当前 commit item
    int commitIntervalMs_;
    // ...
};
```

### 4.3 WS 事件协议（来自调研 §2）
- 客户端 → 服务端：`input_audio_buffer.append`（`{type, audio:<base64 pcm24k>}`）、`input_audio_buffer.commit`、`session.update`（`{type:"session.update", session:{transcription:{model,language}}}`，可选）。
- 服务端 → 客户端：`conversation.item.input_audio_transcription.delta`（`item_id,delta`，isFinal=false）、`.completed`（`item_id,transcript`，isFinal=true）、`.failed`。
- 认证：`Authorization: Bearer <key>`；GA 接口**不带** `OpenAI-Beta` 头。

### 4.4 Base64 复用
`openai_asr.cpp` 的 `Base64Encode` 当前在匿名 namespace。为供 realtime 复用，将其**提升**为 `fcitx` 命名空间内的公共自由函数（放在 `openai_asr.h` 或独立 `utils/base64.h`）。两处调用，**达到 DRY 阈值 2 次**，允许最小抽象。

---

## 5. 模型选型（推荐）

**推荐：`gpt-live-transcribe`**（方案 C）。

| 维度 | gpt-live-transcribe | gpt-realtime-whisper |
|------|---------------------|----------------------|
| 增量体验 | 真连续 delta，可调 `delay`（调研 §2.3） | 需手动 commit，delta 可能批量到达 |
| 官方定位 | 当前新接入推荐模型 | 现有集成可继续使用，非新接入推荐 |
| 计费 | $0.017/音频分钟 | $0.017/音频分钟（相同） |
| 可用性 | GA | GA |

理由：
- 本插件核心诉求是「边说边显示增量」——`gpt-live-transcribe` 正是为此设计，能最大化实时体验。
- 二者同价、同 GA，选体验更佳者无额外成本。
- `gpt-realtime-whisper` 作为配置项**可选**（兼容已用该模型的用户），默认/文档推荐 `gpt-live-transcribe`。

---

## 6. DAG 任务拆解

见 `docs/dev/tasks/gpt-realtime-asr.md`。

---

## 7. 假设与不确定项

| # | 内容 | 影响 |
|---|------|------|
| A1 | `gpt-live-transcribe` 的连续 delta 在 C++ 实测中确实逐词实时（调研 C5 置信度中-高） | 若实测仍批量到达，周期 commit 兜底保证可用性；不阻塞交付 |
| A2 | WS 端点由 baseUrl `https→wss` 派生正确 | 若用户用自建端点，需自行填对 baseUrl；提供日志便于排查 |
| A3 | 重连保持 sessionId 不会触发 OpenAI 侧状态错乱 | 重连即全新 WS 会话，服务端无跨连接状态依赖；若出问题，周期 commit + 空 final 兜底 |
| A4 | 免费 Tier 不可用（需付费） | 配置文档注明；连接失败走 error 分支提示 |
| A5 | `commitIntervalMs` 默认 5000ms 是否最优 | 属调参项，默认值保守可用；通过配置暴露 |

---

## 8. 参考
- 调研文档：`docs/dev/research/gpt-realtime-whisper.md`（端点、协议、模型、计费一手来源）。
- 既有实现参考：`src/addon/asr/volcengine_asr.cpp`（WS worker 线程模型）、`src/addon/asr/openai_asr.cpp`（Base64Encode）。
