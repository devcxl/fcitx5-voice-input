cmake_minimum_required(VERSION 3.20)
project(fcitx5-voice-input VERSION 0.5.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Include custom find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ─── Options ─────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)
set(ONNXRUNTIME_ROOT "" CACHE PATH "Path to an ONNX Runtime installation")
option(ENABLE_SHERPA_ONNX "Enable Sherpa-ONNX local streaming ASR" ON)
set(SHERPA_ONNX_ROOT "" CACHE PATH "Path to a sherpa-onnx installation")

# ─── Dependencies ─────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# Fcitx5 core — try "fcitx5" (Arch) then "Fcitx5Core" (Ubuntu/Debian)
pkg_check_modules(FCITX5 QUIET fcitx5)
if(NOT FCITX5_FOUND)
    pkg_check_modules(FCITX5 REQUIRED Fcitx5Core)
endif()

# Determine fcitx5 addon directory (handles multiarch like Ubuntu's lib/x86_64-linux-gnu)
# Use FCITX5_LIBRARY_DIRS from pkg-config, then make relative to install prefix
# so packaging (CPack, PKGBUILD) works correctly.
if(FCITX5_LIBRARY_DIRS)
    list(GET FCITX5_LIBRARY_DIRS 0 _fcitx5_libdir)
    file(RELATIVE_PATH _rel_fcitx5_libdir "${CMAKE_INSTALL_PREFIX}" "${_fcitx5_libdir}")
    set(FCITX5_ADDON_DIR "${_rel_fcitx5_libdir}/fcitx5")
    message(STATUS "Fcitx5 addon dir: ${FCITX5_ADDON_DIR}")
else()
    set(FCITX5_ADDON_DIR "lib/fcitx5")
endif()

# PulseAudio 采集是必需基线，也兼容现代 PipeWire 桌面的 pipewire-pulse 服务。
pkg_check_modules(PULSE REQUIRED libpulse-simple libpulse)

# PipeWire 直连采集是可选运行时回退。
pkg_check_modules(PIPEWIRE QUIET pipewire-0.3)
if(NOT PIPEWIRE_FOUND)
    pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
endif()

if(NOT PIPEWIRE_FOUND)
    message(WARNING "PipeWire not found — building without PipeWire capture backend")
endif()

# JSON parsing
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# HTTP/WebSocket client (for cloud ASR backends)
find_package(CURL REQUIRED)
if(CURL_VERSION_STRING VERSION_LESS "7.86.0")
    message(FATAL_ERROR "Volcengine WebSocket ASR requires libcurl >= 7.86.0; found ${CURL_VERSION_STRING}")
endif()

# Gzip compression (for Volcengine WebSocket ASR)
find_package(ZLIB REQUIRED)

# Silero VAD via ONNX Runtime (the only VAD backend)
set(ENABLE_SILERO_VAD ON)
pkg_check_modules(ONNXRUNTIME QUIET onnxruntime)
if(NOT ONNXRUNTIME_FOUND)
    pkg_check_modules(ONNXRUNTIME QUIET libonnxruntime)
endif()

if(NOT ONNXRUNTIME_FOUND)
    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        HINTS
            "${ONNXRUNTIME_ROOT}/include"
            "${ONNXRUNTIME_ROOT}/include/onnxruntime"
            /usr/include/onnxruntime
            /usr/local/include/onnxruntime
    )
    find_library(ONNXRUNTIME_LIBRARY
        NAMES onnxruntime
        HINTS
            "${ONNXRUNTIME_ROOT}/lib"
            "${ONNXRUNTIME_ROOT}/lib64"
            /usr/lib
            /usr/local/lib
    )
    if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
        set(ONNXRUNTIME_FOUND TRUE)
        set(ONNXRUNTIME_INCLUDE_DIRS "${ONNXRUNTIME_INCLUDE_DIR}")
        set(ONNXRUNTIME_LIBRARIES "${ONNXRUNTIME_LIBRARY}")
    endif()
endif()

if(NOT ONNXRUNTIME_FOUND)
    message(FATAL_ERROR "Silero VAD requires ONNX Runtime; install onnxruntime (Arch: onnxruntime, Debian: libonnxruntime-dev)")
endif()

set(SILERO_VAD_MODEL_SOURCE "${CMAKE_SOURCE_DIR}/third_party/silero-vad/src/silero_vad/data/silero_vad.onnx")
if(NOT EXISTS "${SILERO_VAD_MODEL_SOURCE}")
    message(FATAL_ERROR "Silero VAD model not found at ${SILERO_VAD_MODEL_SOURCE}. Run: git submodule update --init --recursive")
