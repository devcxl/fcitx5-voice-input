# ── PipeWire 直连采集生命周期回归（issue #54 失焦保流但暂停识别）──────
# 真实 capture 编入测试目标，在真实 dlopen 边界运行 Start/Stop/SetDeliveryEnabled，
# 不链接真实 libpipewire-0.3：dlopen 由 capture 自身完成，测试可执行文件仅链接
# fcitx5 与 dl。
# 假库仅写入构建目录（fake-pipewire/），由测试子进程的 LD_LIBRARY_PATH 加载；
# 不安装、不覆盖系统库，也不接触真实 PipeWire 服务/设备/网络。
if(PIPEWIRE_FOUND)
    add_library(fake_pipewire SHARED
        support/fake_pipewire.cpp
    )
    set_target_properties(fake_pipewire PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "pipewire-0.3"
        SUFFIX ".so.0"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake-pipewire"
    )
    target_include_directories(fake_pipewire PRIVATE
        ${PIPEWIRE_INCLUDE_DIRS}
    )

    add_executable(pipewire_capture_lifecycle_test
        pipewire_capture_lifecycle_test.cpp
        ${CMAKE_SOURCE_DIR}/src/addon/capture/pipewire_capture.cpp
    )
    target_include_directories(pipewire_capture_lifecycle_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src/addon
        ${FCITX5_INCLUDE_DIRS}
        ${PIPEWIRE_INCLUDE_DIRS}
    )
    target_link_libraries(pipewire_capture_lifecycle_test PRIVATE
        ${FCITX5_LIBRARIES}
        ${CMAKE_DL_LIBS}
    )
    add_dependencies(pipewire_capture_lifecycle_test fake_pipewire)

    add_test(NAME pipewire_capture_lifecycle_test
        COMMAND pipewire_capture_lifecycle_test
            "${CMAKE_CURRENT_BINARY_DIR}/fake-pipewire")
    set_tests_properties(pipewire_capture_lifecycle_test PROPERTIES TIMEOUT 60)
else()
    message(STATUS "PipeWire not found — skipping pipewire_capture_lifecycle_test")
endif()
