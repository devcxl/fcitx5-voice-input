# SPDX-FileCopyrightText: 2026 fcitx5-voice-input contributors
#
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# model_gui 单元测试。需要 Qt6（Widgets/DBus）与 Fcitx5Qt6::WidgetsAddons；
# 只用 Qt5 的发行版上这些测试会整体跳过，不影响模块构建。

if(NOT TARGET Qt6::Core)
  message(STATUS "model_gui tests: Qt6 not found, skipping")
  return()
endif()

set(_model_gui_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ── 1. 协议解析 ────────────────────────────────────────────────────────────
add_executable(model_gui_installer_protocol_test
    installer_protocol_test.cpp
    ${_model_gui_test_dir}/installer_protocol.cpp)
target_include_directories(model_gui_installer_protocol_test PRIVATE
    ${_model_gui_test_dir})
target_compile_features(model_gui_installer_protocol_test PRIVATE cxx_std_17)
target_link_libraries(model_gui_installer_protocol_test
    PRIVATE Qt6::Core)
add_test(NAME model_gui_installer_protocol_test
    COMMAND model_gui_installer_protocol_test)

# ── 2. 状态解析（含「缺失即未知」）────────────────────────────────────────
add_executable(model_gui_model_status_test
    model_status_test.cpp
    ${_model_gui_test_dir}/model_status.cpp
    ${_model_gui_test_dir}/model_status.h)
set_target_properties(model_gui_model_status_test PROPERTIES AUTOMOC TRUE)
target_include_directories(model_gui_model_status_test PRIVATE
    ${_model_gui_test_dir})
target_compile_features(model_gui_model_status_test PRIVATE cxx_std_17)
target_link_libraries(model_gui_model_status_test
    PRIVATE Qt6::Core Qt6::DBus)
add_test(NAME model_gui_model_status_test COMMAND model_gui_model_status_test)

# ── 3. 安装器进程控制（假安装器脚本）──────────────────────────────────────
add_executable(model_gui_installer_runner_test
    installer_runner_test.cpp
    ${_model_gui_test_dir}/installer_protocol.cpp
    ${_model_gui_test_dir}/installer_runner.cpp
    ${_model_gui_test_dir}/installer_runner.h)
set_target_properties(model_gui_installer_runner_test PROPERTIES AUTOMOC TRUE)
target_include_directories(model_gui_installer_runner_test PRIVATE
    ${_model_gui_test_dir})
target_compile_features(model_gui_installer_runner_test PRIVATE cxx_std_17)
target_link_libraries(model_gui_installer_runner_test
    PRIVATE Qt6::Core)
add_test(NAME model_gui_installer_runner_test
    COMMAND model_gui_installer_runner_test)

# ── 3b. --check-model 进程生命周期与解析 ──────────────────────────────────
add_executable(model_gui_model_check_runner_test
    model_check_runner_test.cpp
    ${_model_gui_test_dir}/installer_protocol.cpp
    ${_model_gui_test_dir}/model_check_runner.cpp
    ${_model_gui_test_dir}/model_check_runner.h)
set_target_properties(model_gui_model_check_runner_test PROPERTIES AUTOMOC TRUE)
target_include_directories(model_gui_model_check_runner_test PRIVATE
    ${_model_gui_test_dir})
target_compile_features(model_gui_model_check_runner_test PRIVATE cxx_std_17)
target_link_libraries(model_gui_model_check_runner_test
    PRIVATE Qt6::Core)
add_test(NAME model_gui_model_check_runner_test
    COMMAND model_gui_model_check_runner_test)

# ── 4. 插件元数据 / URI key 映射 / saveSubConfig 信号 ─────────────────────
if(TARGET voice-input-model-gui-qt6 AND TARGET Fcitx5Qt6::WidgetsAddons)
  add_executable(model_gui_plugin_test
      plugin_test.cpp
      plugin_widget_checks.cpp
      plugin_warning_checks.cpp
      support/plugin_test_support.h
      ${_model_gui_test_dir}/installer_error_text.cpp
      ${_model_gui_test_dir}/installer_protocol.cpp
      ${_model_gui_test_dir}/installer_runner.cpp
      ${_model_gui_test_dir}/model_manager_widget.cpp
      ${_model_gui_test_dir}/model_manager_widget_ui.cpp
      ${_model_gui_test_dir}/model_check_runner.cpp
      ${_model_gui_test_dir}/model_status.cpp
      ${_model_gui_test_dir}/status_text.cpp
      ${_model_gui_test_dir}/installer_runner.h
      ${_model_gui_test_dir}/model_manager_widget.h
      ${_model_gui_test_dir}/model_check_runner.h
      ${_model_gui_test_dir}/model_status.h
      ${_model_gui_test_dir}/status_text.h)
  set_target_properties(model_gui_plugin_test PROPERTIES AUTOMOC TRUE)
  target_include_directories(model_gui_plugin_test PRIVATE
      ${_model_gui_test_dir}
      ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_features(model_gui_plugin_test PRIVATE cxx_std_17)
  target_compile_definitions(model_gui_plugin_test PRIVATE
      FCITX_GETTEXT_DOMAIN="fcitx5-voice-input"
      VOICE_INPUT_MODEL_GUI_PLUGIN_FILE="$<TARGET_FILE:voice-input-model-gui-qt6>")
  target_link_libraries(model_gui_plugin_test
      PRIVATE
      Fcitx5Qt6::WidgetsAddons
      Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)
  add_test(NAME model_gui_plugin_test COMMAND model_gui_plugin_test)
  set_tests_properties(model_gui_plugin_test PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

  # ── 5. 识别状态自动轮询（需要私有 D-Bus）────────────────────────────────
  find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
  if(DBUS_RUN_SESSION_EXECUTABLE)
    add_executable(model_gui_poll_test
        poll_test.cpp
        ${_model_gui_test_dir}/installer_error_text.cpp
        ${_model_gui_test_dir}/installer_protocol.cpp
        ${_model_gui_test_dir}/installer_runner.cpp
        ${_model_gui_test_dir}/model_check_runner.cpp
        ${_model_gui_test_dir}/model_manager_widget.cpp
        ${_model_gui_test_dir}/model_manager_widget_ui.cpp
        ${_model_gui_test_dir}/model_status.cpp
        ${_model_gui_test_dir}/status_text.cpp
        ${_model_gui_test_dir}/installer_runner.h
        ${_model_gui_test_dir}/model_check_runner.h
        ${_model_gui_test_dir}/model_manager_widget.h
        ${_model_gui_test_dir}/model_status.h
        ${_model_gui_test_dir}/status_text.h)
    set_target_properties(model_gui_poll_test PROPERTIES AUTOMOC TRUE)
    target_include_directories(model_gui_poll_test PRIVATE
        ${_model_gui_test_dir})
    target_compile_features(model_gui_poll_test PRIVATE cxx_std_17)
    target_compile_definitions(model_gui_poll_test PRIVATE
        FCITX_GETTEXT_DOMAIN="fcitx5-voice-input"
        VOICE_INPUT_MODEL_INSTALLER="/bin/false")
    target_link_libraries(model_gui_poll_test
        PRIVATE
        Fcitx5Qt6::WidgetsAddons
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus)
    add_test(NAME model_gui_poll_test
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:model_gui_poll_test>)
    set_tests_properties(model_gui_poll_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  else()
    message(STATUS "model_gui tests: dbus-run-session not found, poll test skipped")
  endif()
endif()
