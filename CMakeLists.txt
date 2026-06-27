cmake_minimum_required(VERSION 3.20)
project(fcitx5-voice-input VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Include custom find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ─── Options ─────────────────────────────────────────────────────────────────
option(ENABLE_SHERPA_ONNX "Enable local ASR via sherpa-onnx" OFF)
option(ENABLE_LLM_SUPPORT "Enable LLM post-processing (libcurl)" OFF)
option(BUILD_TESTS "Build unit tests" OFF)

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
find_package(nlohmann_json REQUIRED)

# HTTP client (for OpenAI-compatible ASR)
find_package(CURL REQUIRED)

# sherpa-onnx local ASR (optional)
if(ENABLE_SHERPA_ONNX)
    pkg_check_modules(SHERPA_ONNX sherpa-onnx)
    if(NOT SHERPA_ONNX_FOUND)
        message(FATAL_ERROR "ENABLE_SHERPA_ONNX=ON but sherpa-onnx not found via pkg-config")
    endif()
endif()

# ─── Sources ──────────────────────────────────────────────────────────────────
set(ADDON_SOURCES
    src/addon/engine.cpp
    src/addon/capture/pipewire_capture.cpp
    src/addon/capture/pulse_audio_capture.cpp
    src/addon/vad/vad.cpp
    src/addon/pipeline/pipeline.cpp
    src/addon/asr/openai_asr.cpp
)

set(ADDON_HEADERS
    src/addon/engine.h
    src/addon/config/config.h
    src/addon/config/voiceinput-config.h
    src/addon/capture/audio_capture.h
    src/addon/capture/pipewire_capture.h
    src/addon/capture/pulse_audio_capture.h
    src/addon/vad/vad.h
    src/addon/asr/asr_engine.h
    src/addon/asr/openai_asr.h
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
    ${nlohmann_json_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    src/addon
)

target_link_libraries(voice-input-addon PRIVATE
    ${FCITX5_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    ${PULSE_LIBRARIES}
    CURL::libcurl
)

target_compile_definitions(voice-input-addon PRIVATE
    VOICE_INPUT_VERSION="${PROJECT_VERSION}"
    FCITX_GETTEXT_DOMAIN="fcitx5-voice-input"
    VOICE_INPUT_LOCALE_DIR="${CMAKE_INSTALL_FULL_LOCALEDIR}"
)

# ─── Translations ────────────────────────────────────────────────────────────
find_program(GETTEXT_MSGFMT msgfmt REQUIRED)
set(LOCALE_DIR "${CMAKE_INSTALL_FULL_LOCALEDIR}")
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
        DESTINATION "${LOCALE_DIR}/${PO_LANG}/LC_MESSAGES"
        RENAME "fcitx5-voice-input.mo"
    )
endforeach()
add_custom_target(translations ALL DEPENDS ${MO_FILES})
add_dependencies(voice-input-addon translations)

# ─── Optional: sherpa-onnx local ASR ─────────────────────────────────────────
if(ENABLE_SHERPA_ONNX AND SHERPA_ONNX_FOUND)
    target_sources(voice-input-addon PRIVATE
        src/addon/asr/sherpa_asr.cpp
        src/addon/asr/sherpa_asr.h
    )
    target_include_directories(voice-input-addon PRIVATE ${SHERPA_ONNX_INCLUDE_DIRS})
    target_link_libraries(voice-input-addon PRIVATE ${SHERPA_ONNX_LIBRARIES})
    target_compile_definitions(voice-input-addon PRIVATE ENABLE_SHERPA_ONNX)
endif()

if(ENABLE_LLM_SUPPORT)
    target_compile_definitions(voice-input-addon PRIVATE ENABLE_LLM_SUPPORT)
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
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${url}")

include(CPack)
