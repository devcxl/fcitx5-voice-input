cmake_minimum_required(VERSION 3.20)
project(fcitx5-voice-input VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ─── Options ─────────────────────────────────────────────────────────────────
option(ENABLE_LLM_SUPPORT "Enable LLM post-processing (libcurl)" OFF)
option(ENABLE_CLOUD_ASR "Enable cloud ASR providers (command engine)" OFF)
option(BUILD_TESTS "Build unit tests" OFF)

# ─── Dependencies ─────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# Fcitx5 core
pkg_check_modules(FCITX5 REQUIRED fcitx5)

# PipeWire audio capture
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

# JSON parsing
find_package(nlohmann_json REQUIRED)

# ─── Sources ──────────────────────────────────────────────────────────────────
set(ADDON_SOURCES
    src/addon/engine.cpp
    src/addon/config/config.cpp
    src/addon/capture/pipewire_capture.cpp
    src/addon/vad/vad.cpp
    src/addon/asr/sherpa_asr.cpp
    src/addon/pipeline/pipeline.cpp
)

set(ADDON_HEADERS
    src/addon/engine.h
    src/addon/config/config.h
    src/addon/capture/pipewire_capture.h
    src/addon/vad/vad.h
    src/addon/asr/asr_engine.h
    src/addon/asr/sherpa_asr.h
    src/addon/pipeline/pipeline.h
    src/addon/utils/audio_buffer.h
    src/addon/utils/thread_safe_queue.h
)

# ─── Library ──────────────────────────────────────────────────────────────────
add_library(voice-input-addon SHARED ${ADDON_SOURCES} ${ADDON_HEADERS})

target_include_directories(voice-input-addon PRIVATE
    ${FCITX5_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${nlohmann_json_INCLUDE_DIRS}
    src/addon
)

target_link_libraries(voice-input-addon PRIVATE
    ${FCITX5_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
)

target_compile_definitions(voice-input-addon PRIVATE
    VOICE_INPUT_VERSION="${PROJECT_VERSION}"
)

if(ENABLE_LLM_SUPPORT)
    find_package(CURL REQUIRED)
    target_link_libraries(voice-input-addon PRIVATE CURL::libcurl)
    target_compile_definitions(voice-input-addon PRIVATE ENABLE_LLM_SUPPORT)
endif()

if(ENABLE_CLOUD_ASR)
    target_compile_definitions(voice-input-addon PRIVATE ENABLE_CLOUD_ASR)
endif()

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS voice-input-addon
    LIBRARY DESTINATION lib/fcitx5
)

install(FILES src/addon/config/voice-input-addon.conf
    DESTINATION share/fcitx5/addon
)

install(DIRECTORY models/
    DESTINATION share/fcitx5-voice-input/models
    OPTIONAL
)

# ─── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
