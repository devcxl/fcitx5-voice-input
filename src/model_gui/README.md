# src/model_gui —— fcitx5-configtool 的本地模型管理插件

Qt ConfigUI 插件（`MODULE`），由 `fcitx5-qt{5,6}-gui-wrapper` 加载，为两个配置 URI
提供「本地 ASR 模型」管理页：

| URI | 插件 key（`create()` 收到） | 安装器参数 |
|-----|------------------------------|-----------|
| `fcitx://config/addon/voiceinput/model/online` | `model/online` | `--model online-zipformer` |
| `fcitx://config/addon/voiceinput/model/offline` | `model/offline` | `--model offline-x-asr` |

两个 URI 共用**同一个模块**，靠插件元数据里的 `files` 列表区分；安装器固定为
`--model <id>`，导入官方压缩包时追加 `--archive <path>`。

本目录不修改任何现有文件；构建注册由仓库维护者 / 统一负责 CMake 的会话完成。

---

## 1. 已在本机验证的上游事实

以下均为**本机实测或读上游源码**得到的结论，不是推测。

### 1.1 依赖版本与路径

| 项 | 实测结果 |
|----|---------|
| Qt | Qt6 **6.11.2**（`qt6-base`）、Qt5 **5.15.19**（`qt5-base`），两套可用 |
| fcitx5-qt 头文件 | `/usr/include/Fcitx5Qt6/Fcitx5QtWidgetsAddons/`、`/usr/include/Fcitx5Qt5/Fcitx5QtWidgetsAddons/` |
| fcitx5-qt CMake 包 | `/usr/lib/cmake/Fcitx5Qt6WidgetsAddons/`、`/usr/lib/cmake/Fcitx5Qt5WidgetsAddons/`（版本均为 **5.1.16**） |
| 导入目标 | `Fcitx5Qt6::WidgetsAddons` / `Fcitx5Qt5::WidgetsAddons` |
| API 头 | `fcitxqtconfiguiwidget.h`、`fcitxqtconfiguiplugin.h`、`fcitxqtconfiguifactory.h` 齐备 |
| fcitx5-configtool | 5.1.16（`/usr/bin/fcitx5-config-qt`、`/usr/lib/fcitx5-qt6-gui-wrapper`、`/usr/lib/fcitx5-qt5-gui-wrapper`） |
| 现有插件安装位置 | `/usr/lib/fcitx5/qt6/libpinyindictmanager.so` 等（与 fcitx5-chinese-addons 一致） |

### 1.2 插件识别规则（读 `fcitx5-qt` 5.1.16 源码 + 本地实测）

`FcitxQtConfigUIFactoryPrivate::scan()`：

1. `StandardPaths::global().locate(StandardPathsType::Addon, "qt6", isLibrary, StandardPathsMode::System)`
   —— 扫描 **fcitx addon 目录下的 `qt6/`（或 `qt5/`）子目录**；
2. 用 `QPluginLoader` 读元数据，要求
   `IID == "org.fcitx.Fcitx.FcitxQtConfigUIFactoryInterface"`；
3. 取 `MetaData.addon` 与 `MetaData.files`，把 `addon + "/" + file` 作为注册键；
4. `create(uri)` 先剥掉 `fcitx://config/addon/` 前缀，用剩余全路径查表，
   然后 `instance->create(path.section('/', 1))`。

因此本模块的 `model_gui.json` 必须是：

```json
{ "addon": "voiceinput", "files": ["model/online", "model/offline"] }
```

`create()` 收到的 key 是 `model/online` / `model/offline`（`path.section('/', 1)`
把 `voiceinput/` 切掉）。

**本地实测**（把模块装到临时前缀，设 `FCITX_ADDON_DIRS` 指向它）：

```
qt6-wrapper --test .../model/online   -> exit 0
qt6-wrapper --test .../model/offline  -> exit 0
qt6-wrapper --test .../model/nope     -> exit 1
qt5-wrapper --test .../model/online   -> exit 0
qt5-wrapper --test .../model/offline  -> exit 0
```

非 `--test` 模式用 4 秒超时运行，Qt5/Qt6 wrapper 均保持运行（窗口成功构造），
没有 `Could not find plugin for file.`。

### 1.3 外部选项（ExternalOption）与通知链路

