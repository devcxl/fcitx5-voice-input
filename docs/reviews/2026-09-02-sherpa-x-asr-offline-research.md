# 调研报告：本地运行 X-ASR Zipformer Transducer 离线模型

> 状态：已完成（PoC 已执行）
> 日期：2026-09-02
> 关联：sherpa-onnx PR #3656 / #3662；模型 `sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03`

---

## 1. 执行摘要

- **建议**：暂定选择「新增 sherpa-onnx **OfflineRecognizer** 后端实现（新文件 `sherpa_offline_asr.cpp/.h`），复用现有 `AsrSession`/Pipeline 会话模型」。
- **适用前提**：sherpa-onnx submodule 需 ≥ 1.13.3（当前 16586713f，2026-08-28，满足）；模型文件放现有模型搜索目录即可。
- **核心依据**：X-ASR 为**非流式 zipformer2 transducer**（官方 PR #3656 标题与 `test_onnx.py` 均证实）；项目 submodule 的 `offline-transducer-model.cc` 已支持 `zipformer2` 离线解码；C API 提供 `OfflineRecognizer`/`OfflineStream` 全套接口。**PoC 已实测通过**：CPU 上 RTF≈0.04，10s 音频 0.4s 出结果。
- **主要代价**：无新依赖；需新增约 200~300 行 C++（新后端）+ 路径解析补 `bpe.model` 探测；离线模型延迟比流式高（整段一次解码，实测仍很快）；热词增强受 bpe_vocab 格式限制（需生成 token-score 文本词表）。
- **最大风险**：`UNVERIFIED` —— **已通过 PoC 解除核心风险**（模型可解码、C API 集成路径可行、延迟优异）；剩余限制为热词格式（增强项）。
- **下一步**：进入实现（新增 `sherpa_offline_asr.cpp/.h` + 配置枚举项 + engine 分支），需用户确认后端组织方式。

---

## 2. 调研任务卡

### 背景
用户希望将 fcitx5-voice-input 的 ASR 从云 API（OpenAI 兼容）扩展为**可本地运行**，给定模型为 X-ASR 非流式 zipformer transducer（2026-06-03 发布，int8）。当前代码已有 sherpa-onnx 流式后端（`sherpa_onnx_asr.cpp`，OnlineRecognizer），需评估能否直接复用，还是必须新增离线后端。

### 决策问题
1. 现有代码能否**直接**加载并识别该模型？
2. 若不能，最小改动路径是什么（改现有 / 新增离线后端）？
3. 需要调整哪些模块（CMake、配置、pipeline、路径解析、热词）？

### 业务场景
- 桌面 Linux 输入法插件（fcitx5），单用户本地运行。
- 语音输入：VAD 切分 → 整句提交；典型单句 1~10 秒。
- 隐私敏感（语音文本不出本机）；无网络或弱网环境可用。
- 硬件：x86-64 CPU（int8 推理，无 GPU 硬性要求）。

### 硬性约束
| 约束 | 说明 |
|---|---|
| 无新增外部依赖 | 不能引入新库/新子模块（sherpa-onnx 已内嵌 submodule） |
| 保持 `AsrSession` 抽象接口 | `FeedAudio`/`End`/`Cancel`/`StartWorker` 语义不变 |
| 构建不破坏现有后端 | openai/volcengine/mistral 流式后端继续可用 |
| 模型目录可配置 | 复用现有 `modelDir` 配置 + 自动探测逻辑 |
| 无网络运行时可用 | 模型完全本地加载 |

### 优先级
稳定性（不崩、优雅降级） > 开发成本（KISS） > 延迟（可接受整段解码） > 热词/标点等增强。

### 成功标准
- 模型在本地 CPU 上完成识别，文本正确输出到 fcitx 上屏。
- 单句（≤10s 音频）解码延迟可接受（目标：识别时间 < 音频时长数量级，不卡死 UI）。
- 现有 4 个云后端 + 流式 sherpa 后端不受影响。
- 热重载配置、取消会话、JoinWithTimeout 等既有机制对离线后端同样生效。

### 非目标
- 不评估 GPU 加速 / CUDA provider（约束 CPU）。
- 不评估流式 X-ASR 版本（用户给的是非流式包）。
- 不评估 sherpa-onnx 之外的其他本地 ASR 框架（模型已定）。
- 不实现 LLM 后处理、标点恢复等增强（已有 LLM 链路，不在此调研范围）。

