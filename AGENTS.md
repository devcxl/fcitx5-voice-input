# AGENTS.md — fcitx5-voice-input

## 项目本质

Fcitx5 addon（共享库），多线程（主线程/Capture/VAD Worker/ASR Worker），无 daemon/CLI/Qt。

**LICENSE**: Apache 2.0。  
**默认 ASR 后端**: OpenAI 兼容 API（whisper-1），sherpa-onnx 可选。

## 构建命令

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DENABLE_SHERPA_ONNX=OFF -DENABLE_LLM_SUPPORT=OFF -DBUILD_TESTS=OFF
cmake --build build -j"$(nproc)"
```

选项：`ENABLE_SHERPA_ONNX`（pkg-config sherpa-onnx），`ENABLE_LLM_SUPPORT`（编译宏），`BUILD_TESTS`（目前无测试文件）。

## 依赖（全部必需）

`fcitx5`（pkg-config 名 fcitx5 或 Fcitx5Core）、`pipewire-0.3`（libpipewire-0.3）、`libpulse-simple`、`jsoncpp`、`libcurl`、`onnxruntime`（Silero VAD）。

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
│   ├── asr_engine.h       # 抽象接口（Start/FeedAudio/Stop）
│   ├── openai_asr.cpp/.h  # OpenAI 兼容 ASR（默认，HTTP multipart WAV）
│   ├── sherpa_asr.cpp/.h  # 本地 ASR（ENABLE_SHERPA_ONNX 编译）
└── utils/
    ├── audio_buffer.h     # Lock-free SPSC ring buffer（仅 PipeWire 内部使用）
    └── thread_safe_queue.h   # mutex + condition_variable 队列（Frame/Utterance/Result）
po/
└── zh_CN.po             # 中文翻译文件
```

## 关键约定

- **构建产物**: `voice-input-addon.so`（无 `lib` 前缀，`PREFIX ""`）
- **Addon 注册**: `FCITX_ADDON_FACTORY(VoiceInputAddonFactory)` — 必须在 `namespace fcitx` 外部
- **PipeWire 回调**: `on_process` 内 ≤100μs，只写 ring buffer，禁止阻塞/VAD/分配
- **音频捕获后端**: 优先 PulseAudio（兼容 PulseAudio 和 pipewire-pulse），失败后 fallback 到 PipeWire 直连
- **PipeWire**: on_process→ringbuffer(float32)→DrainLoop thread→int16 AudioFrame→FrameQueue
- **Ring buffer**: `Clear()` 被故意省略（与 PipeWire 回调 data race），清空用 `Read()` drain 模式
- **音频格式统一**: 16kHz mono, int16, 512 samples/window (32ms)
- **VAD**: 仅 Silero ONNX, predict() 返回 0~1 概率, Idle/Speaking 状态机
- **Pipeline 管道**: FrameQueue → VADWorker → UtteranceQueue → ASRWorker → ResultQueue → eventDispatcher → 主线程
- **Config 热加载**: `setConfig()` → `voiceinput.conf` 保存 + `pipeline_->SetConfig()`
- **交互方式**: 切换到 Voice Input 即启动 pipeline；VAD 检测到人声分段，静音后提交 ASR；主线程 eventDispatcher 接收结果 commit

## 与 ARCHITECTURE.md 的差距

ARCHITECTURE.md 是超前设计文档，实际代码已偏离：
- 文件名不匹配（`engine.cpp` ≠ `voice_input_engine.cpp` 等）
- 实际路线：OpenAI 兼容 API 默认，非本地 sherpa-onnx 优先
- VAD：Silero ONNX 独占，已删除 libfvad fallback
- 架构：FrameQueue → VADWorker → UtteranceQueue → ASRWorker → ResultQueue → 主线程
- 无独立 CaptureLoop（VAD Worker 合并），无 Pipeline 状态机
- `tests/`、`data/`、`models/`、`docs/`、`packaging/` 目录尚不存在
- 无高级 JSON 配置层（`config_manager` 未实现）
- 架构图中"模型分包"方案未落地

## CI

GitHub Actions `build.yml`：Ubuntu 24.04 构建 + CPack DEB + Docker Arch 包。无测试步骤。

## 打包

- Arch: `aur/PKGBUILD`（依赖 fcitx5/pipewire/jsoncpp/curl/onnxruntime-cpu；sherpa-onnx 可选）
- DEB: CPack 自动生成
