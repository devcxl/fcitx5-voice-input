# src/model_manager —— 受控模型安装核心

本地 ASR 模型的受控安装：固定版本、逐文件摘要、白名单解包、子进程试加载、原子发布。
设计依据见 [`docs/development/research/sherpa-model-install-and-loading.md`](../../docs/development/research/sherpa-model-install-and-loading.md)
的 M1–M5。

---

## 1. 对外契约（addon 只读复用）

### 1.1 清单：`model_catalog.h`

纯数据 + `const` 查询，无网络/写盘依赖。这是**唯一**的模型身份来源。

```cpp
#include <model_manager/model_catalog.h>
using namespace fcitx::model_manager;

const ModelSpec* spec      = FindModelSpec("offline-x-asr");
const ModelFileSpec* tokens = FindModelFileByRole(*spec, "tokens");
const ModelFileSpec* f      = FindModelFile(*spec, "tokens.txt");
```

| 类型 | 字段 |
|------|------|
| `ModelFileSpec` | `role`、`relativePath`、`sha256Hex`、`size` |
| `ModelSpec` | `id`、`version`、`backendId`、`streaming`、`archiveUrl`、`archiveSha256Hex`、`archiveSize`、`files`、`fileCount` |

固定的两个模型：

| id | version | streaming | 文件角色 → 文件名 |
|----|---------|-----------|-------------------|
| `online-zipformer` | `sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30` | true | `encoder.int8.onnx` / `decoder.onnx` / `joiner.int8.onnx` / `tokens.txt` |
| `offline-x-asr` | `sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03` | false | `encoder-epoch-99-avg-1.int8.onnx` / `decoder-epoch-99-avg-1.onnx` / `joiner-epoch-99-avg-1.int8.onnx` / `tokens.txt` |

`bpe.model` / `bpe.vocab` **不在**清单内：ADR-0005 确定 `bpe_vocab` 必须留空
（传 sentencepiece 二进制 proto 会 `exit(-1)`），安装器也不解包它们。

**信任锚**：安装器只信任「清单里的归档 `sha256` → 解包产物」这条链。

> **offline-x-asr 的摘要来源（重要）**
> `encoder`/`decoder` 的摘要取自**官方 release 归档的实际内容**（归档 `sha256` 与
> GitHub Release API 的 `digest` 一致，`bzip2 -t` 通过，tar/bsdtar 双工具解包一致）。
> 上游 Hugging Face 仓库中这两个文件的摘要与归档**不同**（`joiner`/`tokens` 相同），
> 属另一次导出。若改用 HF 的值，安装会在逐文件校验处永久失败。**不要替换。**

### 1.2 路径：`model_paths.h`

```cpp
std::string DataHomeDir();                                    // XDG_DATA_HOME（拒绝相对路径）或 $HOME/.local/share
std::string DataRootDir();                                    // <data home>/fcitx5/voice-input/models
std::string VersionDir(std::string_view id, std::string_view version);
std::string InstallMarkerPath(const std::string& versionDir);  // <versionDir>/install.json
std::string ModelsParentDir();                                // <data home>/fcitx5/voice-input（信任边界）
```

```
<XDG_DATA_HOME>/fcitx5/voice-input/models/
├── .locks/<id>.lock              # flock 安装锁
├── .staging/<id>-XXXXXX/         # 同文件系统暂存（mkdtemp，成功后删除）
└── <id>/<version>/               # 不可变版本目录（只放置，从不替换）
    ├── install.json              # 不可信 marker
    └── <relativePath...>         # 逐文件摘要由清单固定
```

`<id>` 与 `<version>` 只来自清单常量，不接受用户输入；`VersionDir` 对非法片段返回空串。

### 1.3 marker：`<version>/install.json` —— **不可信**

由安装器写入，含 `"trusted": false`。只能用于**快速否定**（不存在 ⇒ 肯定没装好），
**绝不能**作为校验依据。校验一律把磁盘内容与 `model_catalog.h` 的内置
`sha256Hex`/`size` 逐文件比对（`installed_model.h` 的 `CheckInstalledModel()`）。

---

## 2. 命令行协议

```
voice-input-model-installer --model <online-zipformer|offline-x-asr> [--archive <path>]
                            [--trial-timeout-ms N]
voice-input-model-installer --check-model <id>      # 只读状态查询
voice-input-model-installer --list
voice-input-model-installer --verify-internal <id> <versionDir>   # 内部使用
```

