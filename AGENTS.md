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

## 依赖（全部必需）

`fcitx5`（pkg-config 名 fcitx5 或 Fcitx5Core）、`pipewire-0.3`（libpipewire-0.3）、`libpulse-simple`、`jsoncpp`、`libcurl`（>= 7.86.0）、`zlib`、`onnxruntime`（Silero VAD）。

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
- **音频捕获后端**: 优先 PulseAudio（兼容 PulseAudio 和 pipewire-pulse），失败后 fallback 到 PipeWire 直连
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

GitHub Actions `build.yml`：Ubuntu 24.04 构建 + CPack DEB + Docker Arch 包。无测试步骤。

## 打包

- Arch: `aur/PKGBUILD`（依赖 fcitx5/pipewire/jsoncpp/curl/onnxruntime-cpu）
- DEB: CPack 自动生成
