# Issue #54：系统管理录音设备与失焦保流方案

> 调研日期：2026-10-12；原始代码基线：`main@80e062afb268d2e952b88f64baa1182b92ff8f70`。
> 关联：[Issue #54](https://github.com/devcxl/fcitx5-voice-input/issues/54)。用户已批准实现；本报告记录修正后的业务契约，真实双麦/桌面验收尚未完成。

## 摘要

1. **根因**：PulseAudio 后端按名称及列表顺序猜测麦克风，再显式指定 source，绕过系统默认及按应用设备恢复；内置源无声但成功建流时不会触发 PipeWire fallback。静态根因置信度：高。[S1/S2]
2. **路由修复**：删除猜源和 `pactl` 子进程，`pa_simple_new` 的 `dev` 传 `nullptr`；保留应用/流身份、后端优先级、格式。不新增设备配置。机制置信度：高。[S3–S7]
3. **用户修正后的生命周期**：Voice Input 开启后保持录音流，失焦不释放；失焦采样丢弃，不启动识别，取消现有 ASR/LLM，不提交迟到结果。回焦复用流、开始新识别周期；明确关闭/切换其他输入法或退出才释放。此项是已确认需求，不是外部事实推断。
4. **默认输入与应用偏好不同**：音频管理器保存的本应用设备偏好可以覆盖全局默认；推荐尊重它，而非强制抢回默认。机制置信度：高。[S4/S6/S7]
5. **边界**：保持录音流不是独占硬件。已上传音频不能撤回；本方案也不根治旧有 PulseAudio 阻塞停止问题，不承诺零延迟或采样级的服务端缓冲时间界限。

## 问题定义与方案演进

#54 报告 Arch + PipeWire/pipewire-pulse 环境下，内置声卡排在 USB 麦克风前，插件选到未插线的内置输入。报告者建议设备下拉框；用户要求改为由系统音频管理器选择。

初版研究建议“先设置系统默认输入再激活”，并保持失焦停流。用户明确纠正：**语音输入开启时持续保持麦克风流，方便在管理器录音页选择，不因失焦而消失**。因此初版的 200ms 延迟停流方案不实施，也不添加临时测试录音模式。

已确认的业务状态：

| 状态 | 录音流 | VAD/ASR/LLM | 输入框结果 |
|---|---|---|---|
| 未开启/明确关闭 | 无 | 停止 | 不提交 |
| 开启且 Voice Input 上下文活动 | 有 | 正常处理 | 仅当前 generation 提交 |
| 开启但失焦/活动上下文销毁 | 保持 | 停止，采样丢弃 | 迟到结果失效 |
| 回到 Voice Input 上下文 | 复用健康流 | 清旧缓存，重新启动 | 新 generation |

“开启”不是安装 addon 或把它添加到输入法列表，而是激活 Voice Input。失焦时保流会令系统继续显示麦克风使用，这是预期行为；用户应主动切换其他输入法或退出 Fcitx 释放。

## 研究方法

### 三轮迭代

1. 阅读 issue、capture/pipeline/engine、现有测试；CodeGraph 追踪调用；Git 历史确定猜源策略来历。
2. 核验 PulseAudio 默认参数、simple flags、stream restore、pavucontrol 移流，以及 PipeWire/WirePlumber 的默认、目标恢复和可移动策略。
3. 搜索反证：显式 source 是否禁移、默认变化是否必移、虚拟麦克风是否应过滤、失焦流是否消失；用户修正后，进一步核验精确 Fcitx 事件调用时机及暂停/恢复并发边界。

### 来源与时效约束

同一项目的文档/源码/镜像不算独立来源。路由原则由 PulseAudio 与 PipeWire/WirePlumber 的不同实现交叉支持。项目独有根因由源码和报告者现象支持，不能用外部资料替代现场复现。ArchWiki 与 OpenAL 讨论只辅助运维及反方评估。

PulseAudio Doxygen 页面为 16.0，v17.0 源码也已超过一年；只用其明确版本核验稳定 API，并与其他实现交叉检查。本机 SDK `17.0-98-gb096` 的短提交在所查镜像不可解析，不声称已核验精确快照全部差异。PipeWire 核验 1.6.8、WirePlumber 0.5.18；报告者未给出精确运行版本。Fcitx 核验本机 SDK 与官方 `5.1.22` tag 的源代码，而非只依赖头文件描述。

## 关键发现与根因

### 1. 名称排序不代表用户意图

原 `FindBestSourceName()` 跳过 `.monitor`/`echoCancel`，保留第一个候选，遇到 `alsa_input.*Mic1*` 特殊覆盖。不读取默认源或端口可用性。算法甚至不严格优先 ALSA：第一个非 ALSA 候选也可能被保留。两个源都为 RUNNING 时按状态排序仍无解。

提交 [18410ea](https://github.com/devcxl/fcitx5-voice-input/commit/18410ea37ee4761188c7722c69c7b219da7d84f6)（2026-06-27）为某台机器上 echo-cancel 源无信号而引入猜源；[89397a7](https://github.com/devcxl/fcitx5-voice-input/commit/89397a71f57f4e787226a48507f35529ee2142d0)（2026-06-28）删除设备设置却保留猜源。根本问题是把单机处理链故障固化为全局路由策略。

依据：[S1] 2026-10-11、23/25；[S2] 基线及历史、24/25。**置信度：高（静态）；端到端硬件复现未完成。**

### 2. `nullptr` 是委托路由，不是硬绑定一个默认源名

`dev=NULL` 允许客户端默认配置和服务端路由策略参与。PulseAudio 的 `PULSE_SOURCE`/client.conf 可以覆盖；stream restore 可以恢复应用偏好，显式 source 已设时则跳过创建期设备恢复。当前实现每次重建又指定列表首项，因此管理器移动流后，下次还可能选错。

不应先执行 `pactl get-default-source` 再指定名称：仍有子进程、查询/建流竞态及显式设备优先问题。日志只能说“路由由音频服务管理”，不能把 default 字样当成已查询的实际设备。

依据：[S3/S4] 16.0/v17.0、24/25；[S6/S7] 0.5.18/1.6.8、25/25。**置信度：高（机制）；报告者具体覆盖策略未知。**

### 3. simple API 已允许音频管理器移流

v17.0 `simple.c` 的录音连接 flags 未设 `PA_STREAM_DONT_MOVE`；pavucontrol 录音页调用 `pa_context_move_source_output_by_name`；pipewire-pulse 将移流请求写入目标 metadata，由会话策略管理器链接。

**显式 source 不等于禁止移流**。当前故障是错误的创建期选择与重建后覆盖。无需为移流迁移异步 API，但 simple API 没有设备移动回调、实际设备查询或自定义 proplist 参数。

依据：[S4] v17.0、24/25；[S5] 固定提交日期 2026-05-01、24/25；[S7] 1.6.8、25/25。**置信度：高（可移动机制）；真实切换听感/延迟未知。**

### 4. 默认与按应用偏好必须区分

| 操作 | 作用域 | 已运行流及恢复 |
|---|---|---|
| 系统输入设备设默认，`set-default-source`/`wpctl set-default` | 全局 | 已有应用覆盖可能不跟随；迁移由服务策略决定 |
| 录音页选设备，`move-source-output` | 当前流 | 请求移动；恢复开启且身份匹配时下次可恢复 |
| patchbay 手工连线 | 图连接 | 不等同管理器目标恢复，持久化取决于工具 |

WirePlumber 默认启用 moving/restore-target；PulseAudio 默认变化只按条件移动流。不能承诺更换默认立即移动所有应用，也不应设 `state.restore-target=false` 来破坏按应用选择。

依据：[S4/S6/S7/S8] 24–25/25；[S9] 动态运维资料、22/25。**置信度：高（机制区别）；桌面 UI 一致性：中。**

### 5. Fcitx 的失焦与明确切换可以区分

Fcitx 5.1.22 `Instance::deactivateInputMethod()` 把原始 `FocusOut` 或 `SwitchInputMethod` 事件传入 engine，随后才发布 Deactivated notification。只按 notification 名称判断会丢掉原因。

对未聚焦上下文的输入法切换，core 跳过 engine 回调。因此还需监听 switch 事件，在保流期间通过托盘/远程切换其他输入法时释放；活动上下文销毁应停止识别，但不冒充“明确关闭”。

依据：[S10] 精确 SDK/tag、25/25；[S2] 原项目事件实现、24/25。**置信度：中（各自源码确定，发行版实际焦点与托盘流程仍需验收）。**

## 三种方案对比

| 维度 | 保守：委托路由，但失焦停流 | 中间：委托路由、失焦保流（已选） | 激进：异步音频与全新录音模式 |
|---|---|---|---|
| 修复错选 | 是 | 是 | 是 |
| 管理器录音页方便选设备 | 焦点切换可能导致流消失 | 是，流持续存在 | 可以，但超过当前需求 |
| 失焦识别/上传 | 停止 | 停止，丢弃采样 | 必须另外定义 |
| 实现/测试成本 | 低 | 中：分离采集和识别生命周期 | 高：更多状态、SDK 和线程边界 |
| 维护成本 | 低 | 中，保留现有 SDK 和接口 | 高，需独立架构评审 |
| 行为迁移 | 虚拟默认重新生效 | 同左，且失焦仍显示麦克风占用 | 录音控制方式及依赖可能改变 |
| 服务费用 | 无新增服务 | 无新增服务；失焦不新开识别请求 | 取决于另定义的识别模式 |

应用内设备下拉框或 USB/RUNNING 排序均不推荐：前者形成双重决策点，后者不能表达用户意图。最佳方案是中间方案；异步有界停止作为已有债务独立交付。

## 已批准的实现设计

- **路由**：PulseAudio 不指定设备；原生 PipeWire 继续 `PW_ID_ANY + AUTOCONNECT`，不加目标或禁止移动属性。保留现有身份及 Pulse→PipeWire 优先级。
- **Engine**：失焦立即失效 generation/输入框引用并调用 `Pause()`；明确停用调用 `Stop()`。监听无焦点 switch 和 group change；上下文销毁暂停识别，避免后台继续上传。
- **Pipeline**：`running_` 只代表识别；采集由 `capture_` 单独拥有。`Start()` 复用健康流，恢复前清队列；死流安全清理后重新创建。`Pause()` 关闭帧投递、暂停结果、停止 VAD/dispatcher，再取消/回收会话、清缓冲；`Stop()` 另关闭采集；`Abort()` 永久关闭通知并完整清理。
- **Capture**：保流时继续消费/丢弃采样。投递 epoch 与 mutex 防止暂停返回后再入队；epoch 而非布尔值防止一条 read 跨快速暂停/恢复泄漏旧帧。主线程不并发 free 或切换 queue 指针。
- **PipeWire**：RT 回调不加 mutex、不做 VAD、不分配，只读 epoch、写 ring，并在写完后发布 producer epoch。Drain 观察新 epoch 的生产者屏障后才有界 drain/重置组帧，避免旧回调在清理后补写。
- **VAD**：每轮启动重置 Silero 模型状态与 pre-roll。失焦已经失效 UI generation，不再等待 speaking 回调清理旧 UI。

应用队列与跨切换在途帧被丢弃；PulseAudio 保流期间持续读取，不新增异步 flush，也不将服务端固有缓冲/远程延迟包装成采样级保证。切换或恢复瞬间可能丢弃过渡音频，不承诺无间断。

## 验证、风险与回滚

### 测试公开边界

用户确认：真实 Engine 事件入口、真实 Capture 与 Pipeline 启动/暂停/停止接口；只替换外部音频库及 VAD/ASR 边界，不访问生产私有状态，不新增生产依赖注入层。

已取得红→绿证据：旧猜源把内置源传入且调用 pactl；旧失焦生命周期在超过 200ms 后 Stop；未连接投递门时暂停音频入队并在恢复后回放；只把 Pause 写成 Stop 时真实 pipeline 释放采集。测试随后覆盖对应修复。最终完整测试/构建结果以交付记录为准，不能将假库测试当成硬件验收。

测试覆盖目标：默认参数及身份、无子进程、失焦保流但无识别、跨 read 快速暂停恢复、应用缓存清理、死流恢复、显式/无焦点关闭、上下文销毁及迟到结果过滤。PipeWire 另覆盖旧 RT callback 跨 epoch 写入与 error 后释放。假库仅在构建目录，不接触用户音频服务或网络。

本地验证：Release 完整构建、36 项 `ctest` 全通过；新增核心生命周期测试重复 20 轮通过；屏蔽 PipeWire pkg-config 的可选后端构建通过，相关 11 项测试通过（之后补充的死流关闭场景由最终测试覆盖）。`nm/readelf` 确认 addon 无直接 `pa_*`/`pw_*` 未定义符号或录音库 DT_NEEDED。PipeWire 假库不使用新版 `pw_buffer.time` 字段，以兼容 Debian 12 的旧 SDK。真实硬件/桌面未验证。

本地复现命令：

```bash
cmake -B build-lifecycle -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTS=ON
cmake --build build-lifecycle -j4
ctest --test-dir build-lifecycle --output-on-failure
```

不执行安装。发布门禁还包括无录音库 DT_NEEDED/直接未定义符号、无 PipeWire 可选构建、真实双麦的默认/移流/恢复、KDE/GNOME 失焦和托盘关闭。改路由只由用户或隔离测试执行，不能清空用户整个音频状态目录。

### 最坏情况与反方

- **错误默认虚拟源重新生效**：旧历史问题可能无声；正常降噪源也不能被名称规则禁用。用户在管理器检查源和电平。若选到回放 monitor，活动识别可能上传播放内容，必须明确提醒。
- **失焦仍占用麦克风**：这是用户明确批准的行为，系统指示灯保持；不等于识别仍进行。已经送出的请求无法撤回，取消只尽力终止在途操作、拒绝迟到结果。
- **Stop 卡住**：PulseAudio 同步 read/join 的已有风险仍在。识别先停、会话先取消再关闭采集，降低关闭期间继续识别的风险，但不承诺主线程停止上界，不用跨线程 free/detach 掩盖。
- **应用偏好不跟随默认**：这是尊重用户选择，不是错选算法回归。管理员可以关闭恢复；两个后端恢复身份也不保证一致。不改身份或数据库做隐式迁移。
- **异步 API 更强**：可获得有界生命周期与设备状态，但不是管理器移流的前提。OpenAL 对实际设备名/配置的争论适用于需要按设备混音的 API，本插件不因此增加设备枚举。[S11]

独立后续任务 **CAPTURE-54-001：有界 PulseAudio 生命周期与状态反馈**，依据 [#14/PR #17 根因记录](https://github.com/devcxl/fcitx5-voice-input/issues/14#issuecomment-5240325469)。先复现设备挂起/服务无响应，再批准异步设计并验证无 UAF、无阻塞停止和可重试；不混入此次交付。

回滚是整体回退本次 capture/recognition 生命周期变更，无用户配置或数据库迁移。不存在旧猜源兼容开关或双写路径。

### 元评审

- 剩余未知：报告者具体默认/恢复策略、音频栈版本、真实桌面流程、硬件切换延迟和 PulseAudio 服务端缓冲。
- 最弱证据：真实声音连续性和桌面设备选择，尚无本项目现场验收。
- 可能错误的假设：USB 已是默认、移动/恢复策略开启、所有桌面托盘选择发出相同事件；需现场确认。
- 已考虑例外：虚拟输入/monitor 隐私、蓝牙 profile、远程 Pulse 服务、管理员关闭恢复、非 WirePlumber 策略、多输入上下文。
- 下一轮最高收益：真实双麦机器上的管理器录音页切换、失焦无识别、回焦新音频、托盘关闭四条流程。继续文献搜索不能替代这些实验。
- 停止文献调研的理由：核心机制已由不同实现支持，主要反方覆盖；未满足双源/实验验证的具体桌面及连续性结论仍标未知，不提高置信度。

## 来源、时间与评分

评分为权威性/一手性/时效性/独立性/可验证性，各 1–5；文档与同项目源码合并计来源族。日期未知只标访问日期，不编造发布时间。

| ID | 来源 | 版本/时间 | 评分与使用边界 |
|---|---|---|---|
| S1 | [Issue #54](https://github.com/devcxl/fcitx5-voice-input/issues/54) | 2026-10-11 | 4/5/5/5/4=23；报告者自述 |
| S2 | [capture](https://github.com/devcxl/fcitx5-voice-input/blob/80e062afb268d2e952b88f64baa1182b92ff8f70/src/addon/capture/pulse_audio_capture.cpp)、[engine](https://github.com/devcxl/fcitx5-voice-input/blob/80e062afb268d2e952b88f64baa1182b92ff8f70/src/addon/engine.cpp)、[pipeline](https://github.com/devcxl/fcitx5-voice-input/blob/80e062afb268d2e952b88f64baa1182b92ff8f70/src/addon/pipeline/pipeline.cpp) 与 Git 历史 | 基线 2026-10-11；历史 2026-06 | 5/5/5/4/5=24；确定原实现，不证明硬件连续性 |
| S3 | [simple.h](https://freedesktop.org/software/pulseaudio/doxygen/simple_8h.html) | 页面 16.0；访问 2026-10-12 | 5/5/4/5/5=24；旧文档，稳定签名 |
| S4 | [simple.c](https://github.com/pulseaudio/pulseaudio/blob/v17.0/src/pulse/simple.c)、[stream.c](https://github.com/pulseaudio/pulseaudio/blob/v17.0/src/pulse/stream.c)、[client-conf](https://github.com/pulseaudio/pulseaudio/blob/v17.0/src/pulse/client-conf.c)、[stream restore](https://github.com/pulseaudio/pulseaudio/blob/v17.0/src/modules/module-stream-restore.c)、[source.c](https://github.com/pulseaudio/pulseaudio/blob/v17.0/src/pulsecore/source.c) | v17.0，2024；访问 2026-10-12 | 5/5/4/5/5=24；不冒充本机精确快照 |
| S5 | [SourceOutputWidget](https://github.com/pulseaudio/pavucontrol/blob/27980e1fbe3d3c3fb36596a43a99c842dc60ebd6/src/sourceoutputwidget.cc) | 提交日期 2026-05-01 | 5/5/5/4/5=24；录音页移流入口 |
| S6 | [Settings](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/settings.html)、[Linking](https://pipewire.pages.freedesktop.org/wireplumber/policies/linking.html)、[Streams](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/stream.html) | WirePlumber 0.5.18；发布日期未知，访问 2026-10-12 | 5/5/5/5/5=25；不外推全部部署 |
| S7 | [pulse-server.c](https://github.com/PipeWire/pipewire/blob/1.6.8/src/modules/module-protocol-pulse/pulse-server.c)、[state-stream.lua](https://github.com/PipeWire/wireplumber/blob/0.5.18/src/scripts/node/state-stream.lua)、[Streams](https://docs.pipewire.org/page_streams.html) | 1.6.8/0.5.18；访问 2026-10-12 | 5/5/5/5/5=25；默认/移动/恢复实际实现 |
| S8 | [wpctl(1)](https://pipewire.pages.freedesktop.org/wireplumber/man/wpctl.html) | 0.5.18；发布日期未知，访问 2026-10-12 | 5/5/5/5/5=25；默认与持久化 |
| S9 | [PulseAudio](https://wiki.archlinux.org/title/PulseAudio)、[WirePlumber](https://wiki.archlinux.org/title/WirePlumber) | 动态 ArchWiki；访问 2026-10-12 | 4/4/5/5/4=22；独立运维辅助 |
| S10 | [instance.cpp](https://github.com/fcitx/fcitx5/blob/5.1.22/src/lib/fcitx/instance.cpp)、[inputcontext.cpp](https://github.com/fcitx/fcitx5/blob/5.1.22/src/lib/fcitx/inputcontext.cpp)、本机 SDK | Fcitx 5.1.22；访问 2026-10-12 | 5/5/5/5/5=25；精确版本事件时序 |
| S11 | [OpenAL Soft #892](https://github.com/kcat/openal-soft/issues/892) | 2023-08 至 2023-09 | 4/4/2/4/4=18；过期，仅用于设计反方 |
