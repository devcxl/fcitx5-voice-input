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
# MODULE targets cannot be linked by target name. Link the exact filename so the
# test's Pipeline::Start substitutes the public boundary via ELF interposition.
# Never load an installed addon: the test must exercise this build's engine.
target_link_libraries(engine_ui_lifecycle_test PRIVATE
    "-l:voice-input-addon.so"
    ${FCITX5_LIBRARIES}
)
target_link_directories(engine_ui_lifecycle_test PRIVATE "$<TARGET_FILE_DIR:voice-input-addon>")
set_target_properties(engine_ui_lifecycle_test PROPERTIES
    BUILD_RPATH "$<TARGET_FILE_DIR:voice-input-addon>")
add_dependencies(engine_ui_lifecycle_test voice-input-addon)
foreach(scenario destroy live active-destroy reactivate pending-switch)
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