### 决策截止时间
无硬性截止；随用户排期。

---

## 3. 场景、约束和假设

**关键假设**（标记置信度）：

| 假设 | 置信度 | 影响 |
|---|---|---|
| 用户希望**保留**现有流式 sherpa 后端 | `INFERRED`（在线模型仍可用） | 决定新增文件而非改写 |
| 模型目录采用现有 `modelDir` 配置 + 自动探测 | `INFERRED`（复用 KISS） | 决定路径解析改动 |
| 无 GPU，纯 CPU 推理 | `CONFIRMED`（现有代码 provider="cpu"） | 决定 int8 收益评估 |
| 本地已有可用的 onnxruntime 库（sherpa 依赖） | `CONFIRMED`（CMake 内嵌编译） | PoC 可行性基础 |

---

## 4. 候选方案与初筛结果

| # | 方案 | 说明 | 硬性约束 | 初筛 |
|---|---|---|---|---|
| A | **现有代码直接加载该模型** | 用 OnlineRecognizer 指向 X-ASR 文件 | ❌ 接口不匹配（online vs offline） | **淘汰** |
| B | **新增 `sherpa_offline_asr.cpp/.h`（OfflineRecognizer）**，保留现有流式后端 | 新后端类 + 路径解析补 bpe | ✅ 全部满足 | **推荐** |
| C | 改写现有 `sherpa_onnx_asr.cpp` 为离线实现 | 替换流式后端 | ⚠️ 牺牲现有在线模型支持 | 备选（不推荐） |
| D | 不引入本地 ASR，维持云 API | 基线方案 | ⚠️ 不满足"本地运行"目标 | 淘汰（基线对照） |

**淘汰理由**：
- **A**：X-ASR 是非流式模型，`SherpaOnnxCreateOnlineRecognizer` 加载 zipformer2 离线结构必然失败或结果错乱（`CONFIRMED`，见证据 E1/E2）。若尝试硬塞会得到 UAF/错误解码，风险不可接受。
- **C**：现有流式 zipformer 在线模型（`download-model.sh` 下载的 xlarge）是项目已支持能力，改写即破坏；新增文件是 SRP 常规做法（与 `RealtimeAsrEngine` 先例一致，见 ADR-2026-08-07 D3）。
- **D**：不满足本次"本地运行"核心诉求，仅作成本基线。

---

## 5. 证据矩阵

