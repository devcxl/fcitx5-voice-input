# PRD：为 fcitx5-voice-input 添加输入法图标

## 背景

Issue: [#5](https://github.com/devcxl/fcitx5-voice-input/issues/5)

fcitx5-voice-input addon 缺少输入法图标，在 fcitx5 输入法列表中显示为空白/默认图标，
用户体验差且不易发现。

社区用户贡献了 SVG/PNG 图标和 `voiceinput.conf` 配置文件，需要集成到项目中。

## 用户故事

- **作为** fcitx5 中文输入法用户
- **我希望** 在输入法列表中看到语音输入法的图标
- **以便** 快速识别和切换到语音输入模式

## 验收标准

1. `cmake --install` 后，`fcitx_voiceinput.svg` 安装到 `/usr/share/icons/hicolor/scalable/apps/`
2. `cmake --install` 后，`voiceinput.conf` 安装到 `/usr/share/fcitx5/inputmethod/`
3. 重启 fcitx5 后，输入法列表中 Voice Input 显示麦克风图标

## 技术方案概要

1. 图标文件 `fcitx_voiceinput.svg` → 放入源码树 `data/icons/` 下
2. 输入法配置 `voiceinput.conf` → 放入 `src/addon/` 下
3. CMakeLists.txt 添加 `install(FILES ...)` 指令安装到目标路径

## 技术约束

- SVG 图标路径遵循 XDG/Freedesktop 规范：`/usr/share/icons/hicolor/scalable/apps/`
- `voiceinput.conf` 是 fcitx5 输入法注册文件，路径：`/usr/share/fcitx5/inputmethod/`
- 不涉及代码变更，仅文件安装

## 排除范围（Out of Scope）

- 多尺寸 PNG 图标渲染（SVG 已覆盖所有尺寸）
- `gtk-update-icon-cache` 自动刷新（由包管理器钩子处理）
- 图标主题支持深色模式变体

## 优先级

P0 — 核心体验修复，尽快交付。
