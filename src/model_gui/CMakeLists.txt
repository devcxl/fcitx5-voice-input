# SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
#
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Qt ConfigUI 插件（fcitx5-configtool / fcitx5-qt gui-wrapper 加载）。
#
# 由主 CMakeLists 通过 add_subdirectory(src/model_gui) 引入；也可单独构建：
#   cmake -S src/model_gui -B build-model-gui \
#     -DVOICE_INPUT_MODEL_INSTALLER=/usr/lib/fcitx5/voice-input-model-installer
#
# 依赖（每个 Qt 主版本独立）：
#   Qt${N}::Widgets、Qt${N}::DBus
#   Fcitx5Qt${N}WidgetsAddons -> Fcitx5Qt${N}::WidgetsAddons
#   Fcitx5::Utils（经 Fcitx5Qt 的 INTERFACE_LINK_LIBRARIES 传递）
#
# 安装位置必须是 fcitx addon 目录下的 qt5/ 或 qt6/ 子目录：
# FcitxQtConfigUIFactory::scan() 用
# StandardPaths::locate(Addon, "qt6") 扫描（StandardPathsMode::System）。

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(fcitx5-voice-input-model-gui LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

include(GNUInstallDirs)

# 安装器绝对路径：主集成负责注入并把可执行文件放进 fcitx addon 目录。
# 默认值跟随 ${FCITX5_ADDON_DIR}（本仓库顶层用相对 prefix 的形式，
# 例如 lib/fcitx5 或 lib/x86_64-linux-gnu/fcitx5），保证多架构打包一致。
set(VOICE_INPUT_MODEL_INSTALLER "" CACHE FILEPATH
    "Absolute path of voice-input-model-installer used by the Qt config UI")
if(NOT VOICE_INPUT_MODEL_INSTALLER)
  if(DEFINED FCITX5_ADDON_DIR AND FCITX5_ADDON_DIR)
    if(IS_ABSOLUTE "${FCITX5_ADDON_DIR}")
      set(VOICE_INPUT_MODEL_INSTALLER
          "${FCITX5_ADDON_DIR}/voice-input-model-installer")
    else()
      set(VOICE_INPUT_MODEL_INSTALLER
          "${CMAKE_INSTALL_PREFIX}/${FCITX5_ADDON_DIR}/voice-input-model-installer")
    endif()
  else()
    set(VOICE_INPUT_MODEL_INSTALLER
        "${CMAKE_INSTALL_FULL_LIBDIR}/fcitx5/voice-input-model-installer")
  endif()
  message(STATUS
          "model_gui: VOICE_INPUT_MODEL_INSTALLER not set, defaulting to "
          "${VOICE_INPUT_MODEL_INSTALLER} (install-time path)")
endif()
if(VOICE_INPUT_MODEL_INSTALLER AND NOT IS_ABSOLUTE "${VOICE_INPUT_MODEL_INSTALLER}")
  message(FATAL_ERROR
          "VOICE_INPUT_MODEL_INSTALLER must be an absolute path, got: "
          "${VOICE_INPUT_MODEL_INSTALLER}")
endif()

# 安装目录（相对 CMAKE_INSTALL_PREFIX）。
if(DEFINED FCITX5_ADDON_DIR AND FCITX5_ADDON_DIR)
  set(_model_gui_addon_dir "${FCITX5_ADDON_DIR}")
else()
  set(_model_gui_addon_dir "lib/fcitx5")
endif()

set(_model_gui_sources
    installer_error_text.cpp
    installer_protocol.cpp
    installer_runner.cpp
    model_check_runner.cpp
    model_manager_widget.cpp
    model_manager_widget_ui.cpp
    model_status.cpp
    status_text.cpp
    plugin.cpp)
set(_model_gui_headers
    i18n.h
    installer_error_text.h
    installer_protocol.h
    installer_runner.h
    model_check_runner.h
    model_kind.h
    model_manager_widget.h
    model_status.h
    status_text.h
    plugin.h
    model_gui.json)

# 为某个 Qt 主版本构建一个 MODULE。
function(voice_input_add_model_gui QT_MAJOR)
  set(_qt "Qt${QT_MAJOR}")
  set(_fcitx_qt "Fcitx5Qt${QT_MAJOR}WidgetsAddons")

  find_package(${_qt} QUIET COMPONENTS Core Gui Widgets DBus)
  if(NOT ${_qt}_FOUND)
    message(STATUS "model_gui: ${_qt} not found, skipping Qt${QT_MAJOR} variant")
    return()
  endif()
  # DbUS 组件单独校验（部分发行版把它拆在独立包）。
  if(NOT TARGET ${_qt}::DBus)
    message(STATUS "model_gui: ${_qt}::DBus not available, skipping Qt${QT_MAJOR} variant")
    return()
  endif()

  find_package(${_fcitx_qt} QUIET)
  if(NOT TARGET Fcitx5Qt${QT_MAJOR}::WidgetsAddons)
    message(STATUS
            "model_gui: Fcitx5Qt${QT_MAJOR}::WidgetsAddons not found, skipping "
            "Qt${QT_MAJOR} variant")
    return()
  endif()

  set(_target "voice-input-model-gui-qt${QT_MAJOR}")
  add_library(${_target} MODULE ${_model_gui_sources} ${_model_gui_headers})

  target_compile_features(${_target} PRIVATE cxx_std_17)
  target_link_libraries(${_target}
      PRIVATE
      Fcitx5Qt${QT_MAJOR}::WidgetsAddons
      ${_qt}::Core
      ${_qt}::Gui
      ${_qt}::Widgets
      ${_qt}::DBus)

  set_target_properties(${_target} PROPERTIES
      AUTOMOC TRUE
      OUTPUT_NAME "voice-input-model-gui"
      # 两个 Qt 版本同名输出必须分目录，否则后构建的会覆盖先构建的 .so。
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qt${QT_MAJOR}"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qt${QT_MAJOR}")

  target_compile_definitions(${_target}
      PRIVATE
      FCITX_GETTEXT_DOMAIN="fcitx5-voice-input"
      VOICE_INPUT_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
      VOICE_INPUT_MODEL_INSTALLER="${VOICE_INPUT_MODEL_INSTALLER}")

  install(TARGETS ${_target}
      LIBRARY DESTINATION "${_model_gui_addon_dir}/qt${QT_MAJOR}")

  message(STATUS
          "model_gui: built ${_target} -> "
          "${_model_gui_addon_dir}/qt${QT_MAJOR} (installer: "
          "${VOICE_INPUT_MODEL_INSTALLER})")
endfunction()

voice_input_add_model_gui(6)
voice_input_add_model_gui(5)

# ─── 单元测试（可选，默认关闭）─────────────────────────────────────────────
# 覆盖：JSON 行解析、状态解析、QProcess 启动/取消、安装器路径解析。
# 由主 CMake 在 BUILD_TESTS 时引入，或：
#   cmake -S src/model_gui -B build-model-gui -DVOICE_INPUT_MODEL_GUI_BUILD_TESTS=ON
option(VOICE_INPUT_MODEL_GUI_BUILD_TESTS "Build model_gui tests" OFF)
if(VOICE_INPUT_MODEL_GUI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