| # | 证据 | 来源 | 时间/版本 | 支持结论 | 等级 | 利益相关 |
|---|---|---|---|---|---|---|
| E1 | PR #3656 标题 "Upload X-ASR **non-streaming** zipformer transducer models"，合并于 2026-06-03，修改 `offline-transducer-model.cc`、`offline-recognizer-transducer-impl.h`、`offline-transducer-model.cc` | GitHub k2-fsa/sherpa-onnx | 2026-06-03 / v1.13.3 | X-ASR 包是非流式模型；sherpa 通过 **offline** transducer 支持它 | A | 官方仓库 |
| E2 | 官方 `test_onnx.py`：整段音频 → `compute_feat`（80 维 fbank）→ 一次 `run_encoder` → 逐帧 `run_joiner`/`run_decoder` | 模型压缩包内 | 2026-06-03 | 解码流程是非流式（offline）；建模单元为 BPE（`bpe.model` + `tokens.txt`） | A | 官方发布物 |
| E3 | submodule 提交 16586713f（2026-08-28）内含 `offline-transducer-model.cc`，代码含 `model_type == "zipformer2"` 分支 | 本地 third_party/sherpa-onnx | 2026-08-28 | 本地 submodule **已支持**离线 zipformer2 transducer | A | 本地源码 |
| E4 | C API `c-api.h`：`SherpaOnnxOfflineRecognizerConfig`（含 `model_config.transducer`、`tokens`、`bpe_vocab`、`modeling_unit`、`decoding_method`、`hotwords_file`）；`SherpaOnnxCreateOfflineStream`/`AcceptWaveformOfflineStream`/`DecodeOfflineStream`/`GetOfflineStreamResult` | 本地 submodule c-api.h | 2026-08-28 | 离线接口齐全，可实现会话化封装 | A | 本地源码 |
| E5 | 现有 `sherpa_onnx_asr.cpp`：`SherpaOnnxCreateOnlineRecognizer` + `c_cfg.model_config.modeling_unit = "cjkchar"`；无 bpe_vocab 字段；`c_cfg.enable_endpoint=0` | 本地代码 | 当前 | 现有实现是**流式**、cjkchar 建模单元，**无法直接**加载 X-ASR | A | 本地代码 |
| E6 | 模型压缩包：`encoder-epoch-99-avg-1.int8.onnx`(154M) + `decoder`(11M) + `joiner.int8`(2.5M) + `tokens.txt`(58K) + `bpe.model`(352K) | 已下载实测 `/tmp/...` | 2026-06-03 | 模型齐全；**int8 encoder**；BPE 词汇表存在 | A | 官方发布物（实测） |
| E7 | 现有 `ResolveSherpaOnnxModelPaths`：`FindMatchingModelFile` 按前缀 "encoder"/"decoder"/"joiner" + `.onnx` 匹配；`tokens.txt` 硬编码；`bpe.model` 有探测但赋值进 `res.bpe_vocab` | 本地代码 | 当前 | 路径解析可自动找到 X-ASR 文件（前缀匹配可行），但 **bpe_vocab 未接入 config 字段** | A | 本地代码 |
| E8 | `download-model.sh` 下载的是 `sherpa-onnx-streaming-zipformer-zh-xlarge-int8`（流式） | 本地脚本 | 当前 | 项目现有模型生态是流式；X-ASR 是新增方向 | A | 本地脚本 |
| E9 | `voiceinput-config.h`：SherpaOnnxAsrConfig 含 `modelDir`/`numThreads`/`hotwordsFile`/`hotwordsScore`，无 bpe/offline 开关 | 本地代码 | 当前 | 配置层需增加离线模型相关字段（或复用 modelDir 自动探测） | A | 本地代码 |
| E10 | ResultCoordinator：`HandleAsrResult(text, isFinal, sid)` 支持 partial（`isPartial=true`）与 final 两条路径；final 后 erase session | 本地代码 | 当前 | Pipeline 可消费"仅 final"的后端；partial 是增强非必需 | A | 本地代码 |
| E11 | `docs/architecture/v4-asr-session-model.md` L377："本地 ASR sherpa-onnx 接口已预留，实现待后续（v5）" | 本地文档 | 当前 | 新增后端是文档计划内演进，非临时 hack | A | 本地文档 |
| E12 | CMake：`ENABLE_SHERPA_ONNX` + submodule `add_subdirectory` + `HAVE_SHERPA_ONNX` 编译定义 | 本地 CMakeLists | 当前 | 构建层无需改动（offline 编译单元已随 submodule 编译） | A | 本地代码 |

**证据冲突**：无 `CONFLICTED` 项。E1/E2 与 E5 表面冲突（"能加载吗"），实质是 **online vs offline 两套接口**，已在 E2/E5 分别证实，属正常差异而非冲突。

---

## 6. 关键维度分析

### 6.1 功能与兼容性
- X-ASR 非流式模型 **不能** 用现有 OnlineRecognizer 加载（E5）。离线接口 E4 已存在。
- **BPE 建模单元**是最大不兼容点：现有配置 `modeling_unit="cjkchar"`、无 bpe_vocab 字段（E5/E7）。X-ASR 用 BPE（`bpe.model`，E6），离线 config 需填 `model_config.bpe_vocab` + `modeling_unit="bpe"`。
- `enable_endpoint` 对离线无意义（无流式端点概念），VAD 切分仍是主导（E5 已关闭端点，离线天然如此）。
- 热词：离线 `SherpaOnnxOfflineRecognizerConfig` 也有 `hotwords_file`/`hotwords_score`/`decoding_method="modified_beam_search"`（E4），可保留现有热词能力。

### 6.2 性能与容量
- **延迟模型**：整句一次 encoder 推理。对 ≤10s 语音，int8 encoder 154M 在 CPU 上预计秒级（未实测，`UNVERIFIED`）。
- 与流式对比：流式边说边出 preedit；离线只能 `End()` 后出全文。**交互体验回退**（无增量 preedit），但准确性更高（大模型）。
- 内存：int8 模型约 168M 文件 + onnxruntime 运行内存，桌面环境可接受。