- 不传 `--archive` 时按清单固定 URL 下载；传则离线安装，校验强度相同（也会先复制进
  私有暂存再校验，避免“校验后按路径重开”的 TOCTOU）。
- **stdout 只输出 JSON Lines**，一行一个对象；日志走 stderr。
- 退出码：`0` 成功；`2` 参数错误；`3` 模型未知；`4` 锁占用；`5` 取消；
  `6` 归档摘要不符；`7` 归档不安全；`8` 文件摘要不符；`9` 缺文件；
  `10` 试加载失败；`11` 试加载超时；`12` 下载失败；`13` 解包失败；
  `14` 发布失败；`15` 路径不安全；`16` 内部错误。

### 2.1 事件

```json
{"event":"start","model":"offline-x-asr","version":"..."}
{"event":"download","model":"offline-x-asr","bytesDone":1373454,"bytesTotal":136902463}
{"event":"verify_archive","model":"offline-x-asr","version":"..."}
{"event":"extract","model":"offline-x-asr","file":"tokens.txt"}
{"event":"verify_file","model":"offline-x-asr","file":"tokens.txt"}
{"event":"verify_trial","model":"offline-x-asr"}
{"event":"publish","model":"offline-x-asr","version":"..."}
{"event":"result","model":"offline-x-asr","version":"...","ok":true,"published":true,
 "cancelled":false,"error":"none","detail":"安装完成","elapsedMs":13856,
 "versionDir":"/…/models/offline-x-asr/<version>"}
```

`result` 恰好一条，且必为最后一条。两个附加布尔字段（默认 `false`，旧客户端可忽略）：

- `durabilityUnknown`：已安装但父目录刷盘失败 → 持久性未知。
- `pathStateUncertain`：已安装，但**发布后**按路径回查发现目标已不是刚放入的那个
  目录（父目录被移动/替换）→ 发布事实成立、不回滚，但**不保证该路径上的模型
  当前可用**，需提示用户检查。

两者都表示"安装已发生"，不要误报为未安装，也不要据 `pathStateUncertain` 声称
"已可用"。

### 2.2 只读状态查询：`--check-model <id>`

```json
{"event":"installed_status","model":"offline-x-asr","state":"valid",
 "version":"sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03",
 "archiveSize":136902463,
 "source":"https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03.tar.bz2"}
```

- 单行 JSON，字段固定 `event` / `model` / `state` / `version` / `archiveSize` / `source`。
  `version`、`archiveSize`（整数字节）、`source`（固定 release HTTPS URL）都直接取自
  内置清单，供 GUI 显示“应该是哪个版本、需要下载多少、来自哪里”。
- `state` ∈ `absent` | `valid` | `tampered` | `unreadable`，由内置清单逐文件
  SHA-256 + 字节数判定。
- **只读**：不创建目录、不写文件、不加锁、不联网。哈希在本进程内完成
  （online 模型约 0.5–1 s），GUI 应异步调用。
- 退出码：`0` 查询完成（与 state 无关）；`3` 未知模型（**不输出**状态行，
  避免把未知 id 误报成 `absent`）；`2` 缺少参数；`5` 查询被信号取消
  （不输出状态行）；`16` 无法确定 XDG 目录。

### 2.3 取消语义（SIGINT/SIGTERM/SIGHUP）

- 停止当前阶段、删除本次暂存，**不产生**版本目录；`result` 为
  `"cancelled":true`、`"published":false`，退出码 `5`。
- **原子发布是取消临界点**：发布成功后即使收到取消也按成功报告
  （`ok:true`、`published:true`），不得误报未安装。
- 试加载子进程在取消/超时时被杀掉整组（`SIGKILL`）并回收，不留孤儿。
- stdout 写失败（GUI 关闭管道）视同取消；`SIGPIPE` 已忽略。

---

## 3. 安全边界与信任边界

### 3.0 信任边界（先说清楚）

**同 UID 且可写 `$XDG_DATA_HOME` 或暂存树的进程不在防御范围内。** 本模块用目录 fd、
`O_NOFOLLOW` 与创建时 dev/ino 校验来应对**非对抗**问题（并发安装、目录被搬走、
异常中断后的清理），这些检查是**尽力而为**的一致性校验，不能证明主动恶意同 UID
进程无法越权。

已知残余竞态：

- `renameat2` 是 **fd + 名字**：名字在**已钉住的目录内**解析，因此该目录内的条目
  仍可能在采集与调用之间被替换；
