cmake_minimum_required(VERSION 3.20)
project(fcitx5-voice-input VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Include custom find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ─── Options ─────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)
set(ONNXRUNTIME_ROOT "" CACHE PATH "Path to an ONNX Runtime installation")

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

# PipeWire audio capture
pkg_check_modules(PIPEWIRE QUIET pipewire-0.3)
if(NOT PIPEWIRE_FOUND)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
endif()

# PulseAudio fallback capture
pkg_check_modules(PULSE REQUIRED libpulse-simple libpulse)

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

# ─── Sources ──────────────────────────────────────────────────────────────────
set(ADDON_SOURCES
    src/addon/engine.cpp
    src/addon/capture/pipewire_capture.cpp
    src/addon/capture/pulse_audio_capture.cpp
    src/addon/vad/vad.cpp
    src/addon/vad/silero_vad.cpp
    src/addon/pipeline/pipeline.cpp
    src/addon/asr/openai_asr.cpp
    src/addon/asr/volcengine_asr.cpp
    src/addon/asr/asr_engine.cpp
    src/addon/asr/session_reaper.cpp
    src/addon/llm/llm_client.cpp
)

set(ADDON_HEADERS
    src/addon/engine.h
    src/addon/types.h
    src/addon/config/voiceinput-config.h
    src/addon/capture/audio_capture.h
    src/addon/capture/pipewire_capture.h
    src/addon/capture/pulse_audio_capture.h
    src/addon/vad/vad.h
    src/addon/vad/silero_vad.h
    src/addon/asr/asr_engine.h
    src/addon/asr/asr_session.h
    src/addon/asr/session_reaper.h
    src/addon/asr/openai_asr.h
    src/addon/asr/volcengine_asr.h
    src/addon/llm/llm_client.h
    src/addon/pipeline/pipeline.h
    src/addon/utils/audio_buffer.h
    src/addon/utils/thread_safe_queue.h
)

# ─── Library ──────────────────────────────────────────────────────────────────
add_library(voice-input-addon MODULE ${ADDON_SOURCES} ${ADDON_HEADERS})

# fcitx5 resolves addon .so as <Library>.so (no lib prefix)
set_target_properties(voice-input-addon PROPERTIES PREFIX "")

target_include_directories(voice-input-addon PRIVATE
    ${FCITX5_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${PULSE_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIRS}
    src/addon
)

target_link_libraries(voice-input-addon PRIVATE
    ${FCITX5_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    ${PULSE_LIBRARIES}
    ${JSONCPP_LIBRARIES}
    ${ONNXRUNTIME_LIBRARIES}
    CURL::libcurl
    ZLIB::ZLIB
)

target_compile_definitions(voice-input-addon PRIVATE
    VOICE_INPUT_VERSION="${PROJECT_VERSION}"
    FCITX_GETTEXT_DOMAIN="fcitx5-voice-input"
    VOICE_INPUT_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
)

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

install(FILES ${CMAKE_BINARY_DIR}/share/fcitx5/addon/voiceinput.conf
    DESTINATION share/fcitx5/addon
)

install(FILES ${SILERO_VAD_MODEL_SOURCE}
    DESTINATION share/fcitx5/voice-input/models
    RENAME silero_vad.onnx
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
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/devcxl/fcitx5-voice-input")

include(CPack)