### 6.3 稳定性与恢复
- `AsrSession` 生命周期机制（`shared_from_this` 保活、`JoinWithTimeout`、`Cancel` 原子标志）对离线后端**完全复用**（E10 设计支持）。
- 离线 worker 在 `End()` 后一次性解码，无中途失败重试路径；解码异常需 catch 并回调错误（现有实现已有 try/catch 模式）。
- 热重载：`SherpaRecognizerHolder` 的共享持有 + 析构顺序需对 OfflineRecognizer 复制同样模式（新 holder）。

### 6.4 运维与可观测性
- 模型目录沿用 `modelDir`（留空自动探测 `~/.local/share/fcitx5/voice-input/models` 等，E7），用户只需把 X-ASR 目录放进去或指定路径。
- 日志沿用 FCITX_DEBUG/INFO 模式。

### 6.5 安全与合规
- 无新增依赖 → 无新供应链面（E12）。
- 语音不出本机（本地模型）→ 隐私收益，是此方案主要价值。
- 模型文件需用户自行下载（~130M bz2）；脚本 `download-model.sh` 可加第二个下载项。

### 6.6 生态与维护状态
- sherpa-onnx 活跃维护（2026-06 发布 v1.13.3，含 X-ASR 支持；submodule 2026-08 仍更新，E3）。
- X-ASR 模型来自字节系 Gilgamesh-J/X-ASR 开源（README 标注来源，E6），sherpa 官方 CI 集成（E1）。

---

## 7. PoC 计划与结果

> 状态：**已完成（2026-09-02）**，核心风险全部解除，仅热词增强受格式限制

### 环境
- 本机 x86-64 Arch，CPU provider；venv onnxruntime 1.29.0 + kaldi_native_fbank 1.22.3（P1）；系统 onnxruntime 1.28.0 + 项目现有 `build/lib/libsherpa-onnx-c-api.so`（P2/P3）。

### P1：官方 test_onnx.py（Python 直跑）— ✅ 通过
- **修正**：官方脚本 decoder/encoder 输入用 `int32`，而模型实际要求 `int64`（PR #3656 修复的 tensor 类型问题），改 `dtype=np.int64` 后通过。
- 4 个测试音频全部识别成功（中英混说），单段 2.1~2.4s。

### P2：sherpa C API 离线识别（项目集成路径）— ✅ 通过
- 用项目已有 `libsherpa-onnx-c-api.so` + `SherpaOnnxOfflineRecognizer` 编译 PoC（`/tmp/poc_offline.c`）。
- 配置：`feat_config{sample_rate=16000, feature_dim=80}` + `transducer{encoder,decoder,joiner}` + `tokens` + `num_threads=2` + `greedy_search`，**无需 bpe_vocab**。
- 结果（与 P1 文本一致，无 BPE 标记）：

| wav | 音频时长 | 解码耗时 | RTF |
|---|---|---|---|
| 0.wav | 10.05s | 0.41s | 0.04 |
| 1.wav | 5.10s | 0.23s | 0.05 |
| 2.wav | 4.69s | 0.20s | 0.04 |
| 3.wav | 8.83s | 0.34s | 0.04 |

### P3：长音频线性扩展 — ✅ 通过
- 拼接 30.16s 音频：解码 1.43s，RTF=0.05，线性扩展稳定。

### 热词路径（modified_beam_search + bpe_vocab）— ⚠️ 限制发现
- 传 `bpe.model` 到 `bpe_vocab` 报错 `exit(-1)`：`Ssentencepiece::LoadVocab`（`build/_deps/simple-sentencepiece-src/ssentencepiece/csrc/ssentencepiece.cc`）期望**文本格式** `token score` 每行两项，而 `bpe.model` 是 sentencepiece 二进制 proto，**格式不兼容**。
- 官方 issue #2977 回复确认：hotwords 仅在 `modified_beam_search` 生效，且 `bpe_vocab` 需与模型 tokenization 匹配。
- 结论：greedy_search 路径完全可用且无需 bpe；热词若要支持，需额外生成「token score 文本词表」（可从 tokens.txt/bpe.model 派生），属增强项，不阻塞主路径。

### PoC 结论
本地运行 X-ASR 完整可行，且 CPU 性能优异（RTF≈0.04~0.05，10s 音频 0.4s 出结果），远优于调研预期。实现可进入。

---

## 8. 对比评分矩阵

权重按本场景（稳定性 25% / 开发成本 25% / 性能 15% / 运维 15% / 生态 10% / 成本 10%）。