- addon 侧 `ExternalOption`（`fcitx-config/option.h`）会以 `Type=External` +
  `External=<uri>` 出现在配置描述里；configtool 的 `ExternalOptionWidget` 在
  `LaunchSubConfig` 为真时内嵌子配置页，否则调用
  `launchExternalConfig(uri, wid)` → `fcitx5-qtN-gui-wrapper <uri>`。
  （本模块页面走后者，即独立窗口。）
- `FcitxQtConfigUIWidget::saveSubConfig(QString)` 在 gui-wrapper 的 `MainWindow`
  中连接到 `proxy_->SetConfig(path, QDBusVariant(0))`；
  fcitx5 的 `Controller1.SetConfig` 遇到 `fcitx://config/addon/voiceinput/model/online`
  会拆成 addon=`voiceinput`、subPath=`model/online`，调用
  `addonInstance->setSubConfig("model/online", config)`。
- 因此**安装成功**时本模块发出 `saveSubConfig("fcitx://config/addon/voiceinput/model/<kind>")`，
  由 addon 侧实现 `setSubConfig("model/online" | "model/offline")` 重新扫描模型。
  载荷是空 `RawConfig`（`QDBusVariant(0)`），addon 应把它当作「重扫」信号，
  不要依赖配置内容。
- **`published` 优先于 `cancelled`**：安装器的原子发布（`rename`）是临界点。
  只要 `result.published=true`，即使 `cancelled=true` 也照常通知；
  否则发布成功后又收到取消会出现「文件已换、Fcitx 仍用旧模型」的窗口。
- **关闭窗口可能错过通知（已知限制）**：关闭会 SIGTERM 安装器再等待退出；
  若安装器在宽限期内完成了原子发布但 `result` 来不及送达，GUI 侧无法保证
  已发通知（`cancelAndWait()` 会把最后结果留在 `lastResult()`，
  `aboutToQuit` 会补发一次，但进程被 SIGKILL 时仍可能丢失）。
  **因此 addon 侧不应只依赖这一条通知**，需要周期性或重新进入输入法时
  重新解析模型目录（父会话负责该协调）。

**本地实测（Qt 级信号链）**：在 `dbus-run-session` 私有总线上跑一个假
`org.fcitx.Fcitx5` 服务（记录 `SetConfig` 调用），把
`FcitxQtConfigUIWidget::saveSubConfig` 接到 `Controller1.SetConfig`，
用假安装器触发一次成功安装，收到：

```
fcitx://config/addon/voiceinput/model/online
fcitx://config/addon/voiceinput/model/offline
```

取消路径不产生任何 `SetConfig`。未触碰用户真实 fcitx5。

### 1.4 状态查询（GetConfig）契约

`Controller1.GetConfig("fcitx://config/addon/voiceinput/model/status/{online,offline}")`
由引擎侧提供，Option 字段固定为：

| 字段 | 含义 | 取值 |
|------|------|------|
| `State` | 引擎运行状态 | `idle` / `loading` / `ready` / `failed` |
| `Detail` | 细分明细 | `loading` / `reloading` / `init_failed` / … |
| `Model` | **实际运行中**引擎的模型标识 | 任意字符串 |
| `Version` | **实际运行中**引擎的版本 | 任意字符串 |

界面语义（`model_status.cpp` + `model_manager_widget.cpp`）：

| 情形 | 识别状态显示 |
|------|-------------|
| `State=ready` | **已就绪**（只有这一种情况说"就绪"） |
| `State=loading` | 加载中…（`Detail=reloading` → 重新加载中…） |
| `State=idle` | 未加载（引擎尚未激活） |
| `State=failed` 且 `Model`/`Version` 仍在 | **加载失败，继续使用旧模型：<Model>（<Version>）** |
| `State=failed` 且无 Model/Version | 加载失败 |
| State 缺失 / 接口报错 | 未知（…） |
| State 取值不在集合内 | 未知（引擎返回：<原文>） |

要点：

- **安装 ≠ 就绪**：安装器成功只代表文件就位，识别状态始终来自该接口；
- `failed` 不能笼统写成"不可用"：重载失败时旧引擎仍在服务，必须写明
  "继续使用旧模型"；
- 安装状态**不依赖**该接口（引擎目前不提供 `Installed` 字段），
  由下节的 `--check-model` 检查器负责。

**自动刷新（有界、无哈希）**：

- 页面可见期间每 **2 s** 只调用一次 `GetConfig` 刷新识别状态；
  `showEvent` 立即查询一次，`hideEvent`/析构/退出停止；
