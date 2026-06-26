# fcitx5-voice-input 架构设计

> **状态:** 当前实现说明 + 早期设计留存
> **设计原则:** 砍掉一切不需要的组件，单 addon 搞定
> **当前交互:** 切换到 Voice Input 后自动监听人声；无快捷键触发录音。

---

## 设计目标

1. **无独立进程（daemon）** — 全部逻辑跑在 Fcitx5 addon 内部，线程隔离
2. **无 CLI 二进制** — 配置靠 fcitx5-configtool + 直接编辑 JSON 文件
3. **无 Qt GUI 依赖** — 不引入 Qt，避免 50MB+ 的依赖膨胀
4. **模型分包** — ASR 模型通过包管理器分发，严禁应用内下载
5. **本地优先** — 默认 sherpa-onnx 离线识别，云 ASR 作为可选扩展
6. **最小依赖** — Fcitx5 + PipeWire + sherpa-onnx 即可运行

---

## 一句话架构

> **一个 Fcitx5 Addon（共享库），内部三个线程。**

```
Fcitx5 进程 (voice-input-addon.so)
├── [主线] 输入法激活/停用、状态显示、上屏
├── [音频线] PipeWire 捕获 → ring buffer → VAD 分段 → 音频缓冲区
└── [ASR线] OpenAI 兼容 API / sherpa-onnx → 回调主线程上屏（非阻塞）
```

没有 daemon、没有 D-Bus、没有 CLI 二进制、没有 Qt GUI。

---

## 为什么可以砍掉 daemon？

### 传统方案（fcitx5-vinput）的选择

```
Fcitx5 Addon ← D-Bus IPC → vinput-daemon (独立进程)
                              ├── PipeWire 音频
                              └── sherpa-onnx 推理
```

理由是："ASR 推理会卡 UI，放另一个进程里安全。"

### 实际问题

ASR 推理是 CPU 密集型操作，在**同一进程的另一个线程**里跑和在**另一个进程**里跑，对 UI 响应的影响是完全一样的——都不阻塞主线程。区别只有：

| 维度 | 独立进程 | 独立线程 |
|------|---------|---------|
| 隔离性 | 更强（崩溃不影响 Fcitx5） | 弱一些（线程崩溃拖整个进程） |
| 复杂度 | ❌ systemd 管理、D-Bus 定义、进程通信 | ✅ 零额外开销 |
| 模型加载 | 重复加载（addon 一份、daemon 一份） | ✅ 共享内存 |
| 延迟 | ❌ 加一次 D-Bus 序列化+反序列化 | ✅ 直接内存访问 |
| 用户操作 | ❌ `systemctl --user start vinput-daemon` | ✅ 装好即用 |
| 调试 | ❌ 跨进程追踪困难 | ✅ 单进程 GDB 一把梭 |

**结论：** 对于输入法这种场景，线程隔离的收益远大于进程隔离的成本。sherpa-onnx 本身很稳定，如果真担心线程崩溃，用 `std::async` + 超时兜底就够了。

---

## 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     Fcitx5 进程                              │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              libvoice-input-engine.so                      │   │
│  │                                                      │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │           主线程 (Fcitx5 事件循环)            │     │   │
│  │  │  ├── activate() → StartListening()            │     │   │
│  │  │  ├── deactivate() → StopListening()           │     │   │
│  │  │  ├── 结果到达 → commitString()                      │     │   │
│  │  │  └── 状态同步 → 显示"语音模式/录音中/转录中"   │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ 线程安全队列                  │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │          音频捕获线程 (Capture Thread)        │     │   │
│  │  │  ├── pw_thread_loop 运行                    │     │   │
│  │  │  ├── on_process 回调 → 仅写入环形缓冲区      │     │   │
│  │  │  └── 循环中从环形缓冲区读取 → VAD 检测       │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  │                         ↕ 音频缓冲区 + 事件信号        │   │
│  │  ┌─────────────────────────────────────────────┐     │   │
│  │  │            ASR 推理线程 (ASR Thread)          │     │   │
│  │  │  ├── 默认 OpenAI 兼容 API，sherpa-onnx 可选    │     │   │
│  │  │  ├── 收到 VAD 分段完成音频 → ASR               │     │   │
│  │  │  ├── 结果 → 回调主线程上屏                    │     │   │
│  │  │  └── 切换模型时后台热加载                     │     │   │
│  │  └─────────────────────────────────────────────┘     │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │           fcitx5-configtool 配置界面                  │   │
│  │  ├── ASR Provider: [openai ▼]                        │   │
│  │  ├── Model: [sensevoice      ▼] ← 自动扫描模型目录    │   │
│  │  └── Scene: [原文           ▼]                       │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 组件详述