| 维度 | 权重 | A: 直接加载 | B: 新增离线后端 | C: 改写现有 | D: 维持云 |
|---|---|---|---|---|---|
| 功能匹配 | 25% | 1（不可行） | 9（完全匹配） | 8（牺牲流式） | 4（无本地） |
| 稳定性与恢复 | 20% | 1 | 8（复用会话机制） | 6 | 8 |
| 性能与延迟 | 15% | 1 | 7（整段解码） | 7 | 6（网络延迟） |
| 运维复杂度 | 15% | 1 | 8（无新依赖） | 6 | 7 |
| 生态与维护 | 10% | 1 | 9（官方支持） | 6 | 7 |
| 成本与退出 | 15% | 1 | 9（零依赖） | 7 | 5（API 费用） |
| **加权总分** | | **1.0** | **8.3** | **6.9** | **6.2** |

**评分依据**：B 各维均为最高或次高；C 因破坏现有流式能力扣分；D 无本地能力且持续 API 成本。
**结论**：B 显著胜出，且符合项目文档规划（E11）。

---

## 9. 风险与 TCO

### 风险清单
| 风险 | 概率 | 影响 | 触发条件 | 缓解 |
|---|---|---|---|---|
| 模型在本地 CPU 解码延迟过高 | 中 | 高（体验差） | 长句/低端 CPU | PoC P3 实测；限制单句长度；提示用户可用流式模型 |
| BPE 配置错误导致乱码/空输出 | 中 | 中 | 忘填 `bpe_vocab` 或 modeling_unit 不匹配 | 路径解析强制探测 bpe.model；Init 校验；测试音频回归 |
| int8 encoder 与 sherpa 离线模型类型不匹配 | 低 | 高 | submodule 低于 1.13.3 | 已确认 submodule 2026-08 支持（E3）；构建期加版本校验 |
| 热词文件存在但 decoding_method 冲突 | 低 | 中 | 热词 + greedy 混配 | 复用现有 Init 逻辑（hotwords 存在则 modified_beam_search） |
| 取消会话时离线解码不可中断 | 中 | 低 | 长音频解码中 Cancel | worker 循环检查 cancelled 标志；解码完成后丢弃结果 |

### TCO（1~3 年）
| 项 | 估算 | 说明 |
|---|---|---|
| 软件许可 | 0 | sherpa-onnx Apache-2.0；X-ASR 开源；无 API 费用 |
| 模型存储 | 一次性 ~170MB | 本地磁盘 |
| 开发接入 | 0.5~1 人日 | 新增后端 + 路径解析 + 测试 |
| 运维 | 0 | 本地无服务器 |
| 退出成本 | 低 | 后端抽象已存在；删除文件即可回退云 API |

**锁定风险**：无数据格式锁定（模型文件标准 ONNX）；无专有 API 依赖。

---

## 10. 推荐方案与适用边界

### 推荐方案
**方案 B：新增 sherpa-onnx 离线后端（`sherpa_offline_asr.cpp/.h`），复用 `AsrSession` 抽象与 Pipeline，保留现有流式 sherpa 后端。**

### 核心理由
1. X-ASR 是非流式模型，现有 OnlineRecognizer 接口**物理不兼容**（E1/E2/E5），方案 A 直接淘汰。
2. 本地 submodule 已含离线 zipformer2 支持（E3/E4），**零新依赖**实现本地 ASR。
3. 项目 `AsrSession` 会话模型（FeedAudio/End/Cancel + ResultCoordinator 的 partial/final）天然适配"整段缓冲 + End 后解码"（E10），改动局限在 ASR 目录，不碰 pipeline。
4. 符合项目文档既有规划（E11），与 Realtime 引擎先例一致（D3）。

### 为什么不选其他方案
- A：接口不匹配，必然失败。
- C：破坏现有流式能力，收益（少一个文件）远小于代价。
- D：不满足本地运行目标。

### 实施要点（最小改动清单）
1. **新增** `src/addon/asr/sherpa_offline_asr.h/.cpp`：
   - `SherpaOfflineRecognizerHolder`（持 OfflineRecognizer，互斥锁 + 共享指针，仿现有 holder）。
   - `SherpaOfflineAsrSession`：`FeedAudio` 只缓冲（复用 `ThreadSafeQueue` 或直接 vector+mutex）；`End()` 后 worker 一次性 `AcceptWaveformOfflineStream` → `DecodeOfflineStream` → `GetOfflineStreamResult` → 回调 `(text, isFinal=true)`。
   - `SherpaOfflineAsrEngine`：`Init` 填 `SherpaOnnxOfflineRecognizerConfig`，`modeling_unit="bpe"` + `bpe_vocab`；热词复用现有探测。
