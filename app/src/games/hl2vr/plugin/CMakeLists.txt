# PSVR2 Haptics - Half-Life 2 VR server plugin.
#
# THIS IS A SEPARATE BUILD. It is deliberately NOT added to the middleware's
# CMake project: it needs the Source SDK 2013 headers and libraries and a
# 32-bit toolchain, neither of which the middleware requires, and folding it in
# would mean nobody could build the middleware without also fetching the SDK.
#
# It has NOT been compiled. There is no Source SDK 2013 checkout and no
# Half-Life 2 VR install on the machine this was written on, so every claim
# below is "written against the published headers", not "verified to build".
# See README.md for exactly what that means and what to check first.
#
# Usage:
#   cmake -S . -B build -A Win32 -DSOURCESDK=<path to source-sdk-2013/sp/src>
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.20)
project(psvr2_haptics_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED SOURCESDK)
    message(FATAL_ERROR
        "Set -DSOURCESDK=<path to source-sdk-2013/sp/src>.\n"
        "Clone it with:\n"
        "  git clone -b singleplayer https://github.com/ValveSoftware/source-sdk-2013")
endif()

if(NOT EXISTS "${SOURCESDK}/public/engine/iserverplugin.h")
    message(FATAL_ERROR
        "${SOURCESDK} does not look like a Source SDK 2013 src directory "
        "(public/engine/iserverplugin.h is missing).")
endif()

# Half-Life 2 VR is built from Source SDK 2013 singleplayer, which is 32-bit.
# A 64-bit plugin will not load into it. This is checked rather than assumed
# because the failure is silent: the engine simply reports that it could not
# load the module.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Configure for 32-bit: cmake -A Win32 ...\n"
        "Half-Life 2 VR is a 32-bit Source SDK 2013 build and cannot load a "
        "64-bit plugin.")
endif()

add_library(psvr2_haptics_plugin SHARED
    psvr2_plugin.cpp
    psvr2_emit.cpp
    psvr2_sendprops.cpp)

target_include_directories(psvr2_haptics_plugin PRIVATE
    "${SOURCESDK}/public"
    "${SOURCESDK}/public/tier0"
    "${SOURCESDK}/public/tier1"
    "${SOURCESDK}/common"
    "${SOURCESDK}/game/shared"
    "${SOURCESDK}/game/server")

target_compile_definitions(psvr2_haptics_plugin PRIVATE
    GAME_DLL
    _DLL_EXT=.dll
    COMPILER_MSVC
    COMPILER_MSVC32
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

target_link_directories(psvr2_haptics_plugin PRIVATE "${SOURCESDK}/lib/public")

target_link_libraries(psvr2_haptics_plugin PRIVATE
    tier0
    tier1
    vstdlib
    mathlib
    ws2_32)

if(MSVC)
    target_compile_options(psvr2_haptics_plugin PRIVATE /W3 /MT)
endif()

# Named so the .vdf the middleware writes finds it.
set_target_properties(psvr2_haptics_plugin PROPERTIES
    OUTPUT_NAME psvr2_haptics_plugin
    PREFIX "")
