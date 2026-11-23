# Sherpa-ONNX 本地流式后端

sherpa-onnx 本地流式后端（`sherpa_onnx`）使用流式 zipformer transducer 模型，在本机 CPU 上完成识别：语音不离开计算机，无需网络与 API Key，说话过程中候选栏会持续刷新增量结果。配置保存在 `~/.config/fcitx5/conf/voiceinput-sherpa-onnx.conf`。

选择此后端：主配置中 `ActiveBackend=sherpa_onnx`，再点击齿轮 ⚙ 配置该后端的模型与热词。后端选择与其他全局参数见[配置总览](/guide/configuration)。

## 配置参数项

| 参数项 | 说明 | 默认值 | 描述 |
|---|---|---|---|
| `ModelDir` | 模型目录 | (空，自动搜索) | 留空时按[模型目录探测](#模型目录探测)自动查找；显式填写时优先使用该目录 |
| `NumThreads` | CPU 推理线程数 (1-16) | `2` | 线程越多解码越快，CPU 占用也越高 |
| `HotwordsFile` | 热词文件路径 | (空，自动探测) | 留空时自动查找默认热词文件；显式配置但文件不存在时仅告警并关闭热词，不回退默认位置 |
| `HotwordsScore` | 热词提权分值 (1-10) | `2` | 作为 `hotwords_score` 传给 sherpa-onnx，数值越大热词权重越高 |
| `AutoCommit` | 无 LLM 时自动上屏 | `true` | 关闭后识别结果保留在预编辑区（preedit），不自动 commit 上屏 |

## 获取模型

模型不随安装包分发，需要单独下载到本机：

```bash
# 流式中文 zipformer 模型（int8），默认下载到
# ~/.local/share/fcitx5/voice-input/models/zipformer
./scripts/download-model.sh --online
```

脚本从 sherpa-onnx 官方 Releases 下载 `sherpa-onnx-streaming-zipformer-zh-xlarge-int8-2025-06-30`，解压后只保留 `encoder*.onnx`、`decoder*.onnx`、`joiner*.onnx`、`tokens.txt`（若模型包含 `bpe.model` 也会一并保留）。也可以自行准备其他流式 zipformer transducer 模型，按同样文件名放入目录。

## 模型目录探测

`ModelDir` 留空时，插件按以下顺序查找；每项先检查目录自身，再检查其一级子目录（因此 `models/zipformer` 这类布局可直接命中）：

1. `$XDG_DATA_HOME/fcitx5/voice-input/models`，未设置 `XDG_DATA_HOME` 时为 `~/.local/share/fcitx5/voice-input/models`
2. `/usr/local/share/fcitx5/voice-input/models`

`ModelDir` 非空时先尝试该目录，找不到再按上述列表继续搜索。一个目录被视为有效模型目录需同时具备：

- `encoder` / `decoder` / `joiner` 前缀的 `.onnx` 文件；同一前缀存在多份时优先 `.int8.onnx`
- `tokens.txt`

日志出现 `Found valid model directory` 表示定位成功；否则引擎初始化失败，状态栏提示「语音识别失败」，日志中输出 `Failed to find valid model files`。

## 热词

`HotwordsFile` 留空时，依次探测：

1. `$XDG_DATA_HOME/fcitx5/voice-input/hotwords.txt`（未设置时为 `~/.local/share/fcitx5/voice-input/hotwords.txt`）
2. `~/.config/fcitx5/hotwords.txt`

也可以用 `scripts/export-hotwords.sh` 从 fcitx5 自定义短语与用户词库导出默认位置的热词文件。

热词文件为 UTF-8 纯文本，每行一个热词（如 `深度学习`）；可在词后加空格和 `:分值` 覆盖该词权重（如 `深度学习 :3`，分值必须与词之间用空白分隔）。流式后端把文件原样交给 sherpa-onnx（建模单元固定为 `cjkchar`，按词语整行编码），插件不做切分或改写。检测到热词文件存在时，解码方法切换为 `modified_beam_search`，否则使用 `greedy_search`。热词只参与本地解码，不会发送到任何服务器。

## 行为说明

- **断句由插件统一负责**：sherpa-onnx 内置的 endpoint 检测被关闭，语音起止完全由 Silero VAD 决定，避免双重切分。
- **增量输出**：识别过程中每次文本变化都会更新预编辑区，语音结束时提交最终结果。
- **纯 CPU 推理**：固定使用 CPU provider，`NumThreads` 控制线程数，不依赖 GPU。
- **无需重采样**：音频以 16kHz 单声道送入模型，与采集格式一致。

## 编译开关

本地后端由 CMake 选项 `ENABLE_SHERPA_ONNX` 控制，默认 `ON`：

- 存在 `third_party/sherpa-onnx` 子模块时优先内嵌编译（先执行 `git submodule update --init --recursive`），`libsherpa-onnx-c-api.so` 随插件安装到同一目录。
- 没有子模块时查找系统库：pkg-config `sherpa-onnx`，或 `SHERPA_ONNX_ROOT` / 系统路径下的 `sherpa-onnx/c-api/c-api.h` 与 `libsherpa-onnx-c-api`。
- 两者都不可用时编译为 stub：后端仍出现在下拉框中，但初始化失败（状态栏提示「语音识别失败」），插件本体正常加载。
- 传 `-DENABLE_SHERPA_ONNX=OFF` 可关闭本地后端（同样以 stub 形式存在，不编译 sherpa-onnx）。

## 运行排查

先确认模型目录中 `encoder*` / `decoder*` / `joiner*` 的 `.onnx` 与 `tokens.txt` 齐全，再查看日志中 `[voice-input:sherpa_onnx]` 前缀的行。其他通用问题见[常见问题与排查](/guide/troubleshooting)。