### 1. 输入法引擎 (`src/addon/`)

```cpp
// 简化的接口定义
class VoiceInputEngine : public fcitx::InputMethodEngineV2 {
    // Fcitx5 生命周期
    void save() override;     // 配置变更时保存
    void reloadConfig() override;  // 重载配置（切换模型等）
    
    // 事件处理（InputMethodEngineV2 接口）：当前不消费快捷键
    void keyEvent(
        const fcitx::InputMethodEntry& entry,
        fcitx::KeyEvent& keyEvent
    ) override;
    
    // 线程管理
    void startCaptureThread();
    void startAsrThread();
    void stopThreads();        // addon 卸载时
    
    // 结果回调（主线程）
    void onRecognitionResult(const std::string& text);
    
private:
    CaptureThread captureThread_;   // PipeWire 音频捕获
    AsrThread asrThread_;           // sherpa-onnx 推理
    
    ThreadSafeQueue<AudioBuffer> audioQueue_;  // 线程安全队列
    std::atomic<State> state_;     // idle / recording / recognizing
};
```

**文件结构：**

```
src/addon/
├── CMakeLists.txt
├── voice_input_engine.cpp          # Fcitx5 InputMethodEngine 入口
├── voice_input_engine.h
├── voice-input-addon.conf          # Fcitx5 addon 配置描述
├── capture/
│   ├── pipewire_capture.cpp   # PipeWire 音频流捕获
│   ├── pipewire_capture.h
│   ├── vad.cpp                # VAD (Voice Activity Detection)
│   └── vad.h
├── asr/
│   ├── engine.h               # ASR 引擎抽象接口
│   ├── sherpa_engine.cpp      # sherpa-onnx 引擎实现
│   ├── sherpa_engine.h
│   ├── command_engine.cpp     # 外部命令引擎（云 ASR 扩展）
│   └── command_engine.h
├── config/
│   ├── config_manager.cpp     # JSON 配置读写
│   └── config_manager.h
├── pipeline/
│   ├── pipeline.cpp            # 录音→ASR→上屏流水线
│   └── pipeline.h
└── utils/
    ├── thread_safe_queue.h     # 线程安全队列
    └── audio_buffer.h          # 音频环形缓冲区
```

### 2. 配置 (`src/addon/config/`)

**配置分层：**

| 层级 | 存储方式 | 编辑方式 | 内容 |
|------|---------|---------|------|
| 基本配置 | fcitx5 Addon config | fcitx5-configtool | ASR 后端、API、VAD、场景切换 |
| 高级配置 | `~/.config/fcitx5/voice-input/config.json` | 直接文件编辑 | LLM Provider、自定义场景 Prompt |
| 运行时 | 内存 | D-Bus/内部 API | 录音状态、识别进度 |

**Fcitx5 配置键定义（voice-input-addon.conf）：**

```ini
[Addon]
Name=fcitx5-voice-input
Category=InputMethod
Configurable=true

[Config/ASRBackend]
Type=String
DefaultValue=openai
Description=ASR 后端（openai 或 sherpa-onnx）

[Config/OpenAIEndpoint]
Type=String
DefaultValue=https://api.openai.com/v1
Description=OpenAI 兼容 API Endpoint

[Config/Scene]
Type=Enum
DefaultValue=__raw__
Description=后处理场景

[Config/CaptureDevice]
Type=String
DefaultValue=default
Description=音频输入设备（PulseAudio/PipeWire 设备名）
```

