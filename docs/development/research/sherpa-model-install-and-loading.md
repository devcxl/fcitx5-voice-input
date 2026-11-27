# 调研：本地 ASR 模型的安装、选择、验证与加载机制 —— 业内最佳实践与 fcitx5-voice-input 落地取舍

> 面向 fcitx5-voice-input（C++ Fcitx5 addon，进程内多线程：主线程 / Capture / VAD Worker / ASR Worker / SessionReaper）。
> 调研范围：`feat/sherpa-onnx-x-asr` 的模型安装、选择、验证、加载链路，与外部同类系统的横向比较。
> 调研日期：2026-11-24。代码基线：`1708687d6a8b029c6845d226b0508b62f87ff258`（2026-11-09），sherpa-onnx submodule `34eba5a27220026b5981b633981c53205515067d`（2026-08-25）。
> 约束：本文件不合并 PR、不改实现、不下载模型（仅读取官方 release API 元数据）。下文严格区分**观察事实（Fact）**、**推荐（Recommendation）**与**未知/待确认（Unknown）**。
> 所有外部引用尽量使用固定 tag 或 commit 的永久链接；无法核实的地方明确写「待确认」。本文件不声称任何做法是「行业共识」——案例之间差异很大，见 §7。

---

## 0. 结论摘要

| # | 结论 | 性质 | 依据 |
|---|------|------|------|
| C1 | 当前两个本地后端（`sherpa_onnx` 流式 / `sherpa_offline` 非流式）共用同一个 `ResolveSherpaModelPaths()`，且该函数**只按文件名前缀 `encoder*`/`decoder*`/`joiner*` 与 `tokens.txt` 判存在性**，不校验模型是不是 transducer 的在线/离线变体，也不校验版本。两个后端各自配置 `ModelDir`，但若两者都留空，会解析到**同一个目录**。 | Fact | `src/addon/asr/utils/sherpa_paths.cpp:99-149`；`src/addon/asr/sherpa_onnx_asr.cpp:248-257`；`src/addon/asr/sherpa_offline_asr.cpp:227-236` |
| C2 | 串用的直接后果不是「识别变差」，而是**杀掉 Fcitx5 主进程**，且两个方向的失败机制不同：<br>**(a) 在线模型 → 离线后端**：`OfflineTransducerModel::InitEncoder` 会对 encoder 的 **input[1] 与 output[1] 强制要求 int32/int64**（`ValidateIntTensorType`），而在线 zipformer2 的 input[1] 是 float32 状态张量 → 直接 `SHERPA_ONNX_EXIT(-1)`（`_Exit`）。<br>**(b) 离线模型 → 在线后端**：`GetModelType()` 读不到 metadata `model_type` 时返回 `kUnknown` → `Create()` 返回 **`nullptr`** → 调用方紧接着 `model_->SetFeatureDim(...)` **空指针解引用（SIGSEGV）**；若离线包确实带 `model_type=zipformer2`，则会构造 `OnlineZipformer2TransducerModel` 并因缺 `encoder_dims`/`num_heads` 等向量 metadata 而 `SHERPA_ONNX_EXIT(-1)`。<br>两条路径都致命，**但都不是 C++ 异常**，`try/catch` 接不住。 | Fact（机制，已读源码）/ 待确认（方向 b 走 SIGSEGV 还是 `_Exit` 取决于模型的 metadata 集合） | `macros.h:54-58`（`_Exit`）；`offline-transducer-model.cc:254-266,82-89`（int 张量校验）；`online-zipformer2-transducer-model.cc:470-483`（input 布局）；`online-transducer-model.cc:71-79,181-187`；`online-recognizer-transducer-impl.h:104-106`（无空指针检查） |
| C3 | 仓库当前的验证只有「文件存在 + 文件名前缀」，**没有摘要校验、没有尺寸校验、没有版本/来源记录**；下载脚本用 `curl` 拉 tar.bz2 后直接 `cp`，失败即 `set -e` 中断但已清理目标目录的旧 `*.onnx`。 | Fact | `scripts/download-model.sh:30-56`；全仓 `grep sha256\|checksum\|digest` 在 `src/`、`scripts/`、`CMakeLists.txt` 无命中（只有 WS handshake 测试里的 SHA-1） |
| C4 | 上游确实提供了可核验的完整性凭据：GitHub Release API 的 asset 对象带 `digest` 字段。实测 `asr-models` release 的 501 个 asset 中，`sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03.tar.bz2` 为 `size=136902463`、`digest=sha256:7592d1a1e9bc1e034c39fbaab9b0db8af46429d95d64c6e2a69593934af10e9c`。也就是说「记录官方摘要并校验」在数据源上**是可行的**，成本低。 | Fact | `GET https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/tags/asr-models`（本次实测，2026-11-24） |
| C5 | 三个横向案例对「身份/完整性」的处理深度不同：Ollama 走 **manifest + content-addressed blob（sha256 文件名）+ pull 后全量 verify + 不匹配即删除重下**；Hugging Face Hub 走 **revision(commit) + blobs/snapshots/refs + etag 校验 + `.locks/` 并发锁 + 进程唯一 `.incomplete` 临时文件 + 原子 rename**；whisper.cpp 只做 **文件魔数（magic）校验 + 存在即跳过**，完全不校验来源或摘要。可见「完整性校验强度」在这个领域并非统一实践，取决于分发模型（registry vs git-backed vs 单文件下载）。 | Fact | 见 §3.1–§3.3 的源码固定链接 |
| C6 | 三者对「加载」的边界处理也不一致：whisper.cpp 的加载函数在魔数不符时**返回 false/nullptr**；sherpa-onnx 在同类错误上使用 `SHERPA_ONNX_EXIT(-1)`（即 `_Exit`），并在 `OnlineTransducerModel::Create()` 返回 `nullptr` 后**直接解引用**而不检查。因此**「用 sherpa C API 做进程内试加载」本身不是安全的验证手段**——试加载失败可能是进程退出或段错误。这一点与「是否真的需要子进程」的讨论直接相关。 | Fact | `whisper.cpp:1504-1509`；`macros.h:54-58`；`online-recognizer-transducer-impl.h:104-106` |
| C7 | Ollama/HF 的「暂存 + 提交」语义都不依赖「覆盖写目标文件」：Ollama 写 `blobs/sha256-<hex>` 再写 manifest（内容寻址天然幂等）；HF 写唯一临时文件后 `os.replace` 原子换入，并再复核尺寸。仓库当前脚本是「先 `rm -f` 旧文件，再 `cp` 新文件」，**存在断电/中断后目录半新半旧的窗口**。 | Fact（对比）/ Recommendation（改进方向） | `download.go:223`；`file_download.py:741,1969`；`download-model.sh:44-53` |
| C8 | 进程隔离成本可以量化落地：sherpa-onnx 的模型加载器在构造 `Ort::Session` 时会读 metadata 并要求若干键存在（见 C2）。**一次独立子进程 `--verify-model` 试加载**可以把「进程被杀」转成「退出码非零」。相比之下「进程外推理」的成本高得多（要跨进程搬 PCM 与结果、要重做 generation/保序语义），不应与「试加载」捆绑决策。 | Recommendation（分层结论） | §6.3 |
| C9 | XDG 规范层面，仓库只读了 `XDG_DATA_HOME`/`XDG_CACHE_HOME`，**未读 `XDG_DATA_DIRS`**；而系统级模型安装到 `${CMAKE_INSTALL_FULL_DATADIR}/fcitx5/voice-input/models`（通常 `/usr/share/...`），却把 `/usr/local/share/fcitx5/voice-input/models` 硬编码进搜索路径。这在「非 /usr 前缀安装」与「多前缀共存」时行为不自洽。 | Fact | `sherpa_paths.cpp:88-95,119-127`；`CMakeLists.txt:104`；XDG spec §3 |
| C10 | 隐私/无云回退方面：本地后端是**纯本地推理**（ONNX Runtime CPU），但仓库在同一进程内默认仍可能走云后端；「无云回退」目前是**用户手动切后端**，没有自动降级路径。这属于产品取舍，不是技术缺口；本报告不建议在没有明确需求时增加自动切换。 | Fact（现状）/ Recommendation（保持显式） | `engine.cpp:335-410`（后端分支 + Init 失败返回 nullptr）；`docs/architecture/index.md:233` |

