# DAG 任务拆解：OpenAI GPT-Realtime 流式实时转录

> 依据技术方案 `docs/06-development/specs/gpt-realtime-asr.md` 与 ADR `docs/03-architecture/adr/ADR-0001-gpt-realtime-asr.md`。
> 每个任务为垂直切片、单人 2-4 小时可完成；标注依赖关系。

## 依赖图总览

```
T1 (Base64 提升复用)
    │
    ├─→ T2 (realtime_asr 骨架 + 重采样) ──→ T3 (WS 传输 + 事件解析)
    │                                          │
    ├─→ T4 (commit 时机 + 重连) ───────────────┘
    │
    ├─→ T5 (配置项: apiMode + commitIntervalMs)
    │       │
    │       ├─→ T6 (engine.cpp 工厂分流)
    │       │
    │       └─→ T7 (CMake 接入)
    │
    └─→ T8 (构建验证 + 文档收尾)
```

**串行关键路径**：T1 → T2 → T3 → T4 → T6 → T8
**可并行**：T5 可与 T2/T3 并行；T7 可与 T6 并行。

---

## T1: 提升 Base64Encode 为公共函数

- **目标**：使 `RealtimeAsrSession` 能复用现有 `Base64Encode`（DRY）。
- **改动文件**：
  - `src/addon/asr/openai_asr.cpp`：将匿名 namespace 的 `Base64Encode` 提升为 `fcitx` 命名空间内公共函数。
  - 新增 `src/addon/asr/utils/base64.h`（声明）+ `.cpp`（实现）或放在 `openai_asr.h` 声明。
- **依赖**：无。
- **验证**：`OpenaiAsrSession` 的 whisper 路径编译通过、行为不变（回归）；新函数可被单独调用。

---

## T2: RealtimeAsrSession 骨架 + 16k→24k 重采样

- **目标**：新建 `RealtimeAsrEngine`/`RealtimeAsrSession` 类骨架，实现 `AsrSession` 全部虚函数；worker 线程 + `FeedAudio` 队列 + 16k→24k 线性插值。
- **改动文件**：
  - 新增 `src/addon/asr/realtime_asr.h`
  - 新增 `src/addon/asr/realtime_asr.cpp`
- **依赖**：T1（复用 Base64Encode）。
- **实现要点**：
  - 类结构仿照 `VolcengineAsrSession`：`audioChunks_`（`ThreadSafeQueue<std::vector<int16_t>>`）、`workerThread_`、atomic `state_`。
  - `FeedAudio` 转 int16 入队；`StartWorker` 启动线程；`End/Cancel/JoinWithTimeout` 对齐 volcengine。
  - `Upsample16kTo24k()`：线性插值，比例 3/2。
- **验证**：构造 session、FeedAudio、End 后 worker 能消费队列并打印重采样样本数（先用日志桩验证，WS 在 T3 接入）。

---

## T3: WS 传输 + 事件解析

- **目标**：worker 内建 WS 连接（`CURLOPT_CONNECT_ONLY=2L` + `curl_ws_send/recv`），推送 `input_audio_buffer.append`，解析 `delta/completed/failed` 并经 `resultCb_` 回调。
- **改动文件**：`src/addon/asr/realtime_asr.cpp`
- **依赖**：T2。
- **实现要点**：
  - 端点：`baseUrl` https→wss 派生 + `/v1/realtime?model=<model>`；`Authorization: Bearer` 头；GA 不带 OpenAI-Beta 头。
  - `session.update` 可选项：设 `transcription.language`。
  - `delta` → 累加到 `currentTranscript_`，`resultCb_(text, false, sid)`；`completed` → `resultCb_(transcript, true, sid)`。
  - `SendAppend`：16k→24k→int16→Base64 封 JSON 发送。
- **验证**：连接真实（若有 key 可端到端）；无 key 时验证 JSON 构造与发送路径正确、错误分支走 `errorCb_`。

---

## T4: commit 时机 + 断线重连 + 30min 会话

- **目标**：实现「VAD End → commit」主提交 + 周期性 commit 兜底；断线/30min 重连（保持 sessionId）。
- **改动文件**：`src/addon/asr/realtime_asr.cpp`
- **依赖**：T3。
- **实现要点**：
  - `End()` → `SendCommit()` → 收 `.completed` final。
  - worker 记录距上次 commit 时长，超 `commitIntervalMs` 自动 commit（长句兜底）。
  - `curl_ws_recv` 断开/错误且未取消 → 退避（1s）重建 WS 连接，同 sessionId；超 `maxReconnectAttempts`（3）放弃走 error。
  - 30min 计时强制 commit + 重连。
- **验证**：模拟连接断开（断网/关服务）→ 观察重连日志与续推；长句测试周期性 commit 出增量。

---

## T5: 配置项（apiMode="realtime" + commitIntervalMs）

- **目标**：`ApiModeAnnotation` 增加 `"realtime"` 枚举；`OpenAIAsrConfig` 增加 `commitIntervalMs`。
- **改动文件**：`src/addon/config/voiceinput-config.h`
- **依赖**：无（可与 T2 并行）。
- **验证**：配置 dump 生成正确；值经 `Config::apiMode`/`commitIntervalMs` 读取无误。

---

## T6: engine.cpp 工厂分流

- **目标**：`CreateAsrEngine()` openai 分支按 `apiMode=="realtime"` 创建 `RealtimeAsrEngine`。
- **改动文件**：`src/addon/engine.cpp`
- **依赖**：T5（配置项）、T2（引擎类存在）。
- **验证**：配置 `apiMode=realtime` 时 `asr->Name()=="realtime"`；`apiMode=whisper` 时仍为 `openai-compat`（回归）。

---

## T7: CMake 接入

- **目标**：将 `realtime_asr.cpp`/`.h` 加入 `ADDON_SOURCES`/`ADDON_HEADERS`。
- **改动文件**：`CMakeLists.txt`
- **依赖**：T2（文件存在）。
- **验证**：`cmake --build` 通过，链接无缺符号。

---

## T8: 构建验证 + 文档收尾

- **目标**：全量编译 + 回归现有后端 + 更新 README/配置文档说明实时模式。
- **改动文件**：`README.md`（或配置说明文档）、`po/zh_CN.po`（新增字符串 `commitIntervalMs` 等，如需）
- **依赖**：T6、T7。
- **验证**：`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` 通过；配置 UI 显示新枚举与选项；三种 apiMode 均编译通过。

---

## 任务属性汇总

| 任务 | 文件数 | 新增文件 | 修改文件 | 依赖 | 预估 |
|------|--------|----------|----------|------|------|
| T1 | 2 | utils/base64 或复用头 | openai_asr.cpp | — | 0.5h |
| T2 | 2 | realtime_asr.h/.cpp | — | T1 | 2h |
| T3 | 1 | — | realtime_asr.cpp | T2 | 3h |
| T4 | 1 | — | realtime_asr.cpp | T3 | 2h |
| T5 | 1 | — | voiceinput-config.h | — | 0.5h |
| T6 | 1 | — | engine.cpp | T5, T2 | 0.5h |
| T7 | 1 | — | CMakeLists.txt | T2 | 0.2h |
| T8 | 2-3 | — | README/po | T6, T7 | 1h |

> 说明：T3/T4 集中在 `realtime_asr.cpp`，若单人实现可合并为一个提交，但拆为 T3/T4 便于独立验证 WS 传输与重连语义。总改动 ≤10 文件，可单批次交付。