**高级 JSON 配置（`~/.config/fcitx5/voice-input/config.json`）：**

```json
{
  "version": 1,
  "asr": {
    "timeout_ms": 15000,
    "vad": { "enabled": true }
  },
  "audio": {
    "device": "default",
    "gain": 1.0,
    "sample_rate": 16000
  },
  "scene": {
    "active": "__raw__",
    "definitions": [
      {
        "id": "__raw__",
        "label": "原文",
        "candidate_count": 0
      },
      {
        "id": "__correct__",
        "label": "纠错",
        "prompt": "修正语音识别结果中的错误，只输出修正后的文本。"
      }
    ]
  },
  "llm": {
    "providers": [],
    "adapters": []
  }
}
```

**为什么费城分开两层？**
- 需要 fcitx5-configtool 可视化的：按键绑定、模型选择（下拉框）
- 需要复杂结构的：场景 Prompt、LLM 配置、自定义 Provider（不适合通用 key-value 表单）

### 3. 音频捕获 (`src/addon/capture/`)

```cpp
// PipeWire 音频流捕获
class AudioCapture {
public:
    void Start(const std::string& device);
    void Stop();
    
private:
    pw_thread_loop* loop_;
    AudioRingBuffer* ring_buffer_;  // 外部传入，lock-free
};
```

**PipeWire 线程规则：**
- `pw_thread_loop` 创建专用线程运行主循环
- `on_process` 回调在 loop 锁内执行，持锁期间 loop 停住
- 回调内（≤100μs 原则）只做：PCM frame → lock-free ring buffer → 返回
- ❌ 回调内禁止：大内存分配、JSON 序列化、ASR 推理、复杂 VAD、`std::mutex` 加锁、Fcitx UI 调用
- VAD 检测在回调*外*完成：捕获线程从 ring buffer 读取批量帧后做 VAD 分析

**文件结构：**

```
src/addon/capture/
├── pipewire_capture.cpp   # PipeWire 音频流捕获（pw_thread_loop 封装）
├── pipewire_capture.h
├── vad.cpp                # VAD 处理（从 ring buffer 读取后调用）
└── vad.h
```

### 4. 音频流水线 (`src/addon/pipeline/`)

状态机：

```
IDLE
  │  输入法 activate()
  ▼
LISTENING
  │  VAD 检测到人声
  ▼
RECORDING
  │  VAD 静音超时 / 最大录音时长
  ▼
PROCESSING_ASR ───→ ASR 完成 ───→ 上屏 ───→ LISTENING
  │  LLM 后处理（可选）
  ▼
PROCESSING_LLM ───→ 最终结果上屏 → LISTENING
```

> ⚠️ 状态切换是瞬时的，主线程不等待 ASR 结果：
> 1. VAD 静音超时 → 捕获线程切换 `RECORDING → PROCESSING_ASR`
> 2. 捕获线程将本段音频投递到 ASR 引擎 → **立即返回**
> 3. ASR 线程完成推理后，通过 `eventLoop().addDeferredEvent()` 回调主线程
> 4. 回调内主线程执行 `commitString()` → 切换回 `LISTENING`

VAD 处理逻辑：

```
┌─ PipeWire on_process 回调 ──────────────────┐
│  PCM frame → lock-free ring buffer → return │  ← ≤100μs
└─────────────────────────────────────────────┘
                      │
捕获线程主循环（pw_thread_loop 解锁后）
                      │
          ┌───────────┴───────────┐
          ▼                       ▼
  从 ring buffer          VAD 检测 + 裁剪
  读取批量帧             标记活动/静音段
          │                       │
          └───────────┬───────────┘
                      ▼
         静音 ≥ 500ms → 触发 VAD 结束
         活动段 → 拷贝到 ASR 输入缓冲区
```