- 同一时刻最多一个在途查询（`statusQueryPending_`），另有 4.5 s 看门狗
  兜底，D-Bus 丢失不会永久卡在"查询中"；
- **轮询绝不触发 `--check-model`**：安装文件哈希只在打开、安装结束、
  手动刷新与 fcitx5 重连时执行。

**本地实测**：

- 真实 fcitx5（状态子配置尚未实现）→ 识别状态为
  `未知（引擎未提供状态接口）`；
- 私有 D-Bus 假服务先返回 `loading`、再返回 `ready`：页面打开后无需操作
  自动变为 `已就绪`，期间 `--check-model` 只执行 1 次（`model_gui_poll_test`）。
- 假服务按契约返回：在线页 `已就绪`；离线页
  `加载失败，继续使用旧模型：offline-x-asr（2026-06-03）`。

### 1.5 安装状态（`--check-model`）

引擎不提供 `Installed` 字段，安装状态改由**独立子进程**查询：

```
voice-input-model-installer --check-model <online-zipformer|offline-x-asr>
# stdout 一行：
{"event":"installed_status","model":"<id>","state":"absent|valid|tampered|unreadable"}
```

实现约束（`model_check_runner.{h,cpp}`）：

- **独立 QProcess**，与安装进程分开；异步、带 60 s 超时；
- 主线程**不做**哈希、不读 marker、不看文件是否存在：一切以安装器按
  内置清单逐文件校验的结论为准；
- 只采信 `installed_status` 事件；超时/崩溃/无事件/未知取值一律
  `Unknown`（界面显示"未知（无法确认安装状态）"）；
- 安装器退出码：`0` 表示查询完成（**state 任意**）；`3` 未知模型 id（stdout
  为空，此时显示未知）；`2` 缺参数；`16` 无法确定 XDG 目录；`5` 表示检查被
  SIGINT/SIGTERM 打断（**不输出状态行**，界面显示未知）。诊断写 stderr；
- 安装器保证**完全只读**（不建目录、不加锁、不读 `install.json`），
  判定只来自内置清单常量 + 实际文件哈希；
- 关闭窗口/析构会 `cancelAndWait()`，不留孤儿进程。

界面映射（`status_text.cpp`）：

| state | 安装状态显示 |
|-------|-------------|
| `valid` | 已安装（已按内置清单校验） |
| `absent` | 未安装 |
| `tampered` | 已损坏（缺文件、大小或摘要不符，或不是普通文件）——请手动检查模型目录 |
| `unreadable` | 不可读（存在但无法判定：符号链接或读取失败）——请手动检查模型目录 |
| 未知/失败/超时 | 未知（无法确认安装状态） |

同一条 `installed_status` 还会携带清单元数据，界面在「模型信息」行展示：

| 字段 | 展示 |
|------|------|
| `version` | 版本（缺失显示未知） |
| `archiveSize`（字节） | 下载大小，如 `130.6 MiB（136,902,463 字节）`（缺失显示未知） |
| `source` | 官方 release URL（可点击，HTML 已转义；缺失显示未知） |

这些值**全部来自安装器输出**，GUI 不复制任何模型常量，避免与清单漂移。
三个字段与 `state` 无关：**`absent` 时也会返回**，因此"未安装"界面就能显示
将安装哪个版本、需要多少流量、来自哪个官方地址（本机实测 online 570.1 MiB /
offline 130.6 MiB）。

触发时机：

1. 页面首次打开（`load()` → `refreshStatus()`）；
2. 点击「刷新状态」；
3. 安装结束（成功/已发布/取消/失败）后重新核对真实磁盘状态；
4. fcitx5 重新连接（`QDBusServiceWatcher` 监听 `org.fcitx.Fcitx5` 注册）。

> 安装器侧建议：安装完成后的重查已经内建（第 3 点），不需要额外的全局
> watcher；本模块的 watcher 只用于 fcitx5 进程重启的场景。
> 安装进行中展示"安装中…"，最终以安装器 `result` 事件为准；检查结果仅在
> 空闲时展示（避免把安装器正在写入的中间态误报为 `tampered`）。

**保守语义（安全复核后）**：安装器不会自动覆盖或删除已存在但损坏的版本目录，
`--check-model` 仍会报告 `tampered`，安装会以 `publish_failed` 失败并由
`result.detail` 给出具体路径。因此界面：