2. **扩展路径解析**：`ResolveSherpaOnnxModelPaths` 对 offline 场景必须解析出 `bpe.model`；现有 `FindMatchingModelFile` 前缀匹配可复用（encoder/decoder/joiner 均匹配 X-ASR 命名）。
3. **配置层**：`voiceinput-config.h` 的 Sherpa 配置增加离线开关或复用 modelDir（推荐：新增 `UseOffline` 布尔或后端枚举项 `sherpa_onnx_offline`，与现有 `Enum/3` 平级）。
4. **engine.cpp** `CreateAsrEngine`：按配置实例化离线或在线 sherpa 后端。
5. **CMake**：无需改动（HAVE_SHERPA_ONNX 已覆盖编译单元）；若新增文件自动纳入 glob 需确认现有 target 源列表。
6. **download-model.sh**：增加 X-ASR 下载项（可选）。

### 适用边界
- 适用于：桌面 Linux 单机、CPU 推理、隐私优先、可接受"整句出结果"（无增量 preedit）的用户。
- 不适用于：需要边说边出增量上屏的低延迟场景（应继续用流式模型/云 API）。

### 重新评估条件
- sherpa-onnx 发布流式 X-ASR（2026-06 已有 `sherpa-onnx-x-asr-streaming-*` 包，见 release tag）→ 若用户更看重增量体验，改评估流式 X-ASR。
- 出现 GPU/其他 provider 需求 → 重评 int8/延迟。
- onnxruntime 或 sherpa 版本升级引入 breaking change → 重评兼容性。

---

## 11. 待验证事项

| 项 | 状态 | 验证方法 | 影响 |
|---|---|---|---|
| 本地 CPU 解码 X-ASR 实际延迟 | `UNVERIFIED` | PoC P3 | 若过高，需限制输入时长或改用流式模型 |
| onnxruntime 版本兼容性（int8 zipformer2 离线） | `UNVERIFIED` | PoC P1/P2 | 若失败，检查 sherpa submodule 版本 |
| 现有 CMake 源文件收集方式（是否需手动加新文件） | `UNVERIFIED` | 查看 CMakeLists 源列表 | 影响新增文件是否自动编译 |
| BPE 建模单元配置正确性 | `UNVERIFIED` | PoC P2 + 中文/英文测试音频 | 影响识别质量 |
| 热词在离线后端的实际效果 | `UNVERIFIED` | 实现后用热词文件测试 | 影响功能完整性 |

---

## 12. ADR

见 `docs/adr/2026-09-02-sherpa-x-asr-offline-asr.md`（单独文件，本报告为分析过程存档）。

---

## 13. 证据来源

| 来源 | 链接/位置 | 访问时间 |
|---|---|---|
| sherpa-onnx PR #3656 | https://github.com/k2-fsa/sherpa-onnx/pull/3656 | 2026-09-02 |
| sherpa-onnx PR #3662（导出脚本） | https://github.com/k2-fsa/sherpa-onnx/pull/3662 | 2026-09-02 |
| sherpa-onnx v1.13.3 release | https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.3 | 2026-09-02 |
| X-ASR 模型包 README | `README.md`（来源 Gilgamesh-J/X-ASR） | 2026-09-02 |
| 模型 test_onnx.py | `/tmp/sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03/test_onnx.py` | 2026-09-02 |
| sherpa-onnx submodule c-api.h | `third_party/sherpa-onnx/sherpa-onnx/c-api/c-api.h` | 2026-09-02 |
| sherpa-onnx submodule offline-transducer-model.cc | `third_party/sherpa-onnx/sherpa-onnx/csrc/offline-transducer-model.cc` | 2026-09-02 |
| 项目现有实现 | `src/addon/asr/sherpa_onnx_asr.cpp` / `asr_engine.h` / `asr_session.h` / `engine.cpp` / `voiceinput-config.h` | 2026-09-02 |
| 项目文档 | `docs/architecture/v4-asr-session-model.md` | 2026-09-02 |
