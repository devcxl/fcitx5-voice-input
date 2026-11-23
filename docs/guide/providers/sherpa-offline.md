# Sherpa-ONNX X-ASR 本地离线后端

sherpa-onnx X-ASR 离线后端（`sherpa_offline`）使用非流式 zipformer transducer 模型，在本机 CPU 上整句解码：语音不离开计算机，无需网络与 API Key。该后端只在一句话结束后返回最终文本，不提供说话过程中的增量预编辑。配置保存在 `~/.config/fcitx5/conf/voiceinput-sherpa-offline.conf`。

选择此后端：主配置中 `ActiveBackend=sherpa_offline`，再点击齿轮 ⚙ 配置模型与热词。后端选择与其他全局参数见[配置总览](/guide/configuration)。

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
# X-ASR 中英混说离线模型（int8），默认下载到
# ~/.local/share/fcitx5/voice-input/models/x-asr
./scripts/download-model.sh --offline
```

脚本从 sherpa-onnx 官方 Releases 下载 `sherpa-onnx-x-asr-zipformer-transducer-zh-en-int8-2026-06-03`，解压后只保留 `encoder*.onnx`、`decoder*.onnx`、`joiner*.onnx`、`tokens.txt`（若模型包含 `bpe.model` 也会一并保留）。该模型为中英混说离线模型，不支持边说边出字。

## 模型目录探测

`ModelDir` 留空时，插件按以下顺序查找；每项先检查目录自身，再检查其一级子目录：

1. `$XDG_DATA_HOME/fcitx5/voice-input/models`，未设置 `XDG_DATA_HOME` 时为 `~/.local/share/fcitx5/voice-input/models`
2. `/usr/local/share/fcitx5/voice-input/models`

`ModelDir` 非空时先尝试该目录，找不到再按上述列表继续搜索。有效模型目录需同时具备：

- `encoder` / `decoder` / `joiner` 前缀的 `.onnx` 文件；同一前缀存在多份时优先 `.int8.onnx`
- `tokens.txt`

两个本地后端共用同一套探测逻辑，日志出现 `Found valid model directory` 表示成功；否则初始化失败，状态栏提示「语音识别失败」，日志中输出 `Failed to find valid model files`。

## 热词与建模单元

`HotwordsFile` 留空时，依次探测：

1. `$XDG_DATA_HOME/fcitx5/voice-input/hotwords.txt`（未设置时为 `~/.local/share/fcitx5/voice-input/hotwords.txt`）
2. `~/.config/fcitx5/hotwords.txt`

热词文件为 UTF-8 纯文本，每行一个热词，行尾可附 `:分值` 覆盖 `HotwordsScore`。与流式后端不同，离线后端会根据模型目录中 `tokens.txt` 的词表形态自动处理：

- **裸汉字词表（`cjkchar`）**：直接使用热词文件，按整行词语编码（如 `深度学习`），插件不做拆分。
- **`▁汉字` 前缀词表（X-ASR 这类每字成词的 BPE 词表）**：把每行汉字逐字拆开、以空格连接并保留行尾分值，写入 `$XDG_CACHE_HOME/fcitx5/voice-input/hotwords-bpe.txt`（未设置时为 `~/.cache/fcitx5/voice-input/hotwords-bpe.txt`），同时切换到 `modeling_unit=bpe`。BPE 词表优先取模型目录下的 `bpe.vocab`，不存在时退回 `tokens.txt`。

热词文件不可读、为空或无法生成缓存文件时，插件告警并关闭热词，后端按 `greedy_search` 继续识别（上述检查发生在 `▁汉字` 词表需要重写热词的路径上）；热词生效时使用 `modified_beam_search` 并应用 `HotwordsScore`。

## 行为说明

- **整句解码，无增量**：说话期间音频先缓存在内存，VAD 判定句子结束后一次性解码并提交最终文本；预编辑区不会出现中间结果。
- **断句仍由 Silero VAD 负责**：`SilenceThresholdMs`、`MaxSpeechMs` 等参数决定一句话的边界。
- **纯 CPU 推理**：固定使用 CPU provider，`NumThreads` 控制线程数。
- **取消即丢弃**：句子被取消（如切窗暂停）时，解码前会检查取消标记；已完成的解码结果也会被丢弃，不会上屏。

## 编译开关

两个本地后端共用 CMake 选项 `ENABLE_SHERPA_ONNX`（默认 `ON`）。子模块缺失时依次回退到系统 `sherpa-onnx` 库，仍找不到则编译为 stub（后端可选但初始化失败，插件本体正常加载）。完整的编译与回退说明见 [流式后端的编译开关](/guide/providers/sherpa-onnx#编译开关)。

## 运行排查

先确认模型目录中 `encoder*` / `decoder*` / `joiner*` 的 `.onnx` 与 `tokens.txt` 齐全，再查看日志中 `[voice-input:sherpa_offline]` 前缀的行（热词启用时会打印实际编码方式与分值）。其他通用问题见[常见问题与排查](/guide/troubleshooting)。
