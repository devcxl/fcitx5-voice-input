# 常见问题与排查 (FAQ)

本文汇总了使用 fcitx5-voice-input 过程中的常见问题与排查方法。

---

## 1. 输入法列表中找不到 Voice Input

### 现象
打开 `fcitx5-configtool` 后，在输入法添加列表中搜索不到 “Voice Input”。

### 排查步骤
1. **确认 Fcitx5 守护进程已重启**：
   安装新插件后必须重启 Fcitx5 才能重新加载模块：
   ```bash
   fcitx5 -r -d
   ```
2. **检查插件动态库安装位置**：
   插件文件 `voice-input-addon.so` 必须安装在 Fcitx5 的插件加载目录下（通常为 `/usr/lib/fcitx5/` 或 `/usr/lib/x86_64-linux-gnu/fcitx5/`）：
   ```bash
   find /usr/lib -name "voice-input-addon.so" 2>/dev/null
   ```
3. **检查取消勾选“仅显示当前语言”**：
   在 `fcitx5-configtool` 的输入法配置页中，取消勾选底部 “仅显示当前语言” 选项，再搜索 “Voice Input”。

---

## 2. 无法录音或说话无反应

### 现象
切换到 Voice Input 输入法后，无论说话多久均无任何反应，候选栏无提示且无法上屏。

### 排查步骤
1. **检查麦克风权限与音量**：
   在系统声音设置（如 `pavucontrol`）中确认麦克风未处于静音状态，并且输入音量处于合理水平（说话时能看到音量条跳动）。
2. **检查音频后端服务状态**：
   插件优先通过 PulseAudio 接口捕获音频（在 PipeWire 环境下依赖 `pipewire-pulse`），次选 PipeWire 原生直连。
   ```bash
   # 检查 pipewire-pulse 状态
   systemctl --user status pipewire-pulse
   # 或检查 pulseaudio 状态
   systemctl --user status pulseaudio
   ```
3. **检查音频设备选择**：
   插件启动时会自动遍历并优先选择前缀为 `alsa_input.*` 的真实麦克风硬件设备，自动忽略 Monitor 监听源与回声消除虚拟源。若使用了特殊虚拟麦克风，请在 `pavucontrol` 中将其设为默认输入设备（Default Source）。
4. **调节 VAD 检测灵敏度**：
   若麦克风拾音音量偏小，VAD 可能未能达到触发阈值。在 `fcitx5-configtool` 中将 `VADThreshold` 从默认的 `20` 调小至 `10` 进行测试。

---

## 3. ASR 识别失败与 API 错误排查

### 现象
说话结束后，候选栏提示识别失败，或者无文字上屏。

### 常见错误原因与解决方案

#### 1. 阿里云百炼 (DashScope) 报错 400
- **原因**：DashScope 的 `qwen3-asr-flash` 模型使用 Chat Completions 协议，而非 OpenAI 兼容的标准 Whisper 文件上传协议。
- **解决办法**：在 OpenAI 后端子配置中，务必将 `ApiMode` 设为 `chat`。

#### 2. OpenAI Realtime 模式报错
- **原因**：OpenAI 官方 Realtime 流式转录接口（如 `gpt-live-transcribe`）仅对付费 Tier 账号开放，免费层（Free Tier）账号调用会返回权限错误。
- **解决办法**：升级为 OpenAI 付费账户，或者切换 `ApiMode` 为标准的 `whisper` 模式。

#### 3. BaseUrl 地址格式问题
- **原因**：部分第三方兼容服务的基础 URL 路径不匹配。
- **解决办法**：确认 BaseUrl 是否包含 `/v1`（如 `https://api.openai.com/v1` 或 `https://api.siliconflow.cn/v1`）。注意不要在末尾添加多余的 `/`。

#### 4. 火山引擎豆包 ASR 鉴权失败
- **原因**：火山引擎的鉴权方式与控制台版本不匹配，或资源 ID (ResourceId) 填写错误。
- **解决办法**：
  - 新版控制台：使用 `AuthMode=api_key`，仅填写 API Key。
  - 旧版控制台：使用 `AuthMode=app_access_key`，填写 AppKey 和 AccessKey。
  - 确认 ResourceId 与控制台购买的套餐类型（大模型 2.0 / 1.0、按时长 / 按并发）完全一致。

---

## 4. 获取详细调试日志

当遇到难以定位的问题时，可以通过命令行以前台详细日志模式启动 Fcitx5：

```bash
# 关闭后台运行的 fcitx5 实例
killall fcitx5

# 以调试模式启动 fcitx5 并输出日志
fcitx5 -d --verbose default=5 2>&1 | grep -i voiceinput
```

在终端输出中可以直观观察到以下阶段的运行日志：
- 音频设备枚举与初始化（`Capture backend opened`）
- VAD 语音开始 (`Speech onset detected`) 与结束分段
- ASR 请求发送与 HTTP / WebSocket 返回状态
- 火山引擎请求的 `X-Tt-Logid`（可用于向火山引擎客服提交排查工单）