- 把 `tampered` / `unreadable` / `publish_failed` 一律表述为
  「请手动检查模型目录后重试」；
- 在安装失败时把 `detail`（含需人工移除的路径）放进日志与提示 Tooltip；
- **不承诺自动修复、不提供删除按钮、不承诺删除旧目录**；
- `state=valid` 时重装仍是幂等成功（`published:false`）。

**持久性警告**：`result.durabilityUnknown=true` 表示发布成功但父目录 `fsync`
失败。界面用橙色警告写明「断电或异常重启后可能丢失；请检查磁盘，若模型丢失
再重新安装」——**不承诺重装即可修复**：安装器对 `state=valid` 的目录是幂等
直接返回成功，并不保证重新 `fsync`。

**路径状态警告**：`result.pathStateUncertain=true` 表示安装已发生（发布成功、
不回滚），但发布后按路径回查发现目标已不是刚放入的那个目录（父目录被移动/
替换等）。这是**尽力而为的事后快照**，不保证该路径上的模型当前可用，也不保证
其他进程之后不再搬动它。界面**不得声称"已安装"**（发布目录可能已被搬走、受控路径甚至不存在），
统一展示「发布已发生，但目标路径状态不确定，请检查模型目录后刷新状态」；
`detail` 中的路径放入提示与日志。

该状态下**不提供"重试安装"**：目标已存在，重试通常会得到 `publish_failed`；
界面提示用户点「刷新状态」（走 `--check-model <id>`，协议不变）。

路径警告不表示失败，但也**不表示已安装/可用**；持久性警告单独出现时允许
「已安装」。两者都按可选字段处理（缺失为 `false`），且
**都照常发送 `saveSubConfig`** 让 addon 重新读取；「刷新状态」始终可用，
安装状态以 `--check-model` 的 `absent/valid/tampered/unreadable` 为准。

安装进行中不并发检查（避免两个安装器进程竞争）；开始安装会取消在跑的检查。

---

## 2. 安装器接口（本模块消费的契约）

来自 `src/model_manager`（同一仓库）：

```
voice-input-model-installer --model <online-zipformer|offline-x-asr> [--archive <path>]
```

- stdout **只**输出 JSON Lines；日志走 stderr；
- 事件：`start` / `download` / `verify_archive` / `extract` / `verify_file` /
  `verify_trial` / `publish` / `result`；
- `result` 只出现一次且是最后一条，含
  `ok`、`published`、`cancelled`、`durabilityUnknown`、`pathStateUncertain`、
  `error`、`detail`、`versionDir`、`elapsedMs`；
- 取消：SIGTERM/SIGINT/SIGHUP → 清理暂存并输出 `result(cancelled)`，退出码 5；
  原子发布之后取消仍按 `ok:true` 报告；
- 退出码见 `src/model_manager/README.md` §2。

本模块的判定（`installer_runner.cpp`）：

| 情况 | 结论 |
|------|------|
| 收到 `result` 且 `ok=true`、退出码 0，且 `durabilityUnknown` 非真 | 成功 |
| `ok=true`/`published=true` 但 `durabilityUnknown=true` | **已安装**，橙色警告「持久性未知：断电或异常重启后可能丢失；请检查磁盘，若模型丢失请重新安装」，仍通知 addon |
| `ok=true`/`published=true` 但 `pathStateUncertain=true` | **不得称"已安装"**：橙色警告「发布已发生，但目标路径状态不确定，请检查模型目录后刷新状态」；detail 路径进提示/日志；不提供"重试安装"，建议点「刷新状态」走 `--check-model`，仍通知 addon |
| 上述两个标记同时为真 | 同一路径警告 + 持久性警告同时显示，**均不出现"已安装"** |
| `durabilityUnknown` 字段缺失 | 按 false 处理（兼容旧安装器） |
| `result` 报 `cancelled`，或用户取消后正常退出 | 已取消 |
| `result` 报 `false` / 退出码非 0 | 失败（错误码翻译为中文） |
| 退出码 0 但**没有** `result` | **不算成功**（`no_result`） |
| 取消宽限期（5 s）后被 SIGKILL | `cancelled_unconfirmed`，界面提示「最终状态未确认」 |
| 进程崩溃 | 失败（`crashed`） |
| 安装器路径缺失/不可执行 | 立即失败并禁用按钮 |

### 运行期路径

