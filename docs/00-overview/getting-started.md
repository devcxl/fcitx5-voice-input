# 入门指南

**fcitx5-voice-input** 是一个无缝集成到 [Fcitx5](https://fcitx-im.org/) 输入法框架中的语音输入插件。它通过本地轻量级 Silero VAD（语音活动检测）自动识别说话起止，并借助云端或自建 ASR（语音识别）服务将语音实时转写为文本上屏。

---

## 核心工作流程

无需像传统语音输入法那样长按快捷键，fcitx5-voice-input 采用全自动语音断句与管道流转设计：

```mermaid
flowchart LR
    A["切换至 Voice Input"] --> B["开始说话 (自动捕获)"]
    B --> C["VAD 检测语音起止"]
    C --> D["ASR 流式/分段转写"]
    D --> E["实时 Preedit 预览"]
    E --> F["静音判定结束 -> 自动上屏"]
```

1. **激活输入法**：通过快捷键（如 `Ctrl + Space` 或 `Super + Space`）将当前输入法切换至 **Voice Input**。
2. **自然说话**：麦克风自动捕获音频，VAD 算法实时判定人声并自动开启识别会话。
3. **实时预览与上屏**：
   - 使用流式后端（火山引擎、Mistral Realtime 或 OpenAI Realtime）时，候选框（Preedit）会实时跟随语音显示增量文字。
   - 停止说话达到设定的静音时长（默认 800ms）后，最终文本自动提交上屏（Commit）。
4. **连续输入**：无需切出输入法，继续说话即可直接开始下一句转写。

---

## 5 分钟快速上手

### 第一步：安装插件

根据您的 Linux 发行版选择最便捷的安装方式：

- **Arch Linux (AUR)**：
  ```bash
  yay -S fcitx5-voice-input
  ```
- **Ubuntu / Debian / Fedora / openSUSE**：
  前往 [GitHub Releases](https://github.com/devcxl/fcitx5-voice-input/releases) 下载对应发行版的 `.deb` 或 `.rpm` 安装包直接安装。

> 详见 [快速安装指南](/00-overview/installation)。

---

### 第二步：添加输入法

1. 重启 Fcitx5 守护进程以加载新安装的插件：
   ```bash
   fcitx5 -r -d
   ```
2. 打开 Fcitx5 配置工具：
   ```bash
   fcitx5-configtool
   ```
3. 在 **输入法 (Input Method)** 标签页中，取消勾选“仅显示当前语言”，在列表中搜索 **Voice Input**。
4. 将 **Voice Input** 添加到右侧的已启用输入法列表中。

---

### 第三步：配置 ASR 后端与 API Key

1. 在 `fcitx5-configtool` 中切换到 **附加组件 (Addon)** 标签页。
2. 找到 **VoiceInput** 插件，点击齿轮图标 ⚙ 打开配置界面。
3. 选择您使用的 ASR 后端（默认支持 OpenAI 兼容、火山引擎豆包、Mistral Realtime）：
   - **OpenAI 兼容后端**：填入 API Key，支持 OpenAI 官方、Groq、硅基流动 (SiliconFlow) 或阿里云百炼 DashScope。
   - **火山引擎后端**：填入 API Key（或 AppKey + AccessKey）以及购买的资源 ID。
4. 点击“应用”保存设置。

> 各后端的详细配置参数与示例请参考 [配置指南](/00-overview/configuration)。

---

### 第四步：体验语音输入

1. 打开任意文本编辑器（如 gedit、VS Code 或终端）。
2. 按快捷键切换到 **Voice Input** 输入法。
3. 对着麦克风清晰说出一句话，例如：*“今天天气真不错，测试一下语音输入插件。”*
4. 说话结束后稍作停顿，识别出的文字将自动上屏到当前光标位置！

---

## 实用技巧

### 1. 窗口切换保护
当您在说话间隙快速切换到其他窗口查阅资料时，插件内置了 200ms 的防抖延迟停止机制。若在 200ms 内切回原窗口，录音与转写管道将无缝保持，不会产生卡顿或重启。

### 2. 流式实时增量上屏
如果您希望在说话的同时实时在候选栏看到字词逐个蹦出，推荐使用以下流式模式：
- **OpenAI Realtime**：在 OpenAI 配置页将 `ApiMode` 设为 `realtime`，模型选用 `gpt-live-transcribe`。
- **火山引擎豆包**：配置 `ActiveBackend=volcengine`，自带低延迟实时流式识别与标点预测。
- **Mistral Realtime**：配置 `ActiveBackend=mistral`，模型使用 `voxtral-mini-transcribe-realtime-2602`。

### 3. LLM 智能后处理
在 OpenAI 后端配置中开启 `LLMEnabled` 并指定大语言模型（如 `gpt-4o-mini` 或 `qwen-plus`），可以为转写结果自动修正同音错字、规范中英文空格排版并补全标点符号。

---

## 下一步

- 查阅完整安装方法：[安装指南](/00-overview/installation)
- 调优 VAD 灵敏度与 ASR 参数：[配置指南](/00-overview/configuration)
- 遇到问题进行排查：[常见问题与排查](/00-overview/troubleshooting)