**音频回调约束（详见 §3 音频捕获）：**
- `on_process` 内只做 PCM → ring buffer，持锁期间禁止 VAD / ASR / 分配大对象
- VAD 检测在回调外完成，从 ring buffer 读取数据后处理

### 5. ASR 引擎 (`src/addon/asr/`)

```cpp
// 引擎接口
class AsrEngine {
public:
    virtual ~AsrEngine() = default;
    
    // 引擎生命周期
    virtual bool Initialize(const std::string& modelPath) = 0;
    virtual void Destroy() = 0;
    virtual bool ReloadModel(const std::string& modelPath) = 0;
    
    // 推理接口（非阻塞，结果通过回调）
    virtual void StartRecognition(const AudioBuffer& audio) = 0;
    virtual void CancelRecognition() = 0;
    
    // 检查模型是否可用
    static bool ProbeModel(const std::filesystem::path& modelDir);
    
    // 回调
    std::function<void(const std::string& text)> OnResult;
    std::function<void(const std::string& error)> OnError;
};
```

**引擎实现：**

| 引擎 | 类名 | 依赖 | 说明 |
|------|------|------|------|
| sherpa-onnx 离线 | `SherpaOfflineEngine` | libsherpa-onnx | 默认引擎，推荐 |
| sherpa-onnx 流式 | `SherpaStreamingEngine` | libsherpa-onnx | 低延迟，支持流式显示 |
| 外部命令 | `CommandEngine` | curl/python3 | 云 ASR 扩展（Doubao/OpenAI） |

**外部命令引擎（云 ASR 扩展）：**

```cpp
class CommandEngine : public AsrEngine {
    // 执行外部进程，音频通过 stdin 传入
    // 结果从 stdout 读取
    // 命令和参数从高级配置中读取
    // 用于对接 doubao、aliyun、openai 等云 ASR
};
```

---

## 模型分包方案

### 安装路径

```
/usr/share/fcitx5-voice-input/models/
└── {model-id}/
    ├── model.onnx              # 主模型
    ├── tokens.txt              # 词表
    └── metadata.json           # 元信息
```

### metadata.json

```json
{
  "id": "sherpa-onnx-sense-voice-zh-2024-07-17",
  "name": "SenseVoice 中文",
  "lang": "zh",
  "size_bytes": 31457280,
  "type": "offline",
  "sha256": "a1b2c3...",
  "engine": "sherpa-onnx",
  "homepage": "https://github.com/k2-fsa/sherpa-onnx"
}
```

### 扫描与发现

Addon 启动时扫描模型目录：

```cpp
std::vector<ModelInfo> ScanModels() {
    std::vector<ModelInfo> models;
    const auto modelDir = std::filesystem::path("/usr/share/fcitx5-voice-input/models/");
    
    if (!exists(modelDir)) return models;
    
    for (const auto& entry : directory_iterator(modelDir)) {
        if (!entry.is_directory()) continue;
        
        auto metaPath = entry.path() / "metadata.json";
        if (!exists(metaPath)) continue;
        
        ModelInfo info = ParseMetadata(metaPath);
        if (VerifyModelFiles(entry.path(), info)) {
            models.push_back(std::move(info));
        }
    }
    return models;
}
```

扫描结果自动填充到 fcitx5-configtool 的 Model 下拉框中。切换模型时，addon 热加载新模型（ASR 线程安全切换）。

### 包管理

**Arch Linux (PKGBUILD split package)：**