安装器路径来自**构建期注入**的绝对路径宏 `VOICE_INPUT_MODEL_INSTALLER`：

- 不使用 `PATH` 查找（避免多前缀 / 多架构打包时命中错误二进制）；
- 未注入时界面明确提示「构建时未提供安装器路径」并禁用安装按钮；
- 测试通过构造函数注入路径（`ModelManagerWidget(kind, parent, program)`）。

---

## 3. 取消与窗口关闭

- 页面提供「取消」按钮；
- **关闭窗口**：`QCoreApplication::aboutToQuit` 与 widget 析构都会调用
  `InstallerRunner::cancelAndWait()`，先 SIGTERM 让安装器清理暂存，
  宽限 3 s 后 SIGKILL，保证不留孤儿进程；
- 取消后**若未发布**则不发送 `saveSubConfig`；若安装器已报告 `published=true`
  （发布后取消），仍会通知一次，避免残留旧模型。

**本地实测**：用忽略 SIGTERM 的假安装器验证 `cancelAndWait` 会强杀并返回；
用写 PID 文件的假安装器验证 widget 析构后进程已消失；取消不产生 `SetConfig`。

---

## 4. 构建与集成

### 4.1 依赖（每个 Qt 主版本独立）

```
Qt${N}::Core / Gui / Widgets / DBus
Fcitx5Qt${N}::WidgetsAddons        （随带 Fcitx5::Utils）
```

### 4.2 主 CMakeLists 需要做的接入（本目录未改，供维护者参考）

```cmake
# 1) 让安装器与 GUI 解析出的路径一致（路径必须是绝对路径）
set(VOICE_INPUT_MODEL_INSTALLER
    "${CMAKE_INSTALL_FULL_LIBDIR}/fcitx5/voice-input-model-installer")
# 2) 引入模块（内部自动识别可用的 Qt5/Qt6，缺依赖则跳过并打印 STATUS）
add_subdirectory(src/model_gui)
```

- 目标名：`voice-input-model-gui-qt5` / `voice-input-model-gui-qt6`，
  输出文件名统一为 `libvoice-input-model-gui.so`，分别安装到
  `${FCITX5_ADDON_DIR}/qt5`、`${FCITX5_ADDON_DIR}/qt6`；
- 模块自行定义 `FCITX_GETTEXT_DOMAIN="fcitx5-voice-input"` 与
  `VOICE_INPUT_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"`；
- `add_subdirectory` 前必须已有 `FCITX5_ADDON_DIR`（顶层已有），否则回落到
  `lib/fcitx5`；
- 单独构建：

```bash
cmake -S src/model_gui -B build-model-gui \
  -DVOICE_INPUT_MODEL_INSTALLER=/usr/lib/fcitx5/voice-input-model-installer
cmake --build build-model-gui -j"$(nproc)"
```

### 4.3 安装期布局

```
${FCITX5_ADDON_DIR}/qt6/libvoice-input-model-gui.so
${FCITX5_ADDON_DIR}/qt5/libvoice-input-model-gui.so   # 若该发行版有 Qt5 wrapper
${FCITX5_ADDON_DIR}/voice-input-model-installer       # 安装器（由主集成安装）
```

---

## 5. 翻译

所有用户可见文案都走 **fcitx gettext 域**（`fcitx5-voice-input`），不用 Qt `.ts`：

- 源码 msgid 直接写**中文**（与 addon 现有 `voice_input-config.h`、
  `engine.cpp` 的写法一致），因此 `zh_CN` 不追加 po 条目也能正确显示；
- 其它语言只需在 `po/<lang>.po` 中补 `msgid`（中文原文）→ `msgstr` 即可；
- 域名与 locale 目录在插件构造时通过
  `fcitx::registerDomain("fcitx5-voice-input", VOICE_INPUT_LOCALE_DIR)` 注册，
  复用 addon 已安装的 `fcitx5-voice-input.mo`；
- **本次没有改动 `po/`**（共享文件，避免与其它改动冲突）。需要提取时：

```bash
xgettext --language=C++ --from-code=UTF-8 \
  --keyword=Tr --keyword=Tr:1,2c \
  -o /tmp/model_gui.pot src/model_gui/*.cpp
# 再 msgmerge 进 po/<lang>.po
```

`Tr()` 定义在 `i18n.h`，是 `translateDomain` / `translateDomainCtx` 的薄封装；
调用点必须传字符串字面量，才能被 xgettext 提取。