- 暂存目录由 `mkdtemp` 创建后立即用 `O_NOFOLLOW` 打开并**一直持有 fd**；身份比较
  基于该 fd，因此不受 inode 回收复用影响（若只比较历史 dev/ino 值，目录删除后
  文件系统可复用同一 inode，判断会失效）；`mkdtemp` 到 open 之间仍有极短缝隙；
- 递归删除作用于**调用时存在于该目录内**的条目：被搬入本树的目录会被删除
  （硬链接被 unlink 不会销毁外部内容）；
- 发布后的 `pathStateUncertain` 只是**事后快照**，同 UID 可在快照之后继续搬动。



| 风险 | 处理 |
|------|------|
| 归档被替换/损坏 | 校验归档 `sha256` + 精确字节数（清单常量） |
| 归档内路径穿越 | 拒绝含 `..`、`.`、空组件、绝对路径、反斜杠、NUL 的条目 |
| 符号链接/硬链接/设备/FIFO/socket | 版本目录内出现即拒绝整个归档；无关位置的链接跳过不落盘 |
| 重复条目覆盖 | 同一白名单文件出现两次即拒绝 |
| 解压炸弹/超限 | 每个文件以清单声明字节数为硬上限，超出立即中止 |
| 白名单外内容 | **忽略、不解包**（官方归档含 README/脚本/test_wavs/bpe.model） |
| 缺文件冒充完整 | 清单要求的文件未全部出现即失败 |
| 解包后文件被改 | 解包时边写边算 SHA-256，并再独立回读校验一次 |
| 符号链接替换已安装文件 | 校验用 `O_NOFOLLOW` 打开，链接直接失败 |
| FIFO/设备冒充模型文件 | `O_NONBLOCK` + `fstat` 校验 `S_ISREG`，快速失败不阻塞 |
| 模型类型错配杀死进程 | 子进程 `--verify-internal` 试加载；`_Exit`/SIGSEGV 只影响子进程 |
| 半成品目录 | 同文件系统暂存 + `renameat2(RENAME_NOREPLACE)` 原子发布；失败/取消只删暂存 |
| 发布前刚被取消 | 在内容 fsync **成功之后、rename 之前**检查取消（fsync 期间的取消也能拦住）；rename 成功后即按成功报告 |
| 发布内容未刷盘 | 内容 fsync 失败即中止发布，绝不 rename |
| 发布后持久性未知 | 父目录 fsync 失败不回滚，返回 `published:true` + `durabilityUnknown:true` 并在 detail 说明 |
| 目标父目录被换成符号链接 | 从 `models` 的父目录起逐级 `openat(O_DIRECTORY\|O_NOFOLLOW)` 钉住父目录 fd，再 `renameat2` 到该 fd；发布后的 fsync 也针对该 fd（不按路径重新解析） |
| 发布瞬间目标被外部创建 | `EEXIST` 即明确失败，绝不覆盖；不支持该 flag 的文件系统同样失败而非退化裸 `rename` |
| 目标路径状态无法判定 | `symlink_status` 返回非 `ENOENT` 错误时提前失败，不当作“不存在” |
| 已存在但未通过校验的版本目录 | **拒绝自动替换**（`EXCHANGE` 会无条件挪走当时在位的对象，无法证明期间未被外部换过）；提示人工确认并移除 |
| 受控目录中间级符号链接 | 从 `models` 起逐级要求真实目录（`lstat` 拒绝链接）；`XDG_DATA_HOME` 及其上层允许符号链接 |
| `--archive` 校验后被替换（TOCTOU） | 先有界复制进私有暂存，之后只操作副本 |
| 超大输入导致长时间读取 | 以清单声明字节数为上限，`fstat` 先判大小，超限不读内容 |
| 用户目录权限被改写 | 只对**本次创建**的目录设权限；已存在目录不 `chmod` |
| 复用陈旧暂存目录 | 暂存目录用 `mkdtemp` 独占创建 |
| 清理时 `.staging` 被换成符号链接 | 清理走「可信父目录 fd + 相对路径 + `O_NOFOLLOW`」；目标 inode 必须等于**创建时钉住且仍打开的 fd**（钉住可阻止 inode 复用，避免"删除后同名重建恰好复用同一 inode"被误判为同一个目录）；不符即**保守保留**（见 §3.0） |
| 源目录在暂存期被替换 | 发布前用 fd 打开源目录并复核创建时的 dev/ino；不一致拒绝发布 |
| 并发安装同一模型 | `flock` 非阻塞锁，占用时返回 `lock_busy` |

