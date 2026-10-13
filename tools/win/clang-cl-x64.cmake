# CMake toolchain: Windows x64 (MSVC ABI) cross-compiled on macOS with
# LLVM clang-cl + lld-link against the MSVC CRT and Windows SDK fetched by
# `xwin` (tools/win/setup-toolchain.sh).
#
#   cmake -S <src> -B <build> -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=tools/win/clang-cl-x64.cmake
#
# Inputs (environment or -D cache variables):
#   TOUCH_CONSOLE_WINSYSROOT  the xwin splat in /winsysroot layout
#                             (default: ~/.cache/touch-console/winsysroot)
#   TOUCH_CONSOLE_LLVM        LLVM prefix with clang-cl / llvm-lib / llvm-rc / llvm-mt
#                             (default: /opt/homebrew/opt/llvm@22)
#   TOUCH_CONSOLE_LLD         lld prefix with lld-link (default: /opt/homebrew/opt/lld@22)
#   TOUCH_CONSOLE_WINE_RUN    emulator used to run built .exe helpers at build time
#                             (default: tools/win/wine-run.sh next to this file)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

macro(_tc_setting name default)
    if(NOT DEFINED ${name})
        if(DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
            set(${name} "$ENV{${name}}")
        else()
            set(${name} "${default}")
        endif()
    endif()
    # try_compile projects inherit the toolchain file but not our cache
    # entries unless they are forwarded.
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ${name})
endmacro()

_tc_setting(TOUCH_CONSOLE_WINSYSROOT "$ENV{HOME}/.cache/touch-console/winsysroot")
_tc_setting(TOUCH_CONSOLE_LLVM "/opt/homebrew/opt/llvm@22")
_tc_setting(TOUCH_CONSOLE_LLD "/opt/homebrew/opt/lld@22")
_tc_setting(TOUCH_CONSOLE_WINE_RUN "${CMAKE_CURRENT_LIST_DIR}/wine-run.sh")

if(NOT EXISTS "${TOUCH_CONSOLE_WINSYSROOT}/VC/Tools/MSVC")
    message(FATAL_ERROR "No MSVC CRT / Windows SDK at ${TOUCH_CONSOLE_WINSYSROOT}. "
        "Run: bash tools/win/setup-toolchain.sh")
endif()

set(CMAKE_C_COMPILER "${TOUCH_CONSOLE_LLVM}/bin/clang-cl")
set(CMAKE_CXX_COMPILER "${TOUCH_CONSOLE_LLVM}/bin/clang-cl")
set(CMAKE_LINKER "${TOUCH_CONSOLE_LLD}/bin/lld-link")
set(CMAKE_AR "${TOUCH_CONSOLE_LLVM}/bin/llvm-lib")
set(CMAKE_RC_COMPILER "${TOUCH_CONSOLE_LLVM}/bin/llvm-rc")
set(CMAKE_MT "${TOUCH_CONSOLE_LLVM}/bin/llvm-mt")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# /winsysroot makes clang-cl and lld-link find VC/Tools/MSVC/<ver> and
# "Windows Kits/10/{Include,Lib}/<ver>" on their own (latest versions win).
set(_tc_sysroot_compile "/winsysroot \"${TOUCH_CONSOLE_WINSYSROOT}\"")
set(_tc_sysroot_link "/winsysroot:\"${TOUCH_CONSOLE_WINSYSROOT}\"")

# set, not APPEND: CMake reads a toolchain file more than once per configure.
set(CMAKE_C_FLAGS_INIT "${_tc_sysroot_compile}")
set(CMAKE_CXX_FLAGS_INIT "${_tc_sysroot_compile}")
foreach(_kind EXE SHARED MODULE)
    set(CMAKE_${_kind}_LINKER_FLAGS_INIT "${_tc_sysroot_link}")
endforeach()

# xwin ships only the release CRT (msvcrt/libcmt, no *d.lib), so CMake's own
# compiler checks must not use the Debug configuration. Build Release or
# RelWithDebInfo; a Debug configuration would need `xwin splat --include-debug-libs`.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# The resource compiler: CMake preprocesses .rc files with clang-cl and then
# runs llvm-rc; both need the SDK headers (windows.h, winver.h).
file(GLOB _tc_sdk_include "${TOUCH_CONSOLE_WINSYSROOT}/Windows Kits/10/Include/*")
list(SORT _tc_sdk_include COMPARE NATURAL ORDER DESCENDING)
list(GET _tc_sdk_include 0 _tc_sdk_include)
file(GLOB _tc_msvc "${TOUCH_CONSOLE_WINSYSROOT}/VC/Tools/MSVC/*")
list(SORT _tc_msvc COMPARE NATURAL ORDER DESCENDING)
list(GET _tc_msvc 0 _tc_msvc)
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES
    "${_tc_sdk_include}/um" "${_tc_sdk_include}/shared" "${_tc_sdk_include}/ucrt"
    "${_tc_msvc}/include")

# Executables built for Windows (e.g. JUCE's VST3 moduleinfo helper, unit
# tests) run through wine when the build or ctest needs to execute them.
# Without wine the project skips the moduleinfo.json step (optional in VST3).
find_program(TOUCH_CONSOLE_WINE_BIN wine PATHS /opt/homebrew/bin /usr/local/bin)
if(TOUCH_CONSOLE_WINE_BIN AND EXISTS "${TOUCH_CONSOLE_WINE_RUN}")
    set(CMAKE_CROSSCOMPILING_EMULATOR "${TOUCH_CONSOLE_WINE_RUN}")
endif()

set(CMAKE_FIND_ROOT_PATH "${TOUCH_CONSOLE_WINSYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