---

## 1. 调研任务与方法

### 1.1 要回答的问题

1. **模型身份/版本/后端绑定**：如何让「这个模型文件属于哪个模型、哪个版本、哪个后端」成为可校验的事实，而不是靠目录约定猜。
2. **文件完整性 vs 来源真实性**：哈希能证明什么、不能证明什么；签名是否必要。
3. **显式下载 / 用户体验 / 离线导入**：一键脚本、GUI、受控透明下载三者的取舍。
4. **暂存与提交的并发/断电语义**：写到一半崩溃后目录处于什么状态。
5. **加载与推理解耦、进程隔离成本**：试加载是否值得子进程化；进程外推理是否必要。
6. **隐私与无云回退**。

### 1.2 方法

- 读仓库代码：`sherpa_paths.{h,cpp}`、两个 sherpa 引擎、`engine.cpp` 的 `CreateAsrEngine/ReloadActiveAsrClient`、`pipeline.cpp` 的 `SetAsrEngine`、`scripts/download-model.sh`、`CMakeLists.txt`、`tests/sherpa_paths_test.cpp`。
- 读 sherpa-onnx submodule 源码（固定 commit），确认失败边界与 metadata 依赖。
- 读外部一手资料：Ollama v0.40.17 源码、huggingface_hub v2.4.0 源码与官方缓存文档、whisper.cpp v1.9.6 源码与下载脚本、Wyoming faster-whisper 源码、XDG Base Directory Spec、OSTree/Flatpak 文档、GitHub Release API 文档。
- 只调用官方 API 读取元数据（不下载 tar.bz2、不安装模型）。

### 1.3 立场声明

本报告把「业内最佳实践」拆成**可核验的具体机制**，而不是提炼成一句口号。凡是案例之间不一致的地方（例如是否做摘要校验），一律如实并列，并说明差异来源。**没有任何一条建议被表述为「行业共识」。**

---

## 2. 仓库现状（观察事实）

### 2.1 模型查找：一个函数、两种后端、无身份概念

`ResolveSherpaModelPaths()` 的查找顺序（`sherpa_paths.cpp:99-149`）：

1. 四个 override 全非空且都存在 → 直接用；
2. `modelDir`（配置项）；
3. `$XDG_DATA_HOME/fcitx5/voice-input/models`（缺省 `$HOME/.local/share/...`）；
4. 硬编码 `/usr/local/share/fcitx5/voice-input/models`。

每个 base 先检查自身，再用 `directory_iterator` 检查**一级子目录**；子目录遍历顺序由文件系统决定（C++ 标准：`directory_iterator` 的迭代顺序 unspecified，见 cppreference 相关条目），命中第一个即返回。文件名匹配规则：

```cpp
// sherpa_paths.cpp:24-45（摘）
if (filename.rfind(prefix, 0) != 0) continue;         // 前缀 encoder/decoder/joiner
if (filename.rfind(".onnx") == std::string::npos) continue;
if (filename.find(".int8.onnx") != std::string::npos) int8Candidate = ...
else fp32Candidate = ...
return !int8Candidate.empty() ? int8Candidate : fp32Candidate;
```

也就是说：

- **只看文件存在与文件名前缀**，不看 ONNX metadata（`model_type`、`vocab_size`、`context_size`、`encoder_dims` 等）。
- **不区分在线/离线**：同一套前缀规则同时服务 `OnlineRecognizer` 与 `OfflineRecognizer`。
- **`tokens.txt` 是唯一被固定名字的文件**，其余靠前缀。
- 两个后端各有独立 `ModelDir` 配置（`voiceinput-config.h:186-187`、`205-206`），但**默认都为空**；为空时二者解析到同一组路径。

`tests/sherpa_paths_test.cpp` 覆盖了 int8 优先、一级子目录、override 优先等行为，但**没有任何「在线模型喂给离线后端」或「离线模型喂给在线后端」的用例**（`tests/sherpa_paths_test.cpp:59-110`）。

### 2.2 加载：同步、进程内、失败即弃

两个引擎的 `Init()` 都在调用线程内同步完成模型加载（`SherpaOnnxCreateOnlineRecognizer` / `SherpaOnnxCreateOfflineRecognizer`），并用 `try/catch` 包裹（`sherpa_onnx_asr.cpp:291-306`；`sherpa_offline_asr.cpp:288-303`）。

关键点：**这个 `try/catch` 只能接住 C++ 异常，接不住 `_Exit(-1)`，也接不住空指针解引用**。sherpa-onnx 用宏 `SHERPA_ONNX_EXIT(code)` 实现「错误即退出」：

```c
// sherpa-onnx/csrc/macros.h:54-58（固定 commit 链接见 §8）
#define SHERPA_ONNX_EXIT(code) \
  do { fflush(stdout); fflush(stderr); _Exit(code); } while (0)
```

`_Exit` 跳过所有析构、atexit、C++ 栈展开。因此下列任一情况都会直接终止 Fcitx5 进程：

- **离线后端拿到在线模型**：`InitEncoder` 对 encoder 的 input[1]/output[1] 强制 `int32|int64`（`offline-transducer-model.cc:254-266` + `ValidateIntTensorType` `:82-89`），在线 zipformer2 的 input[1] 是 float32 状态张量（`online-zipformer2-transducer-model.cc:470-483`）→ `_Exit(-1)`；
- **离线后端** encoder 缺 `vocab_size`/`context_size` metadata（`offline-transducer-model.cc:303-304`，`SHERPA_ONNX_READ_META_DATA` 缺失即 EXIT）；
- **在线后端拿到离线模型**：`GetModelType()` 读不到 `model_type` → `kUnknown` → `Create()` 返回 `nullptr`（`online-transducer-model.cc:71-79,181-187`），而 `OnlineRecognizerTransducerImpl` 构造函数**不检查**就调用 `model_->SetFeatureDim(...)`（`online-recognizer-transducer-impl.h:104-106`）→ **SIGSEGV**；若模型带 `model_type=zipformer2`，则改走缺 `encoder_dims` 等向量 metadata 的 `_Exit` 路径（`online-zipformer2-transducer-model.cc:115-125`）；
- `decoding_method` 非法（`offline-recognizer-transducer-impl.h:115-120`）；
- `bpe_vocab` 指向 sentencepiece 二进制 proto —— 这条已有 ADR 记录（`ADR-0005` D3），机制在 `simple-sentencepiece` 的 `LoadVocab`：每行必须能解析出两个字段，否则 `exit(-1)`（`ssentencepiece.cc:248-262`）。