```bash
# 主包
package_fcitx5-voice-input() {
  pkgdesc="Voice input addon for Fcitx5"
  depends=('fcitx5' 'pipewire' 'sherpa-onnx')
  conflicts=('fcitx5-vinput')
}

# 模型分包
package_fcitx5-voice-input-model-sensevoice() {
  pkgdesc="SenseVoice Chinese model for fcitx5-voice-input"
  depends=('fcitx5-voice-input')
  install -Dm644 model.onnx \
    "$pkgdir/usr/share/fcitx5-voice-input/models/$_model_id/model.onnx"
  install -Dm644 tokens.txt \
    "$pkgdir/usr/share/fcitx5-voice-input/models/$_model_id/tokens.txt"
  install -Dm644 metadata.json \
    "$pkgdir/usr/share/fcitx5-voice-input/models/$_model_id/metadata.json"
}
```

**Debian/Ubuntu (control 多 binary)：**

```
Source: fcitx5-voice-input
Package: fcitx5-voice-input
Depends: fcitx5, pipewire, libsherpa-onnx

Package: fcitx5-voice-input-model-sensevoice
Depends: fcitx5-voice-input
Description: SenseVoice Chinese model for fcitx5-voice-input
```

**模型包管线：**

```
sherpa-onnx 发布新模型
    │  GitHub Actions 自动脚本
    ▼
构建模型包 (.pkg.tar.zst / .deb / .rpm)
    │  上传到 GitHub Releases
    ▼
用户: sudo pacman -S fcitx5-voice-input-model-xxx
     (或 apt install / dnf install)
```

模型包的构建在 `fcitx5-voice-input` 仓库的 `models/` 目录下自动化。

---

## Fcitx5 集成细节

### 录音状态指示

Fcitx5 候选词区显示状态：

```
┌─────────────────────────────────────┐
│  🎤 录音中...                        │  ← 录音时
│  🤖 识别中...                        │  ← ASR 推理时
│  你好世界                            │  ← 识别完成，自动上屏
└─────────────────────────────────────┘
```

通过 `InputContext::updateUserInterface(UserInterfaceComponent::InputPanel)` 实现。

### 输入法激活与按键处理

```cpp
void VoiceInputEngine::activate(
    const fcitx::InputMethodEntry& entry,
    fcitx::InputContextEvent& event
) {
    activeIc_ = event.inputContext();
    pipeline_.StartListening();
}

void VoiceInputEngine::deactivate(
    const fcitx::InputMethodEntry& entry,
    fcitx::InputContextEvent& event
) {
    pipeline_.StopListening();
    activeIc_ = nullptr;
}

void VoiceInputEngine::keyEvent(
    const fcitx::InputMethodEntry& entry,
    fcitx::KeyEvent& keyEvent
) {
    // 不再使用快捷键触发录音；普通按键不被过滤。
}
```

### 配置变更热加载

```cpp
void VoiceInputEngine::reloadConfig() override {
    // 1. 读取 Fcitx5 配置键
    readFcitxConfig(config_);
    
    // 2. 读取高级 JSON 配置
    config_manager_.LoadAdvancedConfig();
    
    // 3. 如果模型变了，后台热加载
    if (config_.model != currentModel_) {
        asrThread_.ReloadModel(config_.model);
        currentModel_ = config_.model;
    }
}
```

---

## 线程安全模型

```
主线程 (Fcitx5 事件循环)
    │  写入: startRecording / stopRecording
    │  读取: onResult / onError (回调)
    ▼
┌─────────────────────────────────────┐
│       ThreadSafeCommand              │
│  - std::atomic<State> state          │
│  - std::mutex + condition_variable   │
│  - std::function<void()> callbacks   │
└─────────────────────────────────────┘
    ▲         ↕ audio buffer (lock-free ring buffer)
    │
音频捕获线程                     ASR 推理线程
PipeWire → ring buffer           sherpa-onnx
  → VAD (外循环)                     ↓
audio ring buffer               text result
```

### 关键线程安全策略

1. **状态同步** — `std::atomic<State>`（idle / recording / processing）
2. **音频传递** — Lock-free SPSC ring buffer（C++20 `atomic_ref` + `memory_order`）
3. **结果回传** — `std::function` + Fcitx5 的事件循环调度（`eventLoop().addDeferredEvent()`）
4. **模型切换** — ASR 线程空闲时执行切换，切换期间录音缓存到队列，切完再处理
5. **竞态终极保障** — 所有对外暴露的接口都走 `std::lock_guard<std::mutex>`

