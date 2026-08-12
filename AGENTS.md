# AGENTS.md — fcitx5-voice-input

## 项目本质

Fcitx5 addon（共享库），多线程（主线程/Capture/VAD Worker/ASR Worker），无 daemon/CLI/Qt。

**LICENSE**: LGPL v3。  
**默认 ASR 后端**: OpenAI 兼容 API（whisper-1），预留 AsrEngine 抽象接口。

## 构建命令

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_TESTS=OFF
cmake --build build -j"$(nproc)"
```

选项：`BUILD_TESTS`（目前无测试文件）。

## 依赖

必需：`fcitx5`（pkg-config 名 fcitx5 或 Fcitx5Core）、`jsoncpp`、`libcurl`（>= 7.86.0）、`zlib`、`onnxruntime`（Silero VAD）。
可选（至少其一）：`pipewire-0.3`（libpipewire-0.3）、`libpulse-simple`。缺少任一录音后端时仅失去对应 capture 后端，addon 仍可构建；两个都缺则 CMake 报错。**运行时**两个库均为 dlopen 延迟加载（无链接期依赖），库升级/soname 变更不影响已安装 addon。

克隆后需执行：`git submodule update --init --recursive`。

## 代码结构

```
src/addon/
├── engine.cpp/.h          # Fcitx5 InputMethodEngineV2 入口
├── types.h                # AudioFrame/Utterance/AsrResult 类型定义
├── voiceinput.conf.in     # addon 配置模板（@PROJECT_VERSION@ 替换）
├── config/
│   └── voiceinput-config.h   # FCITX_CONFIGURATION 宏定义配置键
├── capture/audio_capture.h      # 音频捕获抽象接口
├── capture/pulse_audio_capture.cpp/.h  # PulseAudio 音频捕获（优先，直推 FrameQueue）
├── capture/pipewire_capture.cpp/.h  # PipeWire 音频捕获（fallback, ringbuffer+drain thread）
├── vad/silero_vad.cpp/.h   # Silero ONNX 封装（int16 输入, predict() 返回概率）
├── vad/vad.cpp/.h         # VADWorker（Idle/Speaking 状态机, pre-roll, 队列消费/生产）
├── pipeline/pipeline.cpp/.h   # 管道编排（FrameQueue/UtteranceQueue/ResultQueue + 3 worker 线程）
├── asr/
│   ├── asr_engine.h       # 抽象接口（Start/FeedAudio/Stop，可扩展本地 ASR）
│   └── openai_asr.cpp/.h  # OpenAI 兼容 ASR（默认，HTTP multipart WAV）
└── utils/
    ├── audio_buffer.h     # Lock-free SPSC ring buffer（仅 PipeWire 内部使用）
    └── thread_safe_queue.h   # mutex + condition_variable 队列（Frame/Utterance/Result）
po/
└── zh_CN.po             # 中文翻译文件
```

## 关键约定

- **构建产物**: `voice-input-addon.so`（无 `lib` 前缀，`PREFIX ""`）
- **Addon 注册**: `FCITX_ADDON_FACTORY(VoiceInputAddonFactory)` — 必须在 `namespace fcitx` 外部
- **禁止安装**: 除非用户明确要求 `cmake --install`，否则只构建不安装。勿动 `/usr`、`~/.local` 等路径。
- **PipeWire 回调**: `on_process` 内 ≤100μs，只写 ring buffer，禁止阻塞/VAD/分配
- **音频捕获后端**: 可选编译（CMake 宏 `HAVE_PULSEAUDIO`/`HAVE_PIPEWIRE`），优先 PulseAudio（兼容 PulseAudio 和 pipewire-pulse），失败后 fallback 到 PipeWire 直连；仅编译进来的后端可用
- **录音库运行期加载（dlopen + dlsym 函数指针表）**: libpulse-simple/libpipewire **不链接**（无 DT_NEEDED、无未定义符号），各 capture 的 `LoadLib()` 在 Start 时 dlopen（RTLD_NOW|RTLD_GLOBAL，候选 soname 列表）并逐个 dlsym 填充函数指针表，所有 `pa_*`/`pw_*` 调用经函数指针间接调用。库缺失/soname 变更/符号不完整时后端优雅降级，addon 本体不受影响。兼容 Arch 构建的 `-z,now`（DF_1_NOW 强制立即绑定也无不解析符号，见 PR #20）
- **PipeWire**: on_process→ringbuffer(float32)→DrainLoop thread→int16 AudioFrame→FrameQueue
- **Ring buffer**: `Clear()` 被故意省略（与 PipeWire 回调 data race），清空用 `Read()` drain 模式
- **音频格式统一**: 16kHz mono, int16, 512 samples/window (32ms)
- **VAD**: 仅 Silero ONNX, predict() 返回 0~1 概率, Idle/Speaking 状态机
- **Pipeline 管道**: FrameQueue → VADWorker → UtteranceQueue → ASRWorker → ResultQueue → eventDispatcher → 主线程
- **Config 热加载**: `setConfig()` → `voiceinput.conf` 保存 + `pipeline_->SetConfig()`
- **交互方式**: 切换到 Voice Input 即启动 pipeline；VAD 检测到人声分段，静音后提交 ASR；主线程 eventDispatcher 接收结果 commit

## 与 ARCHITECTURE.md 的关系

ARCHITECTURE.md 已与代码同步。提到的超前功能（Command 引擎、LLM 后处理、场景系统、单元测试、多发行版打包）均在 Route Map 中标记为待实现，非代码与文档的不一致。

## CI

GitHub Actions（详见 `.github/workflows/` 与 `.github/actions/build/distro/*.sh`）：

- `ci.yml`：PR/push 触发。**7 发行版容器矩阵**（ubuntu-24.04 / ubuntu-26.04 /
debian-12 / debian-13 / fedora-44 / opensuse-tumbleweed / archlinux），每发行版在
原生容器内构建原生包（DEB/RPM/pkg.tar.zst）；另含链接校验（nm/readelf/dlopen
smoke，仅 verify job 跑一次）与 build-no-pipewire 回归防护 job。
- `release.yml`：tag `v*` 触发。复用同一构建矩阵，`softprops/action-gh-release`
  v3 生成 draft release（含全部发行版产物 + AUR 源码包）。
- onnxruntime 双策略：`system`（发行版系统包，DEB 开 dpkg-shlibdeps 自动依赖，
  RPM 由 rpmbuild 自动依赖）或 `download`（upstream release 1.28.0，缓存加速）。
- Arch 包：archlinux 容器内直跑 makepkg（无 Docker daemon）。

无测试步骤。

## 打包

- Arch: `aur/PKGBUILD`（依赖 fcitx5/pipewire/jsoncpp/curl/onnxruntime-cpu）
- DEB: CPack 自动生成
