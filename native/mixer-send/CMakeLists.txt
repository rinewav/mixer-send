# Mixer SEND - Copyright (C) 2026 Covo Design
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.22)

# Before project(), or it is ignored: 1.0.0 set it below project() and shipped
# needing macOS 26 (the SDK's own version). A build tree configured then holds
# an empty cache entry, which a plain CACHE set would keep.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

project(MIXER_SEND VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Installing is a separate, deliberate step (tools/install-mixer-send.sh), so a
# build never replaces the copy a DAW is using. Turn this on only for a quick
# local loop.
option(MIXER_SEND_COPY_PLUGIN "Copy the built plugin into the user plugin folders" OFF)

# Sanitised unit tests (core + bus only):  -DMIXER_SEND_SANITIZE=address  or  =thread
set(MIXER_SEND_SANITIZE "" CACHE STRING "Build the core and its tests with -fsanitize=<value>")

if(APPLE)
    # Universal, decided here and FORCEd: set later inside a plugin block, the
    # first configure creates an empty cache entry and the plugin silently
    # becomes arm64-only, which a host with an x86_64 scanning process (or one
    # running under Rosetta) then cannot see at all.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures" FORCE)
endif()

if(WIN32)
    # Static CRT: a plugin DLL must not depend on whichever Visual C++
    # redistributable the user's machine (or DAW) happens to have.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Windows x64 is cross-compiled on macOS with clang-cl + lld-link
# (tools/win/clang-cl-x64.cmake, `pnpm mixer-send:build:win`); the harness
# measures with macOS-only APIs (mach, malloc_logger) and AU exists only there.
if(APPLE)
    set(MIXER_SEND_FORMATS VST3 AU)
else()
    set(MIXER_SEND_FORMATS VST3)
endif()

# JUCE writes Contents/Resources/moduleinfo.json by RUNNING a helper built for
# the target. Cross-compiling, that needs an emulator (wine, set by the
# toolchain file); without one the step is skipped (the file is optional:
# hosts then load the DLL to read the factory).
set(MIXER_SEND_VST3_MANIFEST TRUE)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(MIXER_SEND_VST3_MANIFEST FALSE)
    message(STATUS "Mixer SEND: cross-compiling without an emulator, no moduleinfo.json")
endif()

# ---------------------------------------------------------------------------
# Core: DSP, fader law, wire codec, the process-wide send bus. No JUCE, so the
# unit tests exercise exactly the code the plugin runs, without a host.
# ---------------------------------------------------------------------------
add_library(mixer_send_core STATIC
    src/core/FaderLaw.cpp
    src/core/GainStage.cpp
    src/core/WireProtocol.cpp
    src/core/InstanceId.cpp
    src/bus/UdpEndpoint.cpp
    src/bus/SendBus.cpp
)
target_include_directories(mixer_send_core PUBLIC src)
target_compile_features(mixer_send_core PUBLIC cxx_std_20)
target_compile_definitions(mixer_send_core PUBLIC
    MIXER_SEND_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MIXER_SEND_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MIXER_SEND_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
# A static library linked into a bundle must be position independent.
set_target_properties(mixer_send_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    target_link_libraries(mixer_send_core PUBLIC ${ACCELERATE_FRAMEWORK})
endif()
if(WIN32)
    target_link_libraries(mixer_send_core PUBLIC ws2_32)
    # getenv / snprintf are used deliberately (bounded, read-only).
    target_compile_definitions(mixer_send_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl: /Wall would mean -Weverything.
    target_compile_options(mixer_send_core PRIVATE /W4 -Wshadow -Wconversion -Wno-sign-conversion)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(mixer_send_core PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# ---------------------------------------------------------------------------
# JUCE
# ---------------------------------------------------------------------------
if(DEFINED ENV{JUCE_PATH})
    set(JUCE_PATH_DEFAULT "$ENV{JUCE_PATH}")
else()
    set(JUCE_PATH_DEFAULT "$ENV{HOME}/JUCE")
endif()
set(JUCE_PATH "${JUCE_PATH_DEFAULT}" CACHE PATH "Path to the JUCE repository")
if(NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_PATH}. Set -DJUCE_PATH=/path/to/JUCE")
endif()
add_subdirectory("${JUCE_PATH}" JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Unit tests. The golden-vector test reads the SAME fixture the TypeScript
# tests read, so the two codecs cannot drift apart unnoticed.
# ---------------------------------------------------------------------------
enable_testing()
set(MIXER_SEND_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../packages/protocol/fixtures")

juce_add_console_app(mixer_send_tests PRODUCT_NAME "mixer_send_tests")
target_sources(mixer_send_tests PRIVATE
    tests/test_main.cpp
    tests/test_fader_law.cpp
    tests/test_dsp.cpp
    tests/test_wire.cpp
    tests/test_bus.cpp
)
target_include_directories(mixer_send_tests PRIVATE tests)
target_compile_definitions(mixer_send_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    MIXER_SEND_FIXTURE_DIR="${MIXER_SEND_FIXTURE_DIR}"
)
target_link_libraries(mixer_send_tests PRIVATE mixer_send_core juce::juce_core)
if(WIN32)
    target_compile_definitions(mixer_send_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME mixer_send_tests COMMAND mixer_send_tests)

if(MIXER_SEND_SANITIZE)
    set(MIXER_SEND_SANITIZE_FLAGS -fsanitize=${MIXER_SEND_SANITIZE} -fno-omit-frame-pointer -g)
    if(MIXER_SEND_SANITIZE STREQUAL "address")
        list(APPEND MIXER_SEND_SANITIZE_FLAGS -fsanitize=undefined)
    endif()
    target_compile_options(mixer_send_core PUBLIC ${MIXER_SEND_SANITIZE_FLAGS})
    target_link_options(mixer_send_core PUBLIC ${MIXER_SEND_SANITIZE_FLAGS})
endif()

# ---------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------
set(MIXER_SEND_PLUGIN_SOURCES
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/Parameters.cpp
)

juce_add_plugin(MixerSend
    COMPANY_NAME "Covo Design"
    COMPANY_WEBSITE "https://covodesign.net"
    BUNDLE_ID "net.covodesign.plugins.mixersend"
    PLUGIN_MANUFACTURER_CODE Covo
    PLUGIN_CODE Msnd
    FORMATS ${MIXER_SEND_FORMATS}
    PRODUCT_NAME "Mixer SEND"
    DESCRIPTION "Volume and pan for Touch Console"
    VST3_CATEGORIES "Fx" "Tools"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
    # Not "sandboxSafe": that key tells a sandboxed AU host it may load the
    # plugin as-is with no extra resources, and JUCE writes it INSTEAD of the
    # resourceUsage block. Mixer SEND needs network.client (its loopback UDP
    # socket), which is exactly what JUCE's default resourceUsage declares.
    AU_SANDBOX_SAFE FALSE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${MIXER_SEND_COPY_PLUGIN}
    VST3_AUTO_MANIFEST ${MIXER_SEND_VST3_MANIFEST}
)

target_sources(MixerSend PRIVATE ${MIXER_SEND_PLUGIN_SOURCES})

set(MIXER_SEND_JUCE_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
)

target_compile_definitions(MixerSend PUBLIC ${MIXER_SEND_JUCE_DEFINITIONS})

target_link_libraries(MixerSend
    PRIVATE
        mixer_send_core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
# UI snapshot: renders the editor off-screen at 1x and 2x to PNG, so the look
# is checked on every build without a DAW or a screen.
# ---------------------------------------------------------------------------
juce_add_console_app(mixer_send_snapshot PRODUCT_NAME "mixer_send_snapshot")
target_sources(mixer_send_snapshot PRIVATE tools/ui_snapshot.cpp ${MIXER_SEND_PLUGIN_SOURCES})
target_compile_definitions(mixer_send_snapshot PRIVATE
    ${MIXER_SEND_JUCE_DEFINITIONS}
    JUCE_STANDALONE_APPLICATION=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Build_VST3=0
    JucePlugin_Build_AU=0
    JucePlugin_Build_Standalone=0
    JucePlugin_Name="Mixer SEND"
    JucePlugin_VersionString="${PROJECT_VERSION}"
)
target_link_libraries(mixer_send_snapshot PRIVATE mixer_send_core juce::juce_audio_utils)

# ---------------------------------------------------------------------------
# Host harness: loads the BUILT VST3 / AU the way a DAW does, runs many
# instances against a fake companion and measures them.
# ---------------------------------------------------------------------------
# Mini host: the portable subset (load, name, process, remote edits through
# the message loop, state round trip). Built everywhere; on the Mac it runs the
# Windows build under wine (tools/win/e2e-mixer-send-wine.ts).
juce_add_console_app(mixer_send_mini_host PRODUCT_NAME "mixer_send_mini_host")
target_sources(mixer_send_mini_host PRIVATE tools/mini_host.cpp)
target_compile_definitions(mixer_send_mini_host PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
)
target_link_libraries(mixer_send_mini_host PRIVATE juce::juce_audio_processors)

if(APPLE)
    juce_add_console_app(mixer_send_harness PRODUCT_NAME "mixer_send_harness" PLUGINHOST_AU TRUE)
    target_sources(mixer_send_harness PRIVATE tools/host_harness.cpp)
    target_compile_definitions(mixer_send_harness PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1
    )
    target_link_libraries(mixer_send_harness PRIVATE mixer_send_core juce::juce_audio_utils)
endif()