同一版本目录**已完整时幂等收敛**（不重复下载，`published:false`）。已存在但校验
不通过的目录**不会被自动替换**：发布以 `publish_failed` 失败并给出该路径，由用户
确认后手动移除再重试。**安装器从不删除或移动任何既有目录。**

---

## 4. 构建与部署（现状）

已由顶层 `CMakeLists.txt` 注册，无需额外改动：

- `voice-input-model-installer` 安装到 `${FCITX5_ADDON_DIR}` —— 与 addon 和
  `libsherpa-onnx-c-api.so` 同目录，因此 `--verify-internal` 能按 $ORIGIN 语义
  找到运行库（无需 `LD_LIBRARY_PATH`）。
- 依赖：LibArchive、OpenSSL(Crypto)、libcurl、jsoncpp、`dl`。
  安装器只在 `SHERPA_ONNX_FOUND` 时构建，且固定定义 `HAVE_SHERPA_ONNX`，因此
  试加载总是可用。若下游在没有该定义的情况下构建（例如去掉 sherpa），
  `--verify-internal` 会报“不可用”，安装以 `trial_load_failed` **失败关闭**，
  **不做无试加载的发布**。
- addon 只复用 `model_catalog.cpp` / `model_paths.cpp` / `sha256.cpp`；
  **`sha256.cpp` 必须显式链接 `OpenSSL::Crypto`**。不要指望从 `CURL::libcurl`
  传递获得：现代 ld 不解析 DT_NEEDED 的间接符号
  （`undefined reference to EVP_sha256` / `DSO missing from command line`），
  且部分发行版的 libcurl 链的是 GnuTLS。
- 测试目标 `model_manager_test`、`model_manager_extract_test`、
  `model_manager_fs_test`、`model_manager_child_test`，均为独立 `main()`，
  不依赖网络、真实模型或 sherpa 运行库。

`integration_check.sh` 是**开发验证脚本**，用真实官方归档与真实 sherpa 库跑端到端
检查；它不是用户下载模型的流程，也不进入 ctest（需要数百 MB 归档与可 dlopen 的
sherpa 库）。

---

## 5. 复现方法

单元测试：

```bash
cmake -B build -DBUILD_TESTS=ON && cmake --build build -j"$(nproc)"
ctest --test-dir build -R 'model_manager' --output-on-failure
```

端到端（开发机）：`integration_check.sh` 是开发验证脚本，不是用户下载流程。

```bash
# 参数是**已存在**的父目录；脚本用 mktemp 在其下建本次专用子目录，
# 只写/删自己的子目录，绝不改动父目录或其它内容。
SHERPA_LIB_DIR=/path/to/libsherpa-onnx-c-api \
  ./src/model_manager/integration_check.sh /tmp

# 已下载官方归档可离线跑（否则 offline 会联网下载，online 缺省跳过）
ARCHIVE_DIR=~/Downloads/model-archives \
OFFLINE_ARCHIVE=~/Downloads/model-archives/offline.tar.bz2 \
  ./src/model_manager/integration_check.sh /tmp

# 省略参数：使用 ${TMPDIR:-/tmp}/opencode（不存在则新建 mktemp 根）
./src/model_manager/integration_check.sh
```

脚本启动时会打印真实的运行目录；成功时清理该子目录，失败或 `KEEP_RUN=1`
时保留以便排查。

单独核对归档与逐文件摘要（不改动已安装内容）：

```bash
A=/tmp/archives/sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30.tar.bz2
sha256sum "$A"          # 30437d84dc4861740d40166212ab05f3728945443d53691620075c4031fb88e1
stat -c %s "$A"         # 597755927
bzip2 -t "$A"
mkdir -p /tmp/verify && tar -xjf "$A" -C /tmp/verify
cd /tmp/verify/sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30
sha256sum encoder.int8.onnx decoder.onnx joiner.int8.onnx tokens.txt
```

清单该有的摘要即 `model_catalog.cpp` 中的常量；不一致说明归档不是清单钉住的那份。

### 实测结果（本机）

