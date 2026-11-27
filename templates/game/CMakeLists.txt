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
# The engine comes one of two ways:
#  - prebuilt: an installed engine package (headers + libraries, made with
#    `cmake --install <engine build> --component VulkanEngine --prefix <dir>`):
#        cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<dir>
#    Your game then never compiles the engine. Use the same compiler, and a
#    package with your build type (Debug / Release; one package can hold both).
#  - from source (when no package is found): the engine checkout in
#    VE_ENGINE_DIR is built as part of your game.
# Either way you never need to edit anything inside the engine.
# =============================================================================
cmake_minimum_required(VERSION 3.26)
project(MyGame LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # IntelliSense / clangd

find_package(VulkanEngine CONFIG QUIET)
if(VulkanEngine_FOUND)
    message(STATUS "Prebuilt vulkan-engine ${VulkanEngine_VERSION}: ${VulkanEngine_DIR}")
else()
    # Where the engine checkout lives. The default works while this template sits
    # inside the engine repo (templates/game); change it after copying the folder out.
    set(VE_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to the vulkan-engine checkout")
    if(NOT EXISTS "${VE_ENGINE_DIR}/engine/src/ve/ve.hpp")
        message(FATAL_ERROR "No engine package found and VE_ENGINE_DIR (${VE_ENGINE_DIR}) is not a vulkan-engine checkout. "
                            "Pass -DCMAKE_PREFIX_PATH=<installed engine> or -DVE_ENGINE_DIR=/path/to/vulkan-engine")
    endif()
    # Second argument = where the engine's build files go inside our build folder.
    add_subdirectory(${VE_ENGINE_DIR} vulkan-engine)
endif()

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