> **注意语义差别**：`simple-sentencepiece` 用的是 `exit(-1)`（会跑 atexit、flush stdio），sherpa 主库用 `_Exit(code)`（不跑），而空指针路径是 SIGSEGV（连 C++ 栈都不展开）。三者对宿主进程都是致命的，且**症状不同**（退出码 vs 段错误），排查时不能只看退出码。

### 2.3 热切换：新引擎成功才替换，失败保留旧引擎

`ReloadActiveAsrClient()`（`engine.cpp:447-455`）与 `Pipeline::SetAsrEngine()`（`pipeline.cpp:104-127`）：

- 新引擎 `Init()` 返回 false → 不动旧引擎，只打 warn（**这是好设计**，避免「切换后端把可用后端也弄没」）；
- 新引擎成功 → `SkipAllSessions()` + 交换指针 + `previousEngine->CancelAllSessions()`。

但注意：**同步加载发生在主线程**（`setConfig` → `ReloadActiveAsrClient`，`setSubConfig` 同样，`engine.cpp:130-142,196-201`）。加载 ~137MB int8 模型期间 Fcitx5 主线程阻塞；`docs/guide/troubleshooting.md:105` 已经提示「首次激活可能包含模型加载等一次性开销」，但没有上限承诺。

### 2.4 下载脚本：可用但非事务性

`scripts/download-model.sh`：

```bash
# 44-53 行（摘）
rm -f "${TARGET_DIR}"/*.onnx "${TARGET_DIR}"/tokens.txt "${TARGET_DIR}"/bpe.model
SRC_DIR="${TMP_DIR}/${MODEL_NAME}"
cp -v "${SRC_DIR}"/encoder*.onnx "${TARGET_DIR}/"
...
```

观察：

- 下载到 `mktemp -d` 临时目录 → 解压 → **删除目标目录旧文件** → `cp` 新文件。中间任何一步失败（磁盘满、Ctrl-C），目标目录就是「部分旧文件 + 部分新文件」或「空目录」。
- 没有 `sha256sum` 校验，没有版本记录文件（例如 `.manifest.json`），没有「当前使用哪个模型」的状态。
- 默认在线模型是 `sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30`，实测 release asset `size=597755927`（约 570 MiB）、`digest=sha256:30437d84dc4861740d40166212ab05f3728945443d53691620075c4031fb88e1`。**这个体积对「一键脚本」体验影响很大**（见 §6.1）。

### 2.5 安装路径与 XDG

- `CMakeLists.txt:104`：`VOICE_INPUT_MODEL_DIR = ${CMAKE_INSTALL_FULL_DATADIR}/fcitx5/voice-input/models`，用于**打包 Silero VAD 模型**（`CMakeLists.txt:357-358`）。
- sherpa 模型**不随包分发**（`docs/architecture/index.md:384`：「模型不随包分发，用 `scripts/download-model.sh` 获取」）。
- `ResolveSherpaModelPaths` 不读 `${CMAKE_INSTALL_FULL_DATADIR}`，而是硬编码 `/usr/local/share/...`。若发行版以 `/usr` 前缀安装并由包管理器放置模型，该路径**不会被扫描**（详见 §5.2）。

### 2.6 现有文档与 ADR 的边界

- `docs/architecture/adr/ADR-0005-sherpa-x-asr-offline-asr.md` 已记录：离线/在线模型不通用、`bpe_vocab` 必须留空、路径探测抽公共实现。
- `docs/architecture/index.md:233` 写明「本地模型目录不存在/文件不全 → 对应引擎 Init 返回 false，旧引擎在热切换时保留」。**该表述对「文件缺失」成立，对「文件类型错配」不成立**（后者是 `_Exit`）。这是本次调研发现的文档与实现语义差（见 §7.1 待确认项）。

---

## 3. 横向案例

选择标准：必须能读到**一手源码或官方规范**，且机制有真正区分度。

| 案例 | 分发模型 | 身份载体 | 完整性校验 | 暂存/提交 | 离线/导入 |
|------|---------|---------|-----------|----------|----------|
| Ollama | 自建 registry（OCI 风格） | manifest + `sha256` blob | pull 后全量 verify，mismatch 删文件 | `-partial` 分片 + manifest 最后写 | `ollama create` 从本地 GGUF/safetensors 导入 |
| HF Hub | git-backed + Xet/LFS | revision(commit) + etag(sha1/sha256) | 有（LFS 摘要、尺寸复核） | 唯一临时文件 + 原子 rename + `.locks/` | `HF_HUB_OFFLINE=1` / `local_files_only` |
| whisper.cpp | 单文件 HTTP 下载 | 文件名 + 内部 magic | **仅 magic** | 直接写目标文件名 | 手动放文件 |
| sherpa-onnx 上游 | GitHub Release tar.bz2 | 文件名含日期 | **无**（依赖 HTTPS/Release） | 无 | 手动解压 |

### 3.1 Ollama：显式 pull + manifest + blob digest

**事实。** 固定 tag `v0.40.17`（2026-11-10）。

- manifest 结构是 OCI 风格：`MediaTypeManifest` / `MediaTypeManifestList`，`Config` + `Layers []Layer`，并有 `blobFilenamePattern = ^sha256-[0-9a-fA-F]{64}$` 的强约束（`manifest/manifest.go:23,33`）。
- blob 路径就是 `models/blobs/sha256-<hex>`，digest 里的 `:` 换成 `-`（`manifest/paths.go:400-412`）。**内容寻址意味着「同一个 blob 不会被写两次」，写入天然幂等。**
- 下载采用**分片 + 断点续传**：每个分片有独立 `-partial-<N>` 状态文件（JSON，记录 Offset/Size/Completed），并在 `downloadStallTimeout = 30s` 无进展时判定 stalled 后重试（`server/download.go:106,223`）。
- pull 的最后一步是**全量校验**：`fn(api.ProgressResponse{Status: "verifying sha256 digest"})`，然后逐 layer `verifyBlob`；`errDigestMismatch` 时**删除该 blob 并移除 GGUF metadata**，直接返回错误（`server/images.go:1295-1312`）。校验有优化：只有「全部命中缓存」的 digest 才跳过 verify（`skipVerify[layer.Digest] = existing && cacheHit`）。
- **离线导入**是显式路径：`ollama create` 读 `Modelfile` 的 `FROM <dir|file.gguf|model-*.gguf>`，明确说明「Ollama does not quantize GGUF models during import」（官方文档 `docs.ollama.com/import`）。
- pull 是**显式动作**（`POST /api/pull`，`model` 必填），且默认 `stream: true` 返回 `status/digest/total/completed` 进度事件（官方 OpenAPI 描述）。

