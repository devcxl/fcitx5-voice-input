# ADR-2026-08-07：OpenAI GPT-Realtime 流式实时转录

> 状态：已接受
> 日期：2026-08-07
> 决策方：@architect
> 关联：Issue #10；技术方案 `docs/dev/specs/gpt-realtime-asr.md`

## 背景

用户通过 GitHub Issue #10 建议为 OpenAI 后端增加 GPT-Realtime-Whisper 流式实时转录。现状是 whisper-1 HTTP multipart 一次性转录（延迟秒级、无增量）。目标是「边说边出」增量 preedit。

## 决策

### D1：选 `gpt-live-transcribe` 作为默认推荐模型
- 官方当前新接入推荐模型即 `gpt-live-transcribe`；`gpt-realtime-whisper` 列为「现有集成可继续使用」。
- 二者同价（$0.017/音频分钟）、同 GA；`gpt-live-transcribe` 提供真连续 delta，`gpt-realtime-whisper` 需手动 commit 且 delta 可能批量到达。
- 本插件核心诉求是实时增量，选体验更佳者无额外成本。
- **备选**：`gpt-realtime-whisper` 保留为配置可选，兼容已用该模型的用户。

### D2：零新依赖（复用 libcurl 7.86+ 内置 WebSocket）
- Realtime 增量转录必须持久 WebSocket 连接。
- libcurl ≥ 7.86.0 原生支持 WS（`curl_ws_send/recv` + `CURLOPT_CONNECT_ONLY=2L`），CMake 已强制要求 curl ≥ 7.86（为 Volcengine WS 而设）。
- 不引入 websocketpp / uWebSockets 等新库；打包依赖（AUR/CPack）不变。
- 备选（引入第三方 WS 库）被否决：多一个 C++ 依赖、维护成本高、无必要。

### D3：新增独立 `RealtimeAsrEngine/RealtimeAsrSession` 类，而非改造 openai_asr
- 现有 `OpenaiAsrSession` 是「一次性上传、End 才起线程」的批处理模型；Realtime 是「StartWorker 即建持久 WS 会话、持续收发、手动 commit」的流式模型。
- 并入 openai_asr 会使该文件有 whisper/chat/realtime 三个互不相关的修改理由，违反 SRP。
- 与项目既有模式一致：`volcengine_asr` 已是独立 WS 流式引擎类，Realtime 复用其线程模型。

### D4：16k→24k 线性上采样（放引擎内部）
- Realtime API 硬性要求 pcm16/mono/24kHz，16k 会被拒。
- 只在 realtime 路径需要，放引擎内部隔离，不污染共享音频路径与 VAD（VAD 仍用 16k）。
- 比例固定 3/2，线性插值即可，不引入重采样库。

### D5：commit 时机 = VAD 静音（主） + 周期性兜底（次）
- 主提交点：Silero VAD 语音结束（`SpeechEventType::End`）→ `End()` → 发 `input_audio_buffer.commit` → 收 `.completed` final。
- 兜底：worker 周期性 commit（`commitIntervalMs`，默认 5s），解决长句无停顿拿不到增量的问题（对 `gpt-realtime-whisper` 尤其必要；对 `gpt-live-transcribe` 是纯增量刷新）。
- 架构上自然契合现有 VAD 状态机（调研 §5.3 亦指出此契合点）。

### D6：复用配置段与 apiMode 分流，不新增引擎配置结构
- 复用 `OpenAIAsrConfig`（baseUrl/apiKey/model/language/apiMode），仅 `ApiModeAnnotation` 增 `"realtime"` 枚举 + 新增 `commitIntervalMs`。
- 通过配置即插即用，不改配置结构、不动其他引擎。

## 权衡 / 被否决的替代方案
| 方案 | 否决理由 |
|------|---------|
| 改造现有 openai_asr 加 realtime 分支 | 违背 SRP；批处理与流式模型差异大 |
| 引入第三方 WS 库（websocketpp 等） | 零新依赖可行（curl 已内置 WS），多依赖徒增维护成本 |
| 默认 `gpt-realtime-whisper` | 官方非新接入推荐、增量体验差；`gpt-live-transcribe` 同价更优 |
| 纯 HTTP/SSE 流式替代 | 无纯 HTTP 实时增量方案（调研 C4），得不到持续增量 |

## 后果
- 现有 whisper-1 / chat / Volcengine 路径零改动，可回滚。
- 新增实时转录能力，通过 `apiMode="realtime"` 启用。
- 遗留风险：增量逐词实时程度（A1，置信度中-高）需最小原型实测确认；周期 commit 兜底保证可用性，不阻塞交付。

## 影响文件
- 新增：`src/addon/asr/realtime_asr.{h,cpp}`
- 修改：`voiceinput-config.h`、`engine.cpp`、`CMakeLists.txt`、`openai_asr.cpp`（Base64Encode 提升复用）
