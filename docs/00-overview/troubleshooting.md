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
3. **检查录音设备路由**：
   插件不再枚举、筛选或指定录音设备，路由由系统音频服务与音频管理器负责。先确认系统默认输入是未静音、电平有反应的设备；若默认正确仍无声，需排查是否为本应用保存了历史设备偏好，或 Fcitx 启动环境设置了 `PULSE_SOURCE` / `client.conf` 源覆盖。可查看插件当前使用的流：
   ```bash
   pactl info
   pactl get-default-source
   pactl list source-outputs   # 在录音页/这里找到本插件对应的 index
   ```
   在 `pavucontrol` 的「录音（Recording）」页可把 `fcitx5-voice-input` / `voice input` 这条流移动到目标源。插件不会自动修改你的默认源，也不会跳过任何设备。
4. **调节 VAD 检测灵敏度**：
   若麦克风拾音音量偏小，VAD 可能未能达到触发阈值。在 `fcitx5-configtool` 中将 `VADThreshold` 从默认的 `20` 调小至 `10` 进行测试。

---

## 3. ASR 识别失败与 API 错误排查

### 现象
说话结束后，候选栏提示识别失败，或者无文字上屏。

### 常见错误原因与解决方案

#### 1. Chat 模式（OpenAI 兼容）相关报错
- **阿里云百炼 (DashScope) 返回 400**
  - **原因**：DashScope 的 `qwen3-asr-flash` 模型使用 Chat Completions 协议，而非 OpenAI 兼容的标准 Whisper 文件上传协议。
  - **解决办法**：在 OpenAI 后端子配置中，务必将 `ApiMode` 设为 `chat`。
- **小米 MiMo 返回 402（Insufficient account balance）**
  - **原因**：MiMo 按量付费账户余额不足（鉴权已通过，计费检查失败）。
  - **解决办法**：在 MiMo 控制台充值后重试。
- **小米 MiMo 返回 400（格式错误）**
  - **原因**：MiMo 的 `asr_options` 仅定义 `language`，官方声明未定义参数可能被过滤或触发异常。
  - **解决办法**：将 `EnableItn` 设为 `false`，并确认 `BaseUrl=https://api.xiaomimimo.com/v1`、`Model=mimo-v2.5-asr`、`ApiMode=chat`。

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
- 录音后端与音频服务路由（`Capture: pulseaudio` / `pipewire`，`Capture started ... routing managed by the audio service`）；插件不枚举、不打印具体设备名，实际来源请在音频管理器中查看
- VAD 语音开始（`Speech onset`）与结束分段
- ASR 请求发送与 HTTP / WebSocket 返回状态
- 火山引擎请求的 `X-Tt-Logid`（可用于向火山引擎客服提交排查工单）

---

## 5. 关闭录音流与隐私

### 插件何时释放麦克风？

- **失焦（切到其他窗口）**：暂停识别并丢弃采样，但**保留**录音流，以便快速切回时无缝继续。
- **切换到其他输入法、更改输入法组、退出或重启 Fcitx5**：释放录音流。

若要立刻确保插件不再录音，最简单的做法是切换到其他输入法（例如用 `Ctrl + Space` 切回原本的中文/英文输入法），或直接退出 Fcitx5。

### 虚拟设备与 Monitor 隐私

由于设备由系统默认或本应用的偏好决定：

- 若默认输入是回放 **Monitor** 源，插件可能录到系统正在播放的声音并送往 ASR。请在 `pavucontrol` 中确认默认输入不是 Monitor。
- 正常的虚拟降噪 / 回声消除源不会再被插件按名称排除，可按需选为默认或应用偏好。
- 插件不会自动试录其他设备，也不会把 PCM 或转写内容写入日志（日志只记录长度）。