**对本案的启示（Recommendation，非「照搬」）**：Ollama 的强项不是「有哈希」，而是**哈希同时充当文件名与身份**——因此「校验」与「寻址」是同一件事，不需要额外的 manifest 校验逻辑。本案没有自建 registry，套用内容寻址的收益有限；但 **「pull 完成后统一 verify，mismatch 就删掉并要求重下」** 这条策略成本极低、语义清晰，是可以直接借鉴的最小片段。

### 3.2 Hugging Face Hub：revision / snapshot / 缓存 / 离线

**事实。** `huggingface_hub` v2.4.0（2026-10-29）+ 官方缓存文档。

- 目录骨架 `refs/ blobs/ snapshots/ trees/ .no_exist/`：`refs/main` 存 commit hash；`blobs/<etag>` 存内容（git 文件用 SHA-1，LFS 文件用 SHA-256）；`snapshots/<commit>/<filename>` 是指向 blob 的符号链接（官方文档 `hub/local-cache`）。
- 因此**「同一文件在两个 revision 之间只存一份」**，且**「切换 revision 不复制文件」**。
- 下载写入是**进程唯一临时文件 + 原子换入**：

```python
# file_download.py:741（v2.4.0）
tmp_path = incomplete_path.with_name(f"{incomplete_path.stem}.{uuid.uuid4().hex[:8]}.incomplete")
```

官方注释直接说明了动机：共享的 `<etag>.incomplete` 在锁不被遵守时（Lustre/GPFS/某些 NFS 上 `flock(2)` 静默成功）会互相追加写坏缓存；改成每进程唯一文件后，**锁失效的代价只是重复下载带宽，而不是缓存损坏**（PR #4228 引用）。

- 并发锁目录：`locks_dir = os.path.join(cache_dir, ".locks")`（`file_download.py:1092`）。
- revision 可以是 commit hash（`REGEX_COMMIT_HASH`，40 hex），此时「已存在即直接返回，跳过网络」（`file_download.py:81` 附近逻辑）。
- **离线是显式开关**：`HF_HUB_OFFLINE=1` 时「不做任何 HTTP 调用，只读缓存，缓存缺失即报错」；且官方明确指出「即使最新版本已缓存，`hf_hub_download` 仍会发一次 HTTP 检查新版本；`HF_HUB_OFFLINE=1` 会跳过这次调用从而加快加载」。

**对本案的启示（Recommendation）**：本案是「单机、单用户、模型由用户自己放」，不需要 revision 图。但两条**与规模无关**的机制值得借鉴：
1. **唯一临时文件 + 原子 rename**（`rename(2)` 同文件系统内是原子的）——仓库里 `PrepareSherpaHotwordsFile` 已经这么做了（`sherpa_paths.cpp:390-411`，写 `.tmp` 再 `fsys::rename`），但下载脚本没有。
2. **「离线」是一个显式、可测的状态**，而不是「碰巧没联网」。本案的对应物是「模型目录完整且通过校验」这一布尔量，应能被查询与展示。

### 3.3 whisper.cpp：单文件、magic 校验、存在即跳过

**事实。** tag `v1.9.6`（2026-11-23）。

- 下载脚本 `models/download-ggml-model.sh`：模型清单硬编码在脚本里的 `models="..."` 多行字符串；下载源 `https://huggingface.co/ggerganov/whisper.cpp` + `resolve/main/ggml-<name>.bin`；**若目标文件已存在则 `exit 0` 跳过**；用 `curl --retry 5 --retry-delay 5 --retry-all-errors --retry-connrefused` 重试。**没有摘要校验。**
- 加载侧的完整性检查是**格式自校验**：

```cpp
// src/whisper.cpp:1504-1509（v1.9.6）
// verify magic
{
    uint32_t magic;
    read_safe(loader, magic);
    if (magic != GGML_FILE_MAGIC) {
        WHISPER_LOG_ERROR("%s: invalid model data (bad magic)\n", __func__);
        return false;
    }
}
```

注意 `return false` —— **失败返回，不退出进程**。这与 sherpa-onnx 的 `_Exit` 形成鲜明对比，是「加载边界」设计上的实质差异。

**对本案的启示（Fact 的可迁移性）**：whisper.cpp 证明了「轻量格式自校验 + 失败返回」足以支撑一个广泛使用的本地 ASR 工具，**不需要**内容寻址或签名。但它也**不解决**「这个文件是不是我要的那个模型版本」——这正是本案 C1/C2 的痛点所在，所以不能只学 whisper.cpp 的轻量路线。

### 3.4 Wyoming / faster-whisper：后端隔离与「按语言选后端」

**事实。** `rhasspy/wyoming-faster-whisper`（master `1c40710`，2026-11-19）。

- 它是一个**独立进程的 TCP 服务**，通过 Wyoming 协议与 Home Assistant 通信（`manifest.json` 中 `integration_type: service`、`iot_class: local_push`、`zeroconf: _wyoming._tcp.local.`）。**进程隔离是它的默认形态**，不是可选项。
- 模型选择是**显式参数 + 自动解析**：`--model tiny-int8` 或 HuggingFace 仓库名（如 `Systran/faster-distil-whisper-small.en`）；`--data-dir` 指定下载位置（「Models are downloaded to the first `--data-dir` directory」，README）。
- **离线语义显式**：存在 `--local-files-only`。源码注释解释了策略：

> 「fast, which boot-loops the container. So try the cache first and only reach for the network when a file is genuinely missing. `--local-files-only` stays strict: no fallback, so a model that isn't cached is an error instead of a surprise download.」

即：**默认「先缓存，缺了才联网」，严格模式「缺了就报错」**。这是一个非常清晰的二分，比「永远透明下载」或「永远手动」都更可预测。

- **后端隔离**用 `importlib.util.find_spec` 探测而不 import，官方注释给出真实理由：某些后端在 import 时就加载 native 库，**在特定 CPU 上直接 SIGILL**（树莓派 4 的 Cortex-A72 上 torch 的 LSE atomics）。这是「加载即可能致命」的另一个独立证据，且其解法是**避免加载**而非子进程。

**对本案的启示（Recommendation）**：Wyoming 的「strict 离线开关 + 缺文件才联网」二分，正好对应本案需要的「受控透明下载」语义（§6.1）。而它的「进程隔离是默认形态」是**架构前提不同**（它是常驻服务，本案是插件），不能直接类比。

### 3.5 Flatpak / OSTree：原子提交与回滚（仅取语义，不取机制）

**事实。**

- OSTree：一个 deployment 由 `/ostree/repo` 中一个 commit（SHA256）指定；部署目录 `/ostree/deploy/$stateroot/deploy/$checksum`；「OSTree is designed to boot directly into exactly one deployment at a time」；支持 **staged deployments**（官方 `Deployments` 文档）。
- Flatpak：构建产物「committed to a branch」，安装时「pulled from the remote into a new branch in a local repository」，再用**硬链接**映射到文件系统；「Versioning also makes rollbacks possible」（官方 `Under the Hood`）。

