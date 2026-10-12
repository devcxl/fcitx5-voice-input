add_executable(session_worker_completion_test
    session_worker_completion_test.cpp
)

target_include_directories(session_worker_completion_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
)

add_test(NAME session_worker_completion_test
    COMMAND session_worker_completion_test
)

add_executable(audio_frame_assembler_test
    audio_frame_assembler_test.cpp
)

target_include_directories(audio_frame_assembler_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
)

add_test(NAME audio_frame_assembler_test COMMAND audio_frame_assembler_test)

add_executable(llm_request_cancellation_test
    llm_request_cancellation_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/llm/llm_client.cpp
)

target_include_directories(llm_request_cancellation_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(llm_request_cancellation_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
)

add_test(NAME llm_request_cancellation_test
    COMMAND llm_request_cancellation_test
)

add_executable(asr_ordering_test
    asr_ordering_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/asr_engine.cpp
)

target_include_directories(asr_ordering_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
)

add_test(NAME asr_ordering_test COMMAND asr_ordering_test)

add_executable(result_coordinator_test
    result_coordinator_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/pipeline/result_coordinator.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/llm/llm_client.cpp
)

target_include_directories(result_coordinator_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(result_coordinator_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
)

add_test(NAME result_coordinator_test COMMAND result_coordinator_test)

add_executable(realtime_terminal_state_test
    realtime_terminal_state_test.cpp
)

target_include_directories(realtime_terminal_state_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
)

add_test(NAME realtime_terminal_state_test
    COMMAND realtime_terminal_state_test
)

add_executable(ws_frame_reassembly_test
    ws_frame_reassembly_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/realtime_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/volcengine_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/asr_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/utils/base64.cpp
)

target_include_directories(ws_frame_reassembly_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(ws_frame_reassembly_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
    ZLIB::ZLIB
)

add_test(NAME ws_frame_reassembly_test COMMAND ws_frame_reassembly_test)
set_tests_properties(ws_frame_reassembly_test PROPERTIES TIMEOUT 120)

add_executable(ws_send_budget_test
    ws_send_budget_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/realtime_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/mistral_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/volcengine_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/asr_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/utils/base64.cpp
)

target_include_directories(ws_send_budget_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(ws_send_budget_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
    ZLIB::ZLIB
)

add_test(NAME ws_send_budget_test COMMAND ws_send_budget_test)
# 缺陷回归时会挂死（发送循环无截止时间）；超时使其以失败而非挂起结束
set_tests_properties(ws_send_budget_test PROPERTIES TIMEOUT 90)

add_executable(ordered_buffer_stall_test
    ordered_buffer_stall_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/pipeline/result_coordinator.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/llm/llm_client.cpp
)

target_include_directories(ordered_buffer_stall_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(ordered_buffer_stall_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
)

add_test(NAME ordered_buffer_stall_test COMMAND ordered_buffer_stall_test)

add_executable(realtime_commit_tracking_test
    realtime_commit_tracking_test.cpp
)

target_include_directories(realtime_commit_tracking_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
)

add_test(NAME realtime_commit_tracking_test COMMAND realtime_commit_tracking_test)

add_executable(realtime_commit_e2e_test
    realtime_commit_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/realtime_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/asr_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/utils/base64.cpp
)

target_include_directories(realtime_commit_e2e_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(realtime_commit_e2e_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
)

add_test(NAME realtime_commit_e2e_test COMMAND realtime_commit_e2e_test)
set_tests_properties(realtime_commit_e2e_test PROPERTIES TIMEOUT 90)

add_executable(chat_asr_request_test
    chat_asr_request_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/utils/chat_asr_request.cpp
)

target_include_directories(chat_asr_request_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${JSONCPP_INCLUDE_DIRS}
)

target_link_libraries(chat_asr_request_test PRIVATE
    ${JSONCPP_LIBRARIES}
)

add_test(NAME chat_asr_request_test COMMAND chat_asr_request_test)

add_executable(engine_ui_lifecycle_test engine_ui_lifecycle_test.cpp)
target_include_directories(engine_ui_lifecycle_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
)
# MODULE targets cannot be linked by target name. Pass the absolute file to the
# linker without CMake's SONAME inference, so LD_LIBRARY_PATH cannot select an
# installed addon. The executable substitutes the public Pipeline boundary.
target_link_libraries(engine_ui_lifecycle_test PRIVATE
    "-Wl,$<TARGET_FILE:voice-input-addon>"
    ${FCITX5_LIBRARIES}
)
add_dependencies(engine_ui_lifecycle_test voice-input-addon)
foreach(scenario destroy live active-destroy reactivate pending-switch focus-out focus-resume focus-close focus-dead-close focus-unrelated-switch focus-other-voice-switch focus-other-voice-deactivate)
    foreach(directory config data cache)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ui-${scenario}/${directory}/conf")
    endforeach()
    add_test(NAME engine_ui_${scenario}
        COMMAND engine_ui_lifecycle_test ${scenario})
    set_tests_properties(engine_ui_${scenario} PROPERTIES
        TIMEOUT 5
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/ui-${scenario}/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/ui-${scenario}/data;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/ui-${scenario}/cache"
    )
endforeach()

# ── WS 非法服务端消息回归（仅追加，不影响上方 UI 测试块）──────────
add_executable(ws_invalid_response_test
    ws_invalid_response_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/realtime_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/volcengine_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/asr_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/utils/base64.cpp
)

target_include_directories(ws_invalid_response_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(ws_invalid_response_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
    ZLIB::ZLIB
)

add_test(NAME ws_invalid_response_realtime COMMAND ws_invalid_response_test realtime)
add_test(NAME ws_invalid_response_volcengine COMMAND ws_invalid_response_test volcengine)
set_tests_properties(ws_invalid_response_realtime ws_invalid_response_volcengine
    PROPERTIES
    TIMEOUT 60
    SKIP_RETURN_CODE 77
    ENVIRONMENT "NO_PROXY=127.0.0.1;no_proxy=127.0.0.1"
)

add_executable(ws_worker_boundary_test
    ws_worker_boundary_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/realtime_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/volcengine_asr.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/asr_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/asr/utils/base64.cpp
)
target_include_directories(ws_worker_boundary_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)
target_link_libraries(ws_worker_boundary_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    CURL::libcurl
    ZLIB::ZLIB
)
foreach(backend realtime volcengine)
    set(boundary_scenarios partial unknown error final)
    if(backend STREQUAL "volcengine")
        list(APPEND boundary_scenarios empty-ack)
    endif()
    foreach(scenario IN LISTS boundary_scenarios)
        add_test(NAME ws_worker_${backend}_${scenario}
            COMMAND ws_worker_boundary_test ${backend} ${scenario})
        set_tests_properties(ws_worker_${backend}_${scenario} PROPERTIES
            TIMEOUT 8
            SKIP_RETURN_CODE 77
            ENVIRONMENT "NO_PROXY=127.0.0.1;no_proxy=127.0.0.1"
        )
    endforeach()
endforeach()

# ── PulseAudio 录音源路由回归（issue #54）────────────────────────────
# 真实 capture 编入测试目标（在真实 dlopen 边界运行 Start/Stop），不链接
# libpulse-simple——dlopen 由 capture 自身完成，测试可执行文件不感知假库。
# 假库与假 pactl 只写入构建目录；测试子进程经 LD_LIBRARY_PATH / PATH 加载，
# 绝不安装或覆盖系统库，也不接触真实音频服务/设备/网络。
if(PULSE_FOUND)
add_library(fake_pulse SHARED
    support/fake_pulse.cpp
)
set_target_properties(fake_pulse PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "pulse-simple"
    SUFFIX ".so.0"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-pulse"
)
target_include_directories(fake_pulse PRIVATE
    ${PULSE_INCLUDE_DIRS}
)

set(FAKE_PACTL_DIR "${CMAKE_CURRENT_BINARY_DIR}/fake-pactl")
file(MAKE_DIRECTORY "${FAKE_PACTL_DIR}")
# 输出 issue #54 的两条源（内置排前、USB 在后），并记录被调用。
# 修复后不应再有 pactl 子进程，标记文件不会出现。
file(WRITE "${FAKE_PACTL_DIR}/pactl" "#!/bin/sh
[ -n \"\$PULSE_FAKE_PACTL_MARKER\" ] || exit 3
printf 'called\\n' >> \"\$PULSE_FAKE_PACTL_MARKER\"
printf '42\\talsa_input.pci-0000_00_1f.3.analog-stereo\\tmodule-alsa-card.c\\ts16le 2ch 48000Hz\\tRUNNING\\n'
printf '43\\talsa_input.usb-C-Media_Electronics_Inc._USB_Audio_Device-00.mono-fallback\\tmodule-alsa-card.c\\ts16le 1ch 48000Hz\\tRUNNING\\n'
")
file(CHMOD "${FAKE_PACTL_DIR}/pactl" PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)

add_executable(pulse_capture_routing_test
    pulse_capture_routing_test.cpp
    ${CMAKE_SOURCE_DIR}/src/addon/capture/pulse_audio_capture.cpp
)
target_include_directories(pulse_capture_routing_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${PULSE_INCLUDE_DIRS}
)
target_link_libraries(pulse_capture_routing_test PRIVATE
    ${FCITX5_LIBRARIES}
    ${CMAKE_DL_LIBS}
)
add_dependencies(pulse_capture_routing_test fake_pulse)

add_test(NAME pulse_capture_routing_test
    COMMAND pulse_capture_routing_test
        "${CMAKE_CURRENT_BINARY_DIR}/fake-pulse"
        "${FAKE_PACTL_DIR}")
set_tests_properties(pulse_capture_routing_test PROPERTIES TIMEOUT 30)

add_executable(pipeline_capture_lifecycle_test pipeline_capture_lifecycle_test.cpp)
target_include_directories(pipeline_capture_lifecycle_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/addon
    ${FCITX5_INCLUDE_DIRS}
    ${PULSE_INCLUDE_DIRS}
)
# As in the engine lifecycle fixture, substitute the external model boundary
# while executing the real addon. The fake audio library is test-only.
target_link_libraries(pipeline_capture_lifecycle_test PRIVATE
    "-Wl,$<TARGET_FILE:voice-input-addon>"
    ${FCITX5_LIBRARIES}
    fake_pulse
)
add_dependencies(pipeline_capture_lifecycle_test voice-input-addon)
add_test(NAME pipeline_capture_lifecycle_test COMMAND pipeline_capture_lifecycle_test)
set_tests_properties(pipeline_capture_lifecycle_test PROPERTIES TIMEOUT 10)
endif()

include(pipewire_capture_tests.cmake)