---

## 错误处理策略

| 场景 | 行为 |
|------|------|
| 无模型包安装 | fcitx5-configtool 里 Model 下拉框为空，显示提示文字"请安装模型包" |
| 模型文件损坏 | 启动时报错，fallback 到上一个可用的模型 |
| PipeWire 断开 | 自动重连，记录日志，用户界面显示"🎤 等待音频设备" |
| ASR 超时（>15s） | 丢弃当前音频，回到 IDLE 状态 |
| ASR 线程崩溃 | `std::async` 超时检测，重启 ASR 线程（限制 3 次/分钟） |
| 配置格式错误 | 加载默认值，忽略错误字段，日志警告 |

---

## 构建与打包

### 依赖关系图

```
fcitx5-voice-input
├── fcitx5                      # 输入法框架（核心）
├── pipewire                    # 音频捕获
├── sherpa-onnx                 # 本地 ASR 引擎
├── nlohmann-json               # JSON 配置解析
├── libcurl (可选)               # LLM 后处理 HTTP 请求
└── openssl (可选)               # WebSocket 加密
```

### 构建系统

```bash
# CMake 配置示例
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LLM_SUPPORT=ON \     # 启用 LLM 后处理
    -DENABLE_CLOUD_ASR=ON         # 启用云 ASR（command provider）
cmake --build build
cmake --install build --prefix /usr
```

### 构建输出

```
build/
└── lib/
    └── fcitx5/
        └── libvoice-input-engine.so    # 唯一的输出产物
```

---

## 项目目录结构

```
fcitx5-voice-input/
├── ARCHITECTURE.md               # ← 本文档
├── README.md
├── LICENSE
├── CMakeLists.txt                # 顶配 CMake
├── CMakePresets.json
│
├── src/
│   └── addon/                    # 唯一的代码目录
│       ├── CMakeLists.txt
│       ├── voice_input_engine.cpp
│       ├── voice_input_engine.h
│       ├── voice-input-addon.conf.in  # Fcitx5 addon 配置模板
│       ├── capture/
│       │   ├── pipewire_capture.cpp
│       │   ├── pipewire_capture.h
│       │   ├── vad.cpp
│       │   └── vad.h
│       ├── asr/
│       │   ├── engine.h
│       │   ├── sherpa_engine.cpp
│       │   ├── sherpa_engine.h
│       │   ├── command_engine.cpp
│       │   └── command_engine.h
│       ├── config/
│       │   ├── config_manager.cpp
│       │   └── config_manager.h
│       ├── pipeline/
│       │   ├── pipeline.cpp
│       │   └── pipeline.h
│       └── utils/
│           ├── thread_safe_queue.h
│           └── audio_buffer.h
│
├── models/                       # 模型分包构建脚本
│   ├── PKGBUILD.template         # Arch 分包模板
│   ├── debian/
│   │   ├── control.template      # Debian 多 binary 模板
│   │   └── rules
│   └── build-models.sh           # 自动构建所有模型包
│
├── data/
│   ├── default-config.json       # 默认高级配置
│   └── voice-input-addon.conf         # Fcitx5 配置模板
│
├── cmake/
│   ├── FindPipeWire.cmake
│   ├── FindSherpaOnnx.cmake
│   └── FindFcitx5.cmake
│
├── tests/
│   ├── CMakeLists.txt
│   ├── test_vad.cpp              # VAD 单元测试
│   ├── test_audio_buffer.cpp     # 环形缓冲区测试
│   ├── test_config.cpp           # 配置解析测试
│   └── test_pipeline.cpp         # 流水线集成测试
│
├── docs/
│   └── usage.md                  # 用户文档
│
├── .github/
│   └── workflows/
│       ├── build.yml             # CI 构建验证
│       └── build-models.yml      # 模型分包自动构建发布
│
├── packaging/
│   ├── arch/
│   │   └── PKGBUILD             # Arch 主包
│   ├── debian/
│   │   └── control              # Debian 主包
│   └── fedora/
│       └── fcitx5-voice-input.spec
│
└── systemd/                      # （留空，因为已放弃 daemon）
    └── README.md                 # 解释为什么不需要 systemd
```