**对本案的启示（Recommendation，限定范围）**：不要引入 OSTree。可迁移的只有两条**语义**：
1. **「新版本完整就位后才切换指针」**——本案对应「模型目录就绪 → 原子写一个 `current` 指针/状态文件 → 引擎读取指针」，这样切换是 O(1) 且可回滚。
2. **「同一时刻只有一个 active deployment」**——本案对应「两个 sherpa 后端不应同时指向同一目录的不同期望」（C1 的根因之一）。

---

## 4. 逐维度对比

### 4.1 模型身份/版本/后端绑定

| 系统 | 身份如何表达 | 是否绑定后端/模型类型 |
|------|-------------|---------------------|
| Ollama | manifest 内 `layers[].digest` + `Config`；model name → manifest 路径 | 有 `Runner`（`mlx`/`ggml`/`llamacpp`）与 `Format`（`safetensors`/`gguf`）字段，且 `requiresMLX()` 会按 `Runner` 或 tensor layer 判定 |
| HF Hub | `repo_id` + `revision`(commit) + `filename` | 否（HF 不关心你用什么后端加载） |
| whisper.cpp | 文件名（`ggml-<model>.bin`） | 轻量：magic 只证明「是 ggml 文件」 |
| sherpa-onnx 上游 | 文件名含日期（`...-2026-06-03`）+ tar 包名 | 包名里 `streaming` / 无 `streaming` 区分在线/离线，但**文件名不进入运行时校验** |
| **本案** | **目录位置 + 文件名前缀** | **无** |

**观察**：本案处在「最弱」一档。上游 sherpa-onnx 至少把「是否 streaming」编码进**包名**，而本案的解析器把这个信息丢掉了。

**Recommendation（MVP）**：在模型目录写一个 `model.json`（或 `manifest.json`），至少包含：

```json
{
  "schema": 1,
  "name": "sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03",
  "kind": "offline-transducer",
  "files": {"encoder": "...", "decoder": "...", "joiner": "...", "tokens": "tokens.txt"},
  "sha256": {"encoder": "...", "decoder": "...", "joiner": "...", "tokens": "..."},
  "source": "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/<asset>.tar.bz2",
  "source_sha256": "7592d1a1..."
}
```

这样「身份」从「猜目录」变成「读声明」，并且 `kind` 让两个后端可以**拒绝**不属于自己的模型（把 C2 的 `_Exit` 变成 `Init` 返回 false）。

### 4.2 文件完整性 vs 来源真实性

必须把三件事分开：

1. **完整性（integrity）**：文件没坏、没被截断。哈希/尺寸足够。
2. **来源真实性（authenticity）**：文件确实由 k2-fsa 发布。只有签名（或可信传输 + 可信元数据）能证明。
3. **适用性（suitability）**：这个文件确实能被我的后端加载。**哈希完全证明不了这一点。**

| 手段 | 能证明 | 不能证明 | 本案成本 |
|------|-------|---------|---------|
| 文件存在 + 前缀 | 几乎没有 | 完整性、真实性、适用性 | 已完成 |
| 记录并校验 `sha256`（来自 GitHub Release `digest`） | 完整性；**若 digest 来自可信 API 且与下载分离**，也提供较弱真实性 | 适用性 | 低（脚本 + 一个 JSON 字段） |
| ONNX metadata 读取（`model_type`、`vocab_size` 等） | **适用性**（这是唯一能防 C2 的手段） | 完整性的强保证（需要解析模型） | 中（见 §6.3，**不能在进程内做**） |
| GPG/签名 | 真实性 | 适用性 | 高（上游未提供签名资产，**Unknown**） |

**关键事实核查**：本次实测 `GET /repos/k2-fsa/sherpa-onnx/releases/tags/asr-models` 返回的 asset 对象**确实带 `digest` 字段**（GitHub 官方文档 `Get a release asset` 的响应示例也含 `"digest": "sha256:..."`）。所以「记录官方摘要」不是空想，是**当下就能落地**的。

> **但要注意**：如果脚本「从同一个 HTTPS 响应里同时取 digest 和下载 URL」，那么同时篡改两者的攻击者仍可绕过。真正的真实性需要**把 digest 固化在仓库里**（随包分发的 `models.json`），或使用签名。这一点必须说清楚，不能假装哈希等于真实性。

**Unknown**：k2-fsa 是否为 release asset 提供独立签名（如 `.sig`/`.asc`）——本次未在 release asset 列表中发现签名文件；未逐一核对 501 个 asset，按「未发现」记录，不作断言。

### 4.3 显式下载 / 用户体验 / 离线导入

三种形态的取舍（Recommendation，附本案约束）：

| 形态 | 优点 | 缺点 | 本案适配 |
|------|------|------|---------|
| **随包一次性工具**（脚本/CLI） | 零网络代码进 addon；可审计；可离线分发脚本 | 用户需开终端；无进度 UI；错误信息在终端 | **最契合现状**（已有 `download-model.sh`），应加固而非替换 |
| **GUI（fcitx5-configtool 内）** | 无需终端；可展示进度/校验结果 | 需要在 addon 里写 HTTP + 解压 + 线程；主线程阻塞风险；Fcitx5 配置对话框生命周期不适合长任务 | 中期可做「只读状态展示 + 打开说明」，**不建议**在配置对话框里做大文件下载 |
| **受控透明下载**（首次使用时自动拉） | 体验最顺 | 隐私/带宽意外；无网即失败；需处理并发与半成品；**违背「用户知情」** | 仅在「用户已显式选择本地后端 + 已确认体积」时可接受；否则不建议 |

**事实支撑**：Wyoming 的 `--local-files-only` 注释明确指出「surprise download」是负面体验，因此它把「默认先缓存」与「严格离线」分成两档。本案可以直接采用同样的二分。

**另一个事实**：本案的默认在线模型 tar 包约 **570 MiB**（实测 `size=597755927`），离线 X-ASR int8 约 **130 MiB**（`size=136902463`）。**体积差异本身就应该影响默认值**——默认应偏向小的那个，或至少让用户在选择时看到体积。

### 4.4 暂存与提交：并发与断电语义

| 系统 | 中断后目录状态 | 并发安全 |
|------|---------------|---------|
| Ollama | `blobs/sha256-xxx-partial-N`（JSON 状态文件）+ 未写完的 blob；manifest 未写 → 该模型不可用，但**其他模型不受影响** | 分片状态文件 + 下载管理器按 digest 去重 |
| HF Hub | 唯一 `.incomplete` 文件残留（可清理）；blob 未 rename → snapshot 不指向它 | `.locks/` 目录 + 进程唯一临时文件名（锁失效也不会写坏） |
| whisper.cpp | 直接写目标文件名 → **可能是半截文件** | 无（脚本假设单实例） |
| **本案 `download-model.sh`** | **先删旧文件再 `cp`** → 目标目录可能「半个旧模型 + 半个新模型」 | 无 |

**Recommendation（MVP，低成本）**：

1. 解压到 `models/.staging/<name>-<pid>-<rand>/`；
2. 校验 `sha256`；
3. 写 `manifest.json`（含 `kind`）；
4. `rename(staging, models/<name>)`（同文件系统内原子）；
5. 最后才更新「当前模型」指针文件。