endif()
set(VOICE_INPUT_MODEL_DIR "${CMAKE_INSTALL_FULL_DATADIR}/fcitx5/voice-input/models")
message(STATUS "Silero VAD enabled with model: ${SILERO_VAD_MODEL_SOURCE}")

# Sherpa-ONNX local streaming ASR (optional)
if(ENABLE_SHERPA_ONNX)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/CMakeLists.txt")
        # 优先使用 Submodule 源码内嵌编译
        set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_CHECK OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_BUILD_C_API_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE ON CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_GPU OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

        if(ONNXRUNTIME_INCLUDE_DIRS)
            set(ENV{SHERPA_ONNXRUNTIME_INCLUDE_DIR} "${ONNXRUNTIME_INCLUDE_DIRS}")
        endif()
        if(ONNXRUNTIME_LIBRARIES)
            get_filename_component(_ort_libdir "${ONNXRUNTIME_LIBRARIES}" DIRECTORY)
            set(ENV{SHERPA_ONNXRUNTIME_LIB_DIR} "${_ort_libdir}")
        endif()

        add_subdirectory(third_party/sherpa-onnx EXCLUDE_FROM_ALL)

        set(SHERPA_ONNX_FOUND TRUE)
        set(SHERPA_ONNX_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx")
        set(SHERPA_ONNX_LIBRARIES sherpa-onnx-c-api)
        set(_sherpa_is_target TRUE)
        message(STATUS "Sherpa-ONNX ASR enabled via submodule")
    else()
        pkg_check_modules(SHERPA_ONNX QUIET sherpa-onnx)
        if(NOT SHERPA_ONNX_FOUND)
            find_path(SHERPA_ONNX_INCLUDE_DIR
                NAMES sherpa-onnx/c-api/c-api.h
                HINTS
                    "${SHERPA_ONNX_ROOT}/include"
                    /usr/include
                    /usr/local/include
            )
            find_library(SHERPA_ONNX_C_API_LIB
                NAMES sherpa-onnx-c-api
                HINTS
                    "${SHERPA_ONNX_ROOT}/lib"
                    "${SHERPA_ONNX_ROOT}/lib64"
                    /usr/lib
                    /usr/local/lib
            )
            if(SHERPA_ONNX_INCLUDE_DIR AND SHERPA_ONNX_C_API_LIB)
                set(SHERPA_ONNX_FOUND TRUE)
                set(SHERPA_ONNX_INCLUDE_DIRS "${SHERPA_ONNX_INCLUDE_DIR}")
                set(SHERPA_ONNX_LIBRARIES "${SHERPA_ONNX_C_API_LIB}")
                get_filename_component(_sherpa_lib_dir "${SHERPA_ONNX_C_API_LIB}" DIRECTORY)
                find_library(KALDI_FBANK_LIB
                    NAMES kaldi-native-fbank-core
                    HINTS "${_sherpa_lib_dir}"
                )
                if(KALDI_FBANK_LIB)
                    list(APPEND SHERPA_ONNX_LIBRARIES "${KALDI_FBANK_LIB}")
                endif()
            endif()
        endif()

        if(SHERPA_ONNX_FOUND)
            message(STATUS "Sherpa-ONNX ASR enabled with system library: ${SHERPA_ONNX_INCLUDE_DIRS}")
        else()
            message(STATUS "sherpa-onnx not found — Sherpa-ONNX backend built in stub mode")
        endif()
    endif()
endif()

# ─── Sources ──────────────────────────────────────────────────────────────────
set(ADDON_SOURCES
    src/addon/engine.cpp
    src/addon/vad/vad.cpp
    src/addon/vad/silero_vad.cpp
    src/addon/pipeline/pipeline.cpp
    src/addon/pipeline/result_coordinator.cpp
    src/addon/asr/openai_asr.cpp
    src/addon/asr/realtime_asr.cpp
    src/addon/asr/mistral_asr.cpp
    src/addon/asr/volcengine_asr.cpp
    src/addon/asr/sherpa_onnx_asr.cpp
    src/addon/asr/asr_engine.cpp
    src/addon/asr/session_reaper.cpp
    src/addon/asr/utils/base64.cpp
    src/addon/llm/llm_client.cpp
)

# PipeWire 仅在可用时编入；PulseAudio 是必需基线。
if(PIPEWIRE_FOUND)
    list(APPEND ADDON_SOURCES src/addon/capture/pipewire_capture.cpp)
endif()
list(APPEND ADDON_SOURCES src/addon/capture/pulse_audio_capture.cpp)

set(ADDON_HEADERS
    src/addon/engine.h
    src/addon/types.h
    src/addon/config/voiceinput-config.h
    src/addon/capture/audio_capture.h
    src/addon/vad/vad.h
    src/addon/vad/silero_vad.h
    src/addon/asr/asr_engine.h
    src/addon/asr/asr_session.h
    src/addon/asr/session_reaper.h
    src/addon/pipeline/ordered_result_buffer.h
    src/addon/pipeline/result_coordinator.h
    src/addon/asr/openai_asr.h
    src/addon/asr/realtime_asr.h
    src/addon/asr/mistral_asr.h
    src/addon/asr/volcengine_asr.h
    src/addon/asr/sherpa_onnx_asr.h
    src/addon/llm/llm_client.h
    src/addon/pipeline/pipeline.h
    src/addon/utils/audio_buffer.h
    src/addon/utils/thread_safe_queue.h
    src/addon/asr/utils/base64.h
)

# 头文件仅用于 IDE 索引；PipeWire 按可用性追加。
if(PIPEWIRE_FOUND)
    list(APPEND ADDON_HEADERS src/addon/capture/pipewire_capture.h)
endif()
list(APPEND ADDON_HEADERS src/addon/capture/pulse_audio_capture.h)

# ─── Library ──────────────────────────────────────────────────────────────────
add_library(voice-input-addon MODULE ${ADDON_SOURCES} ${ADDON_HEADERS})

# fcitx5 resolves addon .so as <Library>.so (no lib prefix)
set_target_properties(voice-input-addon PROPERTIES PREFIX "")

target_include_directories(voice-input-addon PRIVATE
    ${FCITX5_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIRS}
    src/addon
)
if(SHERPA_ONNX_FOUND)
    target_include_directories(voice-input-addon PRIVATE ${SHERPA_ONNX_INCLUDE_DIRS})
    target_link_libraries(voice-input-addon PRIVATE ${SHERPA_ONNX_LIBRARIES})
    target_compile_definitions(voice-input-addon PRIVATE HAVE_SHERPA_ONNX)
    if(_sherpa_is_target)
        # submodule 模式：将生成的共享库加入 RPATH
        set_target_properties(voice-input-addon PROPERTIES
            BUILD_RPATH "$<TARGET_FILE_DIR:sherpa-onnx-c-api>"
            INSTALL_RPATH "$ORIGIN:$ORIGIN/..:${CMAKE_INSTALL_FULL_LIBDIR}"
            INSTALL_RPATH_USE_LINK_PATH TRUE
        )
    elseif(_sherpa_lib_dir)
        set_target_properties(voice-input-addon PROPERTIES
            BUILD_RPATH "${_sherpa_lib_dir}"
            INSTALL_RPATH "$ORIGIN:$ORIGIN/..:${CMAKE_INSTALL_FULL_LIBDIR}"
            INSTALL_RPATH_USE_LINK_PATH TRUE
        )
    endif()
endif()
# 录音库仅在编译期需要头文件（结构体/枚举/宏定义）；
# 运行期通过 dlopen 按需加载（见 pulse/pipewire capture 的 LoadLib），
# 因此不链接这两个库——避免 DT_NEEDED 硬依赖，库升级/soname 变更时 addon 仍可加载。
if(PIPEWIRE_FOUND)
    target_include_directories(voice-input-addon PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
endif()
target_include_directories(voice-input-addon PRIVATE ${PULSE_INCLUDE_DIRS})

target_link_libraries(voice-input-addon PRIVATE
    ${FCITX5_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${ONNXRUNTIME_LIBRARIES}
    ${CMAKE_DL_LIBS}
    CURL::libcurl
    ZLIB::ZLIB
)

target_compile_definitions(voice-input-addon PRIVATE
    VOICE_INPUT_VERSION="${PROJECT_VERSION}"
    FCITX_GETTEXT_DOMAIN="fcitx5-voice-input"
    VOICE_INPUT_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
)
if(PIPEWIRE_FOUND)
    target_compile_definitions(voice-input-addon PRIVATE HAVE_PIPEWIRE)
endif()
target_compile_definitions(voice-input-addon PRIVATE HAVE_PULSEAUDIO)

# ─── Translations ────────────────────────────────────────────────────────────
find_program(GETTEXT_MSGFMT msgfmt REQUIRED)
file(GLOB PO_FILES "${CMAKE_SOURCE_DIR}/po/*.po")
foreach(PO_FILE ${PO_FILES})
    get_filename_component(PO_LANG ${PO_FILE} NAME_WE)
    set(MO_FILE "${CMAKE_BINARY_DIR}/locale/${PO_LANG}/LC_MESSAGES/fcitx5-voice-input.mo")
    add_custom_command(
        OUTPUT ${MO_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/locale/${PO_LANG}/LC_MESSAGES"
        COMMAND ${GETTEXT_MSGFMT} -o ${MO_FILE} ${PO_FILE}
        DEPENDS ${PO_FILE}
        COMMENT "Compiling ${PO_LANG} translation"
    )
    list(APPEND MO_FILES ${MO_FILE})
    install(FILES ${MO_FILE}
        DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${PO_LANG}/LC_MESSAGES
        RENAME "fcitx5-voice-input.mo"
    )
endforeach()
add_custom_target(translations ALL DEPENDS ${MO_FILES})
add_dependencies(voice-input-addon translations)

target_compile_definitions(voice-input-addon PRIVATE
    ENABLE_SILERO_VAD
    VOICE_INPUT_MODEL_DIR="${VOICE_INPUT_MODEL_DIR}"
)
if(ONNXRUNTIME_CFLAGS_OTHER)
    target_compile_options(voice-input-addon PRIVATE ${ONNXRUNTIME_CFLAGS_OTHER})
endif()
if(ONNXRUNTIME_LIBRARY_DIRS)
    target_link_directories(voice-input-addon PRIVATE ${ONNXRUNTIME_LIBRARY_DIRS})
endif()

# ─── Addon Config ──────────────────────────────────────────────────────────────
# Generate addon .conf from .conf.in template (substitutes @PROJECT_VERSION@)
configure_file(
    src/addon/voiceinput.conf.in
    ${CMAKE_BINARY_DIR}/share/fcitx5/addon/voiceinput.conf
    @ONLY
)

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS voice-input-addon
    LIBRARY DESTINATION ${FCITX5_ADDON_DIR}
)

if(SHERPA_ONNX_FOUND AND _sherpa_is_target)
    install(TARGETS sherpa-onnx-c-api
        LIBRARY DESTINATION ${FCITX5_ADDON_DIR}
    )
endif()

install(FILES ${CMAKE_BINARY_DIR}/share/fcitx5/addon/voiceinput.conf
    DESTINATION share/fcitx5/addon
)

install(FILES ${SILERO_VAD_MODEL_SOURCE}
    DESTINATION share/fcitx5/voice-input/models
    RENAME silero_vad.onnx
)

install(FILES data/icons/fcitx_voiceinput.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

# Multi-size PNG icons for classicui compatibility (GTK icon theme lookup)
foreach(SIZE 16 22 24 32 48)
    install(FILES data/icons/hicolor/${SIZE}x${SIZE}/apps/fcitx_voiceinput.png
        DESTINATION share/icons/hicolor/${SIZE}x${SIZE}/apps
    )
endforeach()

install(FILES data/inputmethod/voiceinput.conf
    DESTINATION share/fcitx5/inputmethod
)

# ─── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Packaging ──────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "fcitx5-voice-input")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fcitx5 voice input addon")
set(CPACK_PACKAGE_VENDOR "devcxl")
set(CPACK_PACKAGE_CONTACT "64475363+devcxl@users.noreply.github.com")

# DEB package
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "devcxl <64475363+devcxl@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
# 以下三项为各发行版默认值，CI 矩阵按发行版覆盖（cmake -D 传入，见
# .github/actions/build/distro/*.sh）：
# - 依赖名随 t64 迁移/发行版变化（libcurl4t64 vs libcurl4）；
# - onnxruntime 为系统包时（Ubuntu 26.04/Debian 13/Fedora 44/openSUSE/Arch）
#   开启 SHLIBDEPS，由 dpkg-shlibdeps 自动计算运行时依赖；
# - onnxruntime 不在仓库的发行版（Ubuntu 24.04/Debian 12，CI 下载 upstream
#   release 包）无法声明 Depends，DEB 用户需自行安装 onnxruntime 运行时。
set(CPACK_DEBIAN_PACKAGE_DEPENDS "fcitx5, libjsoncpp25, libcurl4t64, zlib1g, libpulse0" CACHE STRING "DEB runtime dependencies (per-distro, set by CI)")
# PulseAudio 客户端库是基础录音能力的必需依赖；PipeWire 直连仅作可选回退。
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libpipewire-0.3-0" CACHE STRING "DEB optional PipeWire capture fallback (per-distro, set by CI)")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF CACHE BOOL "Compute DEB dependencies with dpkg-shlibdeps (ON when onnxruntime is a system package)")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/devcxl/fcitx5-voice-input")

# RPM package（Fedora/openSUSE，cpack -G RPM）
# dlopen 加载的录音库不会形成 ELF 依赖，由 CI 按发行版注入运行时包名。
set(CPACK_RPM_FILE_NAME RPM-DEFAULT) # fcitx5-voice-input-0.4.0-1.x86_64.rpm
set(CPACK_RPM_PACKAGE_LICENSE "LGPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Unspecified")
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_REQUIRES "" CACHE STRING "RPM runtime dependencies (per-distro, set by CI)")

include(CPack)
