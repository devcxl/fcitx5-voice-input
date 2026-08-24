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