这与 HF 的「唯一临时文件 + 原子换入」同构，也与 OSTree 的「commit 完整后才切换」同构。**注意**：`rename(2)` 对**目录**在目标已存在时要求目标为空目录，因此实现上是「先 rename 到新名字，再删旧目录」，不是「覆盖」。这点必须写进验收标准，否则会在实现时踩坑。

### 4.5 加载与推理解耦、进程隔离成本

先把三件被混在一起的事拆开：

| 概念 | 含义 | 本案是否需要 |
|------|------|-------------|
| **A. 试加载（validate by construction）** | 真正构造一次 Recognizer，确认能加载 | **需要**，但要放到子进程（理由见下） |
| **B. 加载与推理生命周期解耦** | 引擎对象可长期持有，会话按需创建 | **已实现**（`holder_` + `AsrSession`） |
| **C. 进程外推理** | 推理在另一个进程，主进程只搬数据 | **不一定需要**，成本高（见下） |

**A 为什么需要子进程？** 因为进程内试加载失败可能是 `_Exit(-1)`（§2.2）。这不是理论风险：`SHERPA_ONNX_READ_META_DATA` 缺失即退出，而 metadata 是否齐全正是「模型类型错配」的典型表现。

**A 的子进程成本（可量化）**：
- 一次 fork/exec + dlopen + 构造三个 `Ort::Session`（encoder/decoder/joiner）→ 读 metadata → 退出。耗时与模型大小线性相关，137MB int8 量级下是**数百毫秒到数秒**（**Unknown**：本次未实测，不应编造数字；应作为验收项测量）。
- 收益：把「Fcitx5 进程被杀」变成「退出码非零 + stderr」，可以安全地在配置保存时提示用户。
- 代价：需要一个小型可执行文件（或复用 addon 的 `--self-check` 模式），并处理「子进程被 OOM killer 杀」与「子进程卡住」（需要超时 + SIGKILL）。

**C 为什么不建议现在做？**
- 需要跨进程搬 PCM（每段 1–10s，16kHz int16 ≈ 32–320 KB）与结果，**带宽不是问题**，问题是**语义**：`generation` 过滤、`utteranceId` 保序、`Cancel`/`JoinWithTimeout`、`SessionReaper` 全部要在 IPC 上重做（`pipeline.cpp`、`result_coordinator.cpp`、`session_reaper.cpp`）。
- 进程外推理的**唯一强收益**是「推理崩溃不杀主进程」。但目前已知的崩溃源（模型错配）**可以在加载阶段拦截**（手段 A）。一旦 A 到位，C 的边际收益大幅下降。
- 因此：**先做 A，把 C 列为观察项**，条件触发（若出现 ONNX Runtime 在推理期崩溃的真实案例）。

### 4.6 隐私与无云回退

**事实**：本地后端 `provider = "cpu"`（`sherpa_onnx_asr.cpp:268`；`sherpa_offline_asr.cpp:249`），模型完全本地，音频不出机。云后端在 `engine.cpp` 的 `CreateAsrEngine` 里按 `activeBackend` 选择，**没有自动降级**。

**Recommendation**：保持「无云回退 = 用户显式选本地后端」。理由：
- 自动降级会让「我以为在用云、其实在用本地（或反之）」成为可能，对隐私预期是负面。
- 仓库已有「Init 失败保留旧引擎」的语义（C0/§2.3），这已经提供了「切坏了不会更糟」的保障。
- 若要改善离线体验，正确做法是**在 UI 上明确显示「本地」与「云端」**，而不是静默切换。

---

## 5. 针对本案的具体判断

### 5.1 「两个后端共用 ResolveSherpaModelPaths 导致串用」——是否成立？

**成立，但需要精确表述。** 三种情形：

1. **两个 `ModelDir` 都为空** → 两者解析到同一目录。若该目录里只有一套模型，且用户从 `sherpa_onnx` 切到 `sherpa_offline`（或反之），**必然错配**。
   - 实测本机：`~/.local/share/fcitx5/voice-input/models/zipformer/` 存在（在线模型），**没有 x-asr 目录**。因此在本机从 `sherpa_onnx` 切到 `sherpa_offline` 会命中在线模型 → 触发 §2.2 的 int 张量校验 `_Exit`（方向 a）。**本次未实际执行该切换**（会造成 Fcitx5 退出），属推断。
2. **两个 `ModelDir` 都指向同一路径** → 同上（配置允许，无校验）。
3. **两个 `ModelDir` 不同** → 不串用，但**仍无 kind 校验**：用户把离线模型放进 `sherpa_onnx` 的目录，照样走到 SIGSEGV 或 `_Exit`。

**重要限定**：`ResolveSherpaModelPaths` 本身不是 bug——它是「按约定找文件」的合理实现。问题是**它被当作了「验证」**。缺的是 §4.1 的 kind 声明与 §4.5 的 A 阶段试加载。

### 5.2 安装路径的 XDG 一致性

**事实**：
- XDG spec §3：`$XDG_DATA_HOME` 缺省 `$HOME/.local/share`；`$XDG_DATA_DIRS` 定义「preference ordered base directories」用于**搜索**数据文件，缺省 `/usr/local/share/:/usr/share/`。
- 本案读了 `XDG_DATA_HOME`（`sherpa_paths.cpp:88-95`），**没读 `XDG_DATA_DIRS`**。
- 本案硬编码 `/usr/local/share/fcitx5/voice-input/models`（`sherpa_paths.cpp:126`），而打包路径是 `${CMAKE_INSTALL_FULL_DATADIR}`（`CMakeLists.txt:104`）。

**后果**：若发行版把模型装到 `/usr/share/fcitx5/voice-input/models`，**不会被扫描**。目前这个问题被「模型不随包分发」掩盖了；一旦将来做「发行版可选模型包」，就会立刻暴露。

**Recommendation（延后项）**：改用 fcitx5 自己的 `StandardPaths`（`engine.cpp:48-51` 已经在用 `StandardPaths::global().userDirectory(StandardPathsType::Config)`），或读 `XDG_DATA_DIRS` 并逐项拼接。**不建议**自造一套路径逻辑。注意 `XDG_DATA_DIRS` 里可能出现相对路径，spec 要求「视为非法并忽略」。

### 5.3 「engine 同步加载 + 保留旧 ASR」的取舍

**事实**：当前从激活或保存配置的主线程路径同步初始化模型，返回前该线程无法处理事件；阻塞时长未实测。`Pipeline::Start()` 要求引擎已就绪（`pipeline.cpp:146`）。

**Recommendation**：后台线程准备引擎，主线程仅接收结果、检查配置代次并安装当前引擎；首次准备好前不启动录音。先测基线，但不靠某个机型的耗时决定是否允许阻塞 Fcitx。仅同一本地后端换模型失败时可保留上一个已验证版本，并显示实际生效的版本。云端切到本地时立即停止接收新的云端会话并取消在途会话，不因本地加载失败恢复云端 ASR；不能撤回已发送的数据。云端 LLM 后处理需独立展示和控制。

### 5.4 「随包一次性工具 vs GUI vs 受控透明下载」

结论（Recommendation）：

