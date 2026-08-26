cmake_minimum_required(VERSION 3.20)
project(psvr2_alyx_haptics LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "PSVR2 Alyx Haptics targets Windows + the PSVR2Toolkit CAPI.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The game-side Lua is compiled into the executable so the release is a single
# self-installing binary with no folder layout for the user to get wrong.
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

add_executable(psvr2_alyx_haptics
    src/main.cpp
    src/capi.cpp
    src/config.cpp
    src/haptics.cpp
    src/triggers.cpp
    src/router.cpp
    src/profiles.cpp
    src/install.cpp
    src/transport.cpp
    "${LUA_EMBED_HEADER}")

add_dependencies(psvr2_alyx_haptics lua_embed)

target_include_directories(psvr2_alyx_haptics PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(psvr2_alyx_haptics PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

target_link_libraries(psvr2_alyx_haptics PRIVATE shell32 ws2_32)

if(MSVC)
    target_compile_options(psvr2_alyx_haptics PRIVATE /W4 /permissive- /MP)
    # Static CRT so the release runs without a VC++ redistributable install.
    set_property(TARGET psvr2_alyx_haptics PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(psvr2_alyx_haptics PRIVATE -Wall -Wextra)
endif()