---

## 与 fcitx5-vinput 对比总结

| 项目 | fcitx5-vinput | fcitx5-voice-input |
|------|---------------|-------------------|
| **架构** | addon + daemon + CLI + GUI | ✅ 单一 addon |
| **进程数** | 3（Fcitx5 + daemon + GUI） | ✅ 1（仅在 Fcitx5 内） |
| **IPC** | D-Bus 全流程 | ✅ 线程同步（零拷贝） |
| **依赖** | Fcitx5 + PipeWire + Qt6 + libcurl + ... | ✅ Fcitx5 + PipeWire + sherpa-onnx |
| **模型分发** | 应用内 registry 下载 | ✅ 包管理器分包 |
| **配置** | CLI + GUI + 配置文件 | ✅ fcitx5-configtool + JSON 文件 |
| **用户操作** | 安装 → 启 daemon → 开 GUI → 下载模型 → 配置 | ✅ 安装 → 切输入法 → 说话 |
| **构建产物** | 4 个二进制 | ✅ 1 个 .so |
| **代码量估计** | ~1.5 万行 | ~8000 行（去重后约 4000 行有效） |

---

## 路线图

### Phase 1: 核心（2-3 天）⚡
- [ ] CMake 构建框架 + Fcitx5 Addon 骨架
- [ ] PipeWire 音频捕获线程
- [ ] VAD 裁剪
- [ ] sherpa-onnx 推理引擎（离线）
- [ ] 录音→ASR→上屏完整流水线
- [ ] 模型扫描 + fcitx5-configtool 下拉框
- [ ] 最小可用版本

### Phase 2: 扩展（1-2 天）🔌
- [ ] 模型分包构建脚本（Arch PKGBUILD）
- [ ] LLM 后处理引擎（libcurl HTTP 调用）
- [ ] 场景系统 — 对语音结果做纠错/翻译/格式化

### Phase 3: 打磨 ✨
- [ ] 多发行版打包（Debian、Fedora）
- [ ] 云 ASR Provider（command engine）
- [ ] 流式 sherpa-onnx（边录边上屏）
- [ ] 热词优化
- [ ] CI 自动构建模型包

---

## FAQ

### Q: Fcitx5 线程崩溃不就输入法没了？
A: 用了 `std::async` + 超时兜底。ASR 线程按 `std::launch::async` 启动，用 `future.wait_for(timeout)` 监控。如果超时或异常，吞掉错误，线程标记为"故障"，告警而不是崩溃。故障状态下按键变成普通按键，不影响打字。

### Q: 主线程不是还卡？
A: 主线程只负责事件分发和上屏。PipeWire 捕获（实时音频）和 ASR 推理（CPU 密集型）都不在主线程。唯一的"卡"是 LLM 后处理，但那也是异步在 ASR 线程完成的，结果回来主线程只做一个 `commitString()`。

### Q: 多个输入法实例呢？
A: Fcitx5 插件的实例是 lazy 创建的，通常只创建一个实例。每个实例有独立的 Pipeline，线程安全和实例生命周期绑定。多输入法场景（比如同时开两个窗口）也一样，共享同一个 addon 实例。

### Q: 我这打包脚本怎么自动构建模型包？
A: 在 `models/` 目录下放一个 `source-models.yaml` 或一个 JSON 列表，记录每个模型的下载链接和 SHA256。CI 每天检查上游 sherpa-onnx 是否更新了模型版本，有更新就自动构建新版本的模型包并上传到 GitHub Releases。

---

*—— 架构设计 v2，去掉了所有不必要的组件，只保留一个 Fcitx5 Addon。*