1. **随包一次性工具**负责安装、状态和离线导入；安装语义与界面分离，不要求终端用户先克隆源码，也不引入常驻进程。继续只改仓库脚本成本最低，但解决不了安装包用户的入口问题。
2. **Fcitx 首版只报状态并引导安装**，不在配置对话框里下载几百 MiB。GUI 下载可复用安装工具，但不是安全加载的前提。
3. **选择本地后端不等于授权下载**。展示来源与体积，用户显式确认后才联网。

### 5.5 是否真需要子进程试加载 / 进程外推理

| 需求 | 判断 | 依据 |
|------|------|------|
| 子进程**试加载** | **首版纳入安装验收** | 进程内试加载可能 `_Exit`（§2.2）；子进程把致命错误转成可诊断的失败。它不是运行时沙箱 |
| 进程**外推理** | **暂不需要** | 已知崩溃源可在加载期拦截；IPC 要重做 generation/保序/超时，成本远高于收益 |
| 进程内**异步加载** | **需要（与试加载独立）** | 主线程阻塞是当前真实问题（§5.3） |

---

## 6. 建议：MVP / 延后 / 明确不做

### 6.1 MVP（项目建议，不是行业标准）

| ID | 内容 | 验收标准 |
|----|------|---------|
| M1 | 随包清单固定 ID、版本、后端、文件角色、归档与逐文件 SHA-256，不信任用户目录自报的 `kind` | 修改目录名或本地清单不能把离线模型变成流式模型 |
| M2 | 两后端按清单解析明确的版本路径；在后台校验，失败关闭本地识别，不回退扫描 | 两套共存永不串用；文件损坏时在调用 Sherpa 前返回错误 |
| M3 | 旧目录不删除；如提供显式导入，只有内容匹配固定清单才接收，并遵循同一安装流程 | 未登记或错配的旧目录不会被悄悄加载 |
| M4 | 安装工具用同文件系统暂存、受限解包、校验、限时子进程试加载，最后原子发布不可变版本目录；同版本并发安装需串行化或幂等收敛 | 中断、空间不足、校验失败或试加载崩溃均不发布半套目录；旧版本保留 |
| M5 | 支持从用户提供的同一份官方归档离线安装 | 断网仍可安装，校验强度与下载相同 |
| M6 | 后台准备、代次过滤、主线程提交；不保留旧云端引擎 | 慢加载不阻塞主线程，过期完成不生效，切到本地失败后不发起新的云端 ASR |
| M7 | 记录加载计时和失败类别，更新用户配置与迁移文档 | 可区分未安装、损坏、不兼容，不输出转写或凭据 |

### 6.2 延后项（有明确触发条件）

| ID | 内容 | 触发条件 |
|----|------|---------|
| L2 | XDG `XDG_DATA_DIRS` 支持 / 改用 `StandardPaths` | 发行版开始提供 sherpa 模型包时 |
| L4 | 多模型并存 + UI 切换（不靠 `ModelDir` 手填） | 用户明确要求，或 manifest 机制已稳定 |
| L5 | 模型签名校验 | 上游提供签名资产（**当前 Unknown**） |
| L6 | 进程外推理 | 出现「加载期校验通过、推理期崩溃」的真实案例 |

### 6.3 明确不做

- **不做透明自动下载**（未经用户确认的几百 MB 流量）。
- **不做 OSTree/内容寻址存储**（单机单用户，收益不匹配复杂度）。
- **不把哈希当真实性**（哈希只证明完整性；真实性需要签名或可信固化的摘要）。
- **不在配置对话框里做大文件下载**（生命周期与线程模型都不合适）。
- **不为了「行业惯例」引入子进程推理**。

---

## 7. 待确认与未验证项

| # | 项 | 状态 | 说明 |
|---|----|------|------|
| U1 | 在线模型喂给 `OfflineRecognizer` 时，实际先命中哪条 `_Exit` | **部分推断** | `InitEncoder` 的 int32/int64 校验（input[1]/output[1]）在 metadata 读取之前执行，因此对 float32 状态的在线模型**应当**先命中它；但未实测（未下载模型）。若某在线模型的 input[1] 恰为 int 张量，则可能改命中 `vocab_size` 缺失 |
| U2 | 离线模型喂给 `OnlineRecognizer` 时的具体失败点 | **部分推断** | 已确认 `GetModelType()` 缺 `model_type` 返回 `kUnknown` → `Create()` 返回 `nullptr` → `model_->SetFeatureDim()` 无检查（SIGSEGV）。但 X-ASR 离线包的 encoder 是否带 `model_type`（若带 `zipformer2` 则改走 `encoder_dims` 缺失的 `_Exit` 路径）**未实测**。两条路径都致命 |
| U3 | 137 MB int8 模型的子进程试加载耗时 | **未实测** | 作为 M4 的验收测量项 |
| U4 | k2-fsa 是否提供 release asset 签名 | **未确认** | 未在 asset 列表中看到签名文件，但未逐一核对 501 个 asset |
| U5 | `directory_iterator` 命中顺序对多模型目录的影响 | **已知风险，未构造用例** | 标准规定顺序 unspecified；多模型共存时「命中哪个」不确定 |
| U6 | 本机 `/usr/share/fcitx5/voice-input/models` 是否有 sherpa 模型 | **已核对：无**（只有 `silero_vad.onnx`） | 说明当前不存在「系统级 sherpa 模型」路径，但不代表其他发行版没有 |
| U7 | ADR-0005 提到的「submodule 曾含未推送补丁」是否影响本文结论 | **不影响** | 已回退到上游 `34eba5a`，本文所有 sherpa 引用均基于该 commit |
| U8 | Ollama manifest 的 `MediaTypeManifestList` 是否已实际启用 | **未验证** | 代码中存在该常量与处理逻辑，但未验证线上是否有 manifest list 模型；不影响本文结论 |

---

## 8. 引用

### 8.1 仓库内（本次基线 `1708687`）

| 引用 | 位置 |
|------|------|
| 模型路径解析 | `src/addon/asr/utils/sherpa_paths.cpp:99-149`；头注释 `sherpa_paths.h:24-38` |
| 在线后端加载 | `src/addon/asr/sherpa_onnx_asr.cpp:246-313` |
| 离线后端加载 | `src/addon/asr/sherpa_offline_asr.cpp:225-311` |
| 引擎工厂 / 热切换 | `src/addon/engine.cpp:335-410`（`CreateAsrEngine`）、`:447-455`（`ReloadActiveAsrClient`） |
| 引擎替换语义 | `src/addon/pipeline/pipeline.cpp:104-127` |
| 配置项 | `src/addon/config/voiceinput-config.h:185-221` |
| 下载脚本 | `scripts/download-model.sh:12-56` |
| 安装路径常量 | `CMakeLists.txt:104`、`:343-359` |
| 路径测试 | `tests/sherpa_paths_test.cpp` |
| ADR | `docs/architecture/adr/ADR-0005-sherpa-x-asr-offline-asr.md` |
| 架构文档的错误处理表 | `docs/architecture/index.md:233` |

### 8.2 上游 sherpa-onnx（commit `34eba5a27220026b5981b633981c53205515067d`）

