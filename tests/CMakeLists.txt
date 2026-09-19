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