---

## 6. 文件说明

| 文件 | 职责 |
|------|------|
| `plugin.h/.cpp` | `FcitxQtConfigUIPlugin` 实现 + 元数据；`create(key)` 分发两种模型 |
| `model_gui.json` | Qt 插件元数据：`addon` + `files` |
| `model_kind.h` | 模型种类 ↔ key / 安装器参数 / 配置 URI / 状态 URI |
| `installer_protocol.h/.cpp` | 单行 JSON 解析（不依赖 Qt GUI，可单测） |
| `installer_runner.h/.cpp` | `QProcess` 生命周期、事件转发、取消与收尾判定 |
| `installer_error_text.h/.cpp` | 稳定错误码 → 中文文案 |
| `model_status.h/.cpp` | DBus `GetConfig` 状态查询 + 纯函数解析（State/Detail/Version，缺失即未知） |
| `model_check_runner.h/.cpp` | 异步 `--check-model` 检查（独立进程、超时、取消、只信显式事件） |
| `status_text.h/.cpp` | 状态/详情文案（ready 才说就绪；failed 保留旧模型提示） |
| `model_manager_widget.h/.cpp` | 页面行为：状态轮询、检查/安装流程、通知与取消 |
| `model_manager_widget_ui.cpp` | 页面布局（控件构建，拆出以控制单文件长度） |
| `i18n.h` | gettext 域封装 |
| `CMakeLists.txt` | Qt5/Qt6 双模块构建与安装 |
| `tests/` | 协议、状态、进程控制、插件识别与信号链测试 |

---

## 7. 测试

```bash
cmake -S src/model_gui -B build-model-gui \
  -DVOICE_INPUT_MODEL_GUI_BUILD_TESTS=ON \
  -DVOICE_INPUT_MODEL_INSTALLER=/path/to/voice-input-model-installer
cmake --build build-model-gui -j"$(nproc)"
ctest --test-dir build-model-gui --output-on-failure
```

覆盖内容（本机全部通过）：

| 测试 | 覆盖 |
|------|------|
| `model_gui_installer_protocol_test` | 真实事件形态、进度换算、未知事件不误判为 `result` |
| `model_gui_model_status_test` | `idle/loading/ready/failed` 语义、failed 保留旧模型、未知 State 原文、可选安装字段、嵌套与大小写 |
| `model_gui_installer_runner_test` | 成功/幂等/失败/无 result/崩溃/取消/强杀/路径校验/**关闭窗口时 published 结果保留** |
| `model_gui_model_check_runner_test` | `installed_status` 解析（含 version/archiveSize/source）、四种状态、垃圾输出、崩溃、超时、取消/无孤儿进程、路径校验；另用真实安装器手工验证 `absent` |
| `model_gui_poll_test` | 私有 D-Bus：`loading→ready` 自动刷新、轮询不重复哈希、查询不叠积（需要 `dbus-run-session`） |
| `model_gui_plugin_test` | 插件 IID 与元数据、两个 URI 识别、`create()` 标题区分、`saveSubConfig` 载荷、取消不通知、**published+取消仍通知**、**关闭窗口时已发布补发通知**、析构取消进程、**元数据 UI（含缺失→未知）**、**durabilityUnknown 警告且仍通知**、**pathStateUncertain 警告（不出现“已安装/安装成功/已就绪”）**、**两警告并存** |

`model_gui_plugin_test` 用 `QT_QPA_PLATFORM=offscreen`，不需要显示器。

---

## 8. 未覆盖 / 已知限制

- 引擎侧尚未提供 `model/status/online|offline` 子配置（当前实测报
  `Failed to get addon config.`），界面按设计显示「未知」；接入后无需改本模块。
- GUI 关闭时可能错过发布通知（见 §1.3），addon 需自行周期/重新进入时校验。
- `--check-model` 已由 `src/model_manager` 实现并通过本模块端到端验证
  （本机两个模型均为 `absent`）；若安装器版本较旧（不支持该参数），
  界面显示「未知（无法确认安装状态）」并在日志记录 stderr 诊断。
- 未实现「安装进度断点续传」等安装器能力，界面只做展示。
- 归档文件选择只在 GUI 侧做「文件存在」检查；完整性与安全性由安装器负责。
- Qt5 变体只做了加载/构造实测，没有单独的 Qt5 单元测试目标（UI 逻辑共用同一份源码）。