| 场景 | 结果 |
|------|------|
| 官方 offline 归档（136,902,463 B）解包 | 4 个文件摘要全部匹配清单 |
| 官方 online 归档（597,755,927 B）解包 | 4 个文件摘要全部匹配清单 |
| 真实网络下载安装 offline-x-asr | 成功，`published:true`，约 27 s |
| 试加载 offline-x-asr / online-zipformer | `ok`，约 3.9 s / 7.7 s |
| **离线模型喂给在线后端** | 子进程 `_Exit(-1)`（退出码 255），父进程存活 |
| 截断的 encoder | 子进程 SIGABRT（信号 6），父进程存活 |
| 路径穿越/绝对路径/符号链接/硬链接/FIFO/重复条目 | 全部拒绝，且不落盘 |
| 白名单外的 README/test_wavs/bpe.model | 忽略，不当作错误 |
| 归档摘要不符 / 用错模型归档 | `archive_mismatch`，退出码 6 |
| 重复安装 | 幂等，`published:false` |
| 已安装文件被篡改 1 字节 | 检测到并重装修复 |
| 篡改 `install.json` 的摘要 | 无影响（marker 不可信） |
| SIGTERM 于解包期 / 试加载期 | 取消，无版本目录、无暂存残留、无孤儿进程 |
| 并发安装同一模型 | 第二个 `lock_busy`，退出码 4 |
| 发布瞬间目标被外部创建 | `publish_failed`，外部符号链接/目录内容不变 |
| 已存在坏目录时重装 | `publish_failed`，原目录内容不变；人工移除后方可重装 |
| `models` 被换成符号链接 | `unreadable`；安装失败且不向链接目标写入 |
| 上层（HOME/XDG 上级）是符号链接 | 安装正常成功（不误伤合法布局） |
| 4 GiB 稀疏文件冒充模型文件 | 立即判为 `tampered`，不读取内容 |
| 无 `HAVE_SHERPA_ONNX` 构建 | 编译通过；安装 `trial_load_failed`，不发布 |

---

## 6. 已知限制

- 下载**不支持断点续传**（已校验的归档不会重复下载）。
- 进程被 `SIGKILL` 时暂存目录会残留；不会自动清理陈旧暂存（避免误删用户资产）。
- 发布依赖 `renameat2(RENAME_NOREPLACE)`：不支持的文件系统上显式失败并保留原目录，
  绝不退化成裸 `rename`。
- 发布的原子性范围是**命名空间层面**（读者要么看不到版本目录、要么看到完整目录），
  保证进程中断/并发下不会出现半套目录。内容与父目录都会 `fsync`；若父目录 fsync
  失败，安装按"已发布、持久性未知"报告（`durabilityUnknown:true`）。
  仍**不声称**在任意文件系统（如非日志型）与断电场景下的持久性。
- 发布的**源与目标**都用 fd 钉住：源目录与目标父目录都从受控根逐级
  `openat(O_DIRECTORY|O_NOFOLLOW)` 打开，`renameat2` 两端都传 fd；源目录还要求
  `fstat` 与创建时记录的 dev/ino 一致。发布后的 fsync 也针对同一批 fd。
  因此"父路径被换成符号链接"或"源目录被整体替换"都会**拒绝发布**而不是误报成功。
- 清理同样基于 fd：`RemoveTreePinned()` 从可信父目录逐级 `openat(O_NOFOLLOW)`
  定位，并与**创建时钉住的 fd**（`PinnedDir`，从创建到清理一直打开）比较 inode，
  然后只删除该 fd 下的内容（`fdopendir` + `unlinkat`，符号链接只 unlink 不跟随）。
  身份不符或无法确认时**保留目录**并放弃删除。
  钉住 fd 是可靠性的前提：目录删除后 inode 会被复用，仅比较历史 dev/ino 值会把
  "删除后重建的同名目录"误判为同一个（Ubuntu 26.04 的 ctest 曾因此失败）。
- 仍然存在的边界见 §3.0；此外：
  - `--check-model` / 幂等校验按路径逐文件打开（每个文件 `O_NOFOLLOW`），
    目录链在校验与打开之间被整体替换成**另一个真实目录**时不保证检测到；
  - `.staging` 若被替换成真实目录且其中恰有同名子目录，身份校验会失败并保留，
    但该目录本身不会被清理；
  - 发布后会把"钉住父目录下的条目"与"按路径解析的 `finalDir`"做一次快照比对，
    不一致则 `pathStateUncertain=true`（不回滚、不声称可用）。
- `--verify-internal` 的运行库搜索顺序：`$VOICE_INPUT_SHERPA_LIB` → 安装器同目录 →
  `<bin>/../lib{,64}/fcitx5/` → 系统默认路径。**从不搜索当前工作目录**。
- 仅支持 Linux（依赖 `flock`、`/proc/self/exe`、`setpgid`、`renameat2`）。
- 代理/镜像不在清单内：下载只走清单里的固定 URL。
