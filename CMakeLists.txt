# =============================================================================
# vulkan-engine
#
#   ve_engine   static library  - the core engine            (engine/)
#   ve_effects  static library  - optional visual add-ons     (effects/)
#   savanna     executable      - test scene / demo game      (sandbox/)
#
# Everything is output to <build>/bin:  bin/<game>, bin/shaders/*.spv, bin/assets/...
#
# Two ways to use this file:
#  1. Build it directly (engine development): builds the engine + the savanna test scene.
#  2. From a game's own CMakeLists (see templates/game):
#        add_subdirectory(path/to/vulkan-engine vulkan-engine)
#        add_executable(mygame ...)
#        ve_setup_game(mygame ASSETS assets SHADERS ${MY_SHADERS})
#     The sandbox is not built in that case (VE_BUILD_SANDBOX defaults to OFF).
# =============================================================================
cmake_minimum_required(VERSION 3.25) # 3.25: FetchContent SYSTEM
project(VulkanEngine VERSION 0.1.0 LANGUAGES C CXX)

option(VE_BUILD_SANDBOX "Build the savanna test scene" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Single output folder for executables + shaders + assets, in the *top-level*
# build directory (so it's the game's build folder when used as a subproject).
# CACHE INTERNAL makes the values visible to the parent project as well.
set(VE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin" CACHE INTERNAL "Where executables, shaders and assets go")
set(VE_SHADER_OUTPUT_DIR "${VE_OUTPUT_DIR}/shaders" CACHE INTERNAL "Compiled SPIR-V output folder")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)   # Vulkan, SDL2, glm, + fetched VMA/ImGui/cgltf/stb/miniaudio
include(Shaders)        # ve_compile_shaders()
include(Game)           # ve_setup_game(), ve_set_warnings()

add_subdirectory(engine)
add_subdirectory(effects)
if(VE_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()
