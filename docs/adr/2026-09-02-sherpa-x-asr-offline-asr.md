# ADR-2026-09-02：sherpa-onnx 离线后端支持 X-ASR 非流式模型

> 状态：已接受（PoC 已通过）
> 日期：2026-09-02
> 决策方：@architect
> 关联：调研报告 `docs/reviews/2026-09-02-sherpa-x-asr-offline-research.md`；sherpa-onnx PR #3656/#3662；v4-asr-session-model.md（"本地 ASR 实现待 v5"）

## 背景

用户要求本地运行 X-ASR zipformer transducer 离线模型（`sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03.tar.bz2`，~170MB，int8）。现有代码仅有 sherpa-onnx **流式**（OnlineRecognizer）后端，接口与离线模型不匹配。目标：零新依赖、保留现有云/流式后端，为隐私敏感或无网环境提供本地 ASR。

## 决策

### D1：X-ASR 包为非流式模型，现有 OnlineRecognizer 无法直接加载
- 证据：sherpa-onnx PR #3656 标题明确 "non-streaming zipformer transducer"；官方 `test_onnx.py` 为整段音频一次性 encoder + 逐帧 joiner/decoder 的离线解码流程。
- 现有 `sherpa_onnx_asr.cpp` 使用 `SherpaOnnxCreateOnlineRecognizer` + `AcceptWaveform`，**不兼容**该模型。
- 结论：必须走 sherpa-onnx **OfflineRecognizer** 接口。

### D2：新增独立离线后端文件，不改写现有流式实现
- 新建 `src/addon/asr/sherpa_offline_asr.cpp/.h`，包含：
  - `SherpaOfflineRecognizerHolder`：共享持有 `SherpaOnnxOfflineRecognizer`，互斥锁串行化 decode，仿现有 holder 生命周期（防热重载 UAF）。
  - `SherpaOfflineAsrSession`：`FeedAudio` 缓冲 PCM（复用现有队列/vector 模式）；`End()` 后 worker 一次性 `AcceptWaveformOfflineStream` → `DecodeOfflineStream` → `GetOfflineStreamResult` → 回调 `(text, isFinal=true)`。
  - `SherpaOfflineAsrEngine`：`Name()="sherpa_offline"`。
- 理由：与 RealtimeAsrEngine 先例一致（ADR-2026-08-07 D3，SRP）；现有流式 zipformer 在线模型（download-model.sh 的 xlarge）继续可用。

### D3：离线模型配置使用 BPE 建模单元
- `SherpaOnnxOfflineRecognizerConfig.model_config`：
  - `transducer.{encoder,decoder,joiner}` 指向 X-ASR 三件套。
  - `tokens` = tokens.txt。
  - `bpe_vocab` = bpe.model（X-ASR 为 BPE 模型，**必须**配置，现有 cjkchar 配置不可复用）。
  - `modeling_unit = "bpe"`。
  - `num_threads`/`provider="cpu"` 沿用现有配置。
  - `decoding_method`：无热词 `greedy_search`，有热词 `modified_beam_search`（沿用现有逻辑）。
  - `enable_endpoint` 概念不存在于离线；断句仍由 Pipeline VAD 主导。
- 现有 `ResolveSherpaOnnxModelPaths` 前缀匹配可复用（encoder/decoder/joiner 命名兼容），需补 `bpe.model` 强制探测与校验。

### D4：配置层新增离线后端开关
- `voiceinput-config.h` 后端枚举新增 `sherpa_offline`（与现有 4 后端平级），或 Sherpa 配置加 `UseOffline` 布尔。
- 优先方案：新增独立枚举项 `sherpa_offline`（显式、与 `Enum/3` 风格一致），UI 文案 "Sherpa-ONNX (Local Offline)"。
- `engine.cpp::CreateAsrEngine` 增加对应分支，复用 `modelDir`/`numThreads`/`hotwords*` 配置字段。

### D5：CMake 与依赖零改动
- 本地 submodule（16586713f，2026-08-28）已含离线 zipformer2 transducer 编译单元与 C API；`HAVE_SHERPA_ONNX` 编译定义已覆盖。
- 仅需确认 target 源文件收集方式（glob 或显式列表），新增 .cpp 纳入编译。

### D6：交互语义 = 整句出结果（无增量 preedit）
- 离线后端仅产生 `isFinal=true` 回调（ResultCoordinator 已支持，partial 为可选增强）。
- 期望管理：VAD 静音切分 → 整句提交 → 全文上屏；不承诺流式增量。
- 若未来有增量需求，优先评估流式 X-ASR 包（sherpa-onnx-x-asr-streaming-*，2026-06 已发布）而非改造离线后端。

### D7：下载脚本补充 X-ASR 选项
- `scripts/download-model.sh` 增加第二个模型选项（X-ASR int8，默认目录不变），便于用户一键获取。

## 后果

**正面**：本地零依赖 ASR；隐私（语音不出机）；无 API 费用；现有后端不受影响。
**负面**：无增量 preedit（交互回退）；CPU 解码延迟高于流式；模型 ~170MB 磁盘占用。
**风险与缓解**：
- CPU 延迟过高 → PoC 实测，必要时限制单句时长。
- BPE 配置错误 → Init 校验 + 测试音频回归。
- 取消不可中断 → worker 循环检查 cancelled，解码后丢弃。

## 待办（实施前置）
- [x] PoC：本地 onnxruntime 跑官方 test_onnx.py + sherpa 离线 demo，验证解码与延迟（调研报告 §7）—— **已通过**：RTF≈0.04，热词 bpe_vocab 格式限制已记录。
- [x] 确认 CMake 源文件收集方式（ADDON_SOURCES 显式列表，需手动加新 .cpp）。
- [ ] 实施 D2~D7 后补测试音频回归（中英混说、热词）。

## 相关文档
- 调研报告：`docs/reviews/2026-09-02-sherpa-x-asr-offline-research.md`
- 会话模型：`docs/architecture/v4-asr-session-model.md`
- 历史 ADR：`docs/adr/2026-08-07-gpt-realtime-asr.md`（D3 独立引擎先例）
