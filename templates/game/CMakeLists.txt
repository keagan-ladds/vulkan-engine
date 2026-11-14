# =============================================================================
# Game template for vulkan-engine.
#
# Copy this whole folder somewhere, rename "mygame", and point VE_ENGINE_DIR at
# your vulkan-engine checkout (edit the default below or pass
# -DVE_ENGINE_DIR=/path/to/vulkan-engine when configuring).
#
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ./build/bin/mygame
#
# The engine is built as part of your game (no install step); you never need
# to edit anything inside the engine folder.
# =============================================================================
cmake_minimum_required(VERSION 3.24)
project(MyGame LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # IntelliSense / clangd

# Where the engine lives. The default works while this template sits inside
# the engine repo (templates/game); change it after copying the folder out.
set(VE_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to the vulkan-engine checkout")
if(NOT EXISTS "${VE_ENGINE_DIR}/engine/src/ve/ve.hpp")
    message(FATAL_ERROR "VE_ENGINE_DIR (${VE_ENGINE_DIR}) is not a vulkan-engine checkout. "
                        "Pass -DVE_ENGINE_DIR=/path/to/vulkan-engine")
endif()
# Second argument = where the engine's build files go inside our build folder.
add_subdirectory(${VE_ENGINE_DIR} vulkan-engine)

# --- The game ------------------------------------------------------------------
# New .cpp / shader files are picked up automatically (re-run CMake configure).
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB GAME_SHADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)

add_executable(mygame ${GAME_SOURCES})
target_include_directories(mygame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Links ve_engine + ve_effects, outputs to build/bin, compiles shaders, copies assets.
ve_setup_game(mygame
    ASSETS  ${CMAKE_CURRENT_SOURCE_DIR}/assets
    SHADERS ${GAME_SHADERS})
