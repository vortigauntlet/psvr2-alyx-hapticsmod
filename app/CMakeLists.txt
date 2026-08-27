cmake_minimum_required(VERSION 3.20)
project(psvr2_haptics LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "PSVR2 Haptics targets Windows + the PSVR2Toolkit CAPI.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Half-Life: Alyx game-side Lua is compiled into the executable so the
# release is a single self-installing binary with no folder layout for the user
# to get wrong. Half-Life 2 VR's game side is a compiled Source server plugin
# and cannot work the same way - see src/games/hl2vr/plugin/README.md.
set(LUA_EMBED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/lua_embed.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(ADDON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../addon")
file(GLOB_RECURSE ADDON_SOURCES CONFIGURE_DEPENDS "${ADDON_DIR}/*.lua")

add_custom_command(
    OUTPUT "${LUA_EMBED_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            -DADDON_DIR=${ADDON_DIR}
            -DOUT=${LUA_EMBED_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateLuaEmbed.cmake"
    DEPENDS ${ADDON_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateLuaEmbed.cmake"
    COMMENT "Embedding Half-Life: Alyx addon scripts"
    VERBATIM)

add_custom_target(lua_embed DEPENDS "${LUA_EMBED_HEADER}")

# Game-agnostic haptics core. Nothing in here knows which game is running.
set(CORE_SOURCES
    src/core/bhaptics_listener.cpp
    src/core/capi.cpp
    src/core/config.cpp
    src/core/events.cpp
    src/core/haptics.cpp
    src/core/hmd.cpp
    src/core/impact.cpp
    src/core/materials.cpp
    src/core/profiles.cpp
    src/core/transport.cpp
    src/core/triggers.cpp
    src/core/variation.cpp)

# Half-Life: Alyx integration.
set(ALYX_SOURCES
    src/games/alyx/alyx_adapter.cpp
    src/games/alyx/alyx_install.cpp
    src/games/alyx/netconsole.cpp)

# Half-Life 2 VR integration. The middleware half only - the game-side server
# plugin is a separate build against the Source SDK, in src/games/hl2vr/plugin.
set(HL2VR_SOURCES
    src/games/hl2vr/hl2vr_adapter.cpp
    src/games/hl2vr/hl2vr_bhaptics.cpp
    src/games/hl2vr/hl2vr_install.cpp
    src/games/hl2vr/hl2vr_surfaces.cpp)

add_executable(psvr2_haptics
    src/main.cpp
    ${CORE_SOURCES}
    ${ALYX_SOURCES}
    ${HL2VR_SOURCES}
    "${LUA_EMBED_HEADER}")

# The executable was called psvr2_alyx_haptics for its whole life as a
# single-game build. Keep that name so existing shortcuts, batch files and the
# packaging script are not silently broken by a rename that buys nothing.
set_target_properties(psvr2_haptics PROPERTIES OUTPUT_NAME psvr2_alyx_haptics)

add_dependencies(psvr2_haptics lua_embed)

source_group("core" FILES ${CORE_SOURCES})
source_group("games\\\\alyx" FILES ${ALYX_SOURCES})
source_group("games\\\\hl2vr" FILES ${HL2VR_SOURCES})

target_include_directories(psvr2_haptics PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(psvr2_haptics PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

target_link_libraries(psvr2_haptics PRIVATE shell32 ws2_32)

if(MSVC)
    target_compile_options(psvr2_haptics PRIVATE /W4 /permissive- /MP)
    # Static CRT so the release runs without a VC++ redistributable install.
    set_property(TARGET psvr2_haptics PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(psvr2_haptics PRIVATE -Wall -Wextra)
endif()