- `SHERPA_ONNX_EXIT` / metadata 宏：https://github.com/k2-fsa/sherpa-onnx/blob/34eba5a27220026b5981b633981c53205515067d/sherpa-onnx/csrc/macros.h#L54-L92
- 离线 transducer 初始化与 `bpe_vocab`：https://github.com/k2-fsa/sherpa-onnx/blob/34eba5a27220026b5981b633981c53205515067d/sherpa-onnx/csrc/offline-recognizer-transducer-impl.h#L104-L119
- 离线 transducer 必需 metadata：https://github.com/k2-fsa/sherpa-onnx/blob/34eba5a27220026b5981b633981c53205515067d/sherpa-onnx/csrc/offline-transducer-model.cc#L303
- 在线 model_type 探测与未知类型退出：https://github.com/k2-fsa/sherpa-onnx/blob/34eba5a27220026b5981b633981c53205515067d/sherpa-onnx/csrc/online-transducer-model.cc#L74 、#L182
- 在线 recognizer 分发（「Please specify a model」后 EXIT）：https://github.com/k2-fsa/sherpa-onnx/blob/34eba5a27220026b5981b633981c53205515067d/sherpa-onnx/csrc/online-recognizer-impl.cc#L143
- C API Validate 边界：https://github.com/k2-fsa/sherpa-onnx/blob/34eba5a27220026b5981b633981c53205515067d/sherpa-onnx/csrc/c-api.cc#L208-L224
- 配置校验：`online-model-config.cc#L55`、`online-transducer-model-config.cc#L49`、`offline-recognizer.cc#L72,L99`
- simple-sentencepiece `LoadVocab` 的 `exit(-1)`：https://github.com/pkufool/simple-sentencepiece/blob/v0.7/ssentencepiece/csrc/ssentencepiece.cc#L248-L262

### 8.3 外部案例

- Ollama manifest：https://github.com/ollama/ollama/blob/v0.40.17/manifest/manifest.go#L23-L33
- Ollama blob 路径：https://github.com/ollama/ollama/blob/v0.40.17/manifest/paths.go#L400-L412
- Ollama 分片续传 / stall 超时：https://github.com/ollama/ollama/blob/v0.40.17/server/download.go#L106 、#L223
- Ollama pull 后校验与 mismatch 删除：https://github.com/ollama/ollama/blob/v0.40.17/server/images.go#L1295-L1312
- Ollama 导入文档：https://docs.ollama.com/import
- Ollama pull API（含 `digest/total/completed`）：https://docs.ollama.com/api/pull
- HF 缓存结构：https://huggingface.co/docs/hub/local-cache
- HF 缓存管理（refs/blobs/snapshots）：https://huggingface.co/docs/huggingface_hub/guides/manage-cache
- HF 唯一临时文件 + 原子移动：https://github.com/huggingface/huggingface_hub/blob/v2.4.0/src/huggingface_hub/file_download.py#L1969 （及 `#L741`、`#L1092`）
- HF 离线环境变量：https://huggingface.co/docs/huggingface_hub/package_reference/environment_variables
- whisper.cpp 下载脚本：https://github.com/ggml-org/whisper.cpp/blob/v1.9.6/models/download-ggml-model.sh
- whisper.cpp magic 校验：https://github.com/ggml-org/whisper.cpp/blob/v1.9.6/src/whisper.cpp#L1504-L1509
- Wyoming faster-whisper README（`--local-files-only`、`--data-dir`）：https://github.com/rhasspy/wyoming-faster-whisper/blob/1c407106530e09fc8f2477e4ae4cf86480fd6256/README.md
- Wyoming 模型加载与 `find_spec` 理由：https://github.com/rhasspy/wyoming-faster-whisper/blob/1c407106530e09fc8f2477e4ae4cf86480fd6256/wyoming_faster_whisper/models.py
- Home Assistant Wyoming 集成 manifest：`homeassistant/components/wyoming/manifest.json`（`integration_type: service`）
- XDG Base Directory Specification（v0.8，2021-05-08）：https://specifications.freedesktop.org/basedir-spec/latest/
- OSTree Deployments / staged deployments：https://ostreedev.github.io/ostree/deployment/
- Flatpak Under the Hood（OSTree、hardlink、rollback）：https://docs.flatpak.org/en/latest/under-the-hood.html
- GitHub Release asset `digest` 字段：https://docs.github.com/en/rest/releases/assets
- sherpa-onnx X-ASR 官方模型页（文件清单、体积、`test_wavs`）：https://k2-fsa.github.io/sherpa/onnx/x-asr/index.html
- sherpa-onnx 预训练模型总表：https://k2-fsa.github.io/sherpa/onnx/pretrained_models/index.html

### 8.4 本次实测的 release 元数据（2026-11-24）

`GET https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/tags/asr-models` → `assets: 501`，其中：

| asset | size | digest |
|-------|------|--------|
| `sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03.tar.bz2` | 136902463 | `sha256:7592d1a1e9bc1e034c39fbaab9b0db8af46429d95d64c6e2a69593934af10e9c` |
| `sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30.tar.bz2` | 597755927 | `sha256:30437d84dc4861740d40166212ab05f3728945443d53691620075c4031fb88e1` |

> 说明：仅读取 API 元数据，**未下载**上述 tar 包。`digest` 字段的可用性即本报告 §4.2 建议的前提。

### 8.5 实施阶段的逐文件摘要来源

安装后的模型文件需要独立校验，不能信任用户数据目录内自行声明的摘要。
模型作者在 Hugging Face 的固定 revision 提供各 ONNX 文件的 Git LFS SHA-256：

- 流式模型：[revision `18f31ce4644ac3bcf544eea692d7242b798d508f`](https://huggingface.co/api/models/csukuangfj/sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30/tree/18f31ce4644ac3bcf544eea692d7242b798d508f?recursive=true)。
- 离线模型：[revision `bbc3ebbf8f134848f804967f3a3b69f981308a85`](https://huggingface.co/api/models/csukuangfj2/sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03/tree/bbc3ebbf8f134848f804967f3a3b69f981308a85?recursive=true)。

其中 `tokens.txt` 是普通 Git 文件而非 LFS 对象，需下载对应 revision 的文本后计算
SHA-256；不能把 Git blob 的 SHA-1 当作 SHA-256。

**来源差异（实施时实测）：**离线 GitHub release 压缩包与上述 Hugging Face revision
的 encoder、decoder 并非同一份导出，摘要不同；joiner 与 tokens 的摘要相同。离线
清单必须以校验过压缩包摘要后实际解包得到的文件摘要为准，不能直接采用 HF 摘要。
在线模型的 release 压缩包已实际下载并核对 SHA-256、大小和解压完整性；四个必要
文件的摘要与上述 HF revision 及本机已有模型均一致。

---

## 9. 一句话总结

本案缺的不是「下载能力」，而是**模型身份**与**失败边界**：`ResolveSherpaModelPaths` 用文件名前缀代替了身份，而 sherpa-onnx 用 `_Exit(-1)` 代替了失败返回——两者叠加，使「放错模型」从「识别不准」升级为「输入法进程消失」。最小可行修复是 manifest + kind 校验 + 事务性安装；子进程试加载与进程外推理应分开决策，后者暂无必要。
