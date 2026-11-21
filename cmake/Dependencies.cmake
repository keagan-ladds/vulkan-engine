# =============================================================================
# Third-party dependencies
#
# System packages:
#   Vulkan headers + loader + glslc  (Linux: apt; Windows: the LunarG Vulkan SDK)
#   SDL2                             (Linux: apt libsdl2-dev; elsewhere, if no
#                                     SDL2 CMake package is found, it's built
#                                     from source, statically: nothing to install)
#   glm                              (fetched if not installed)
#
# Fetched automatically at configure time (pinned versions):
#   Vulkan Memory Allocator, Dear ImGui, cgltf, stb, miniaudio, nlohmann/json,
#   Jolt, meshoptimizer, Recast/Detour, ENet
#
# The fetched libraries are header-only or loose source files, so we compile
# them ourselves into one static library `ve_thirdparty` with warnings off.
# `SOURCE_SUBDIR _none` stops FetchContent from running their own CMake
# scripts (we only want the files).
# =============================================================================
include(FetchContent)

# CMake 4 refuses projects whose cmake_minimum_required is below 3.5 (Recast,
# ENet) and warns below 3.10 (glm, meshoptimizer). Raise those fetched
# projects' minimum to 3.10; projects already above it are unaffected.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
endif()

find_package(Vulkan REQUIRED COMPONENTS glslc)

# SDL2: the system's (apt, vcpkg, an SDL2 development package on
# CMAKE_PREFIX_PATH), else built from source. Windows has no system SDL2, so
# a plain checkout configures there with nothing but the Vulkan SDK installed.
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found: building it from source (static)")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE) # the engine has its own main (SDL_MAIN_HANDLED)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.9
        GIT_SHALLOW TRUE
        SYSTEM)
    FetchContent_MakeAvailable(SDL2)
endif()
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 ALIAS SDL2-static) # the static build's target, under the usual name
endif()

find_package(glm CONFIG QUIET)
if(NOT glm_FOUND)
    FetchContent_Declare(glm
        URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(glm)
endif()

FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.1.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _none)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _none)
FetchContent_Declare(cgltf
    URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.14.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _none)
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/f0569113c93ad095470c54bf34a17b36646bbbb5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _none)
FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.21.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _none)

FetchContent_Declare(nlohmann_json   # JSON for scene files / editor
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(vma imgui cgltf stb miniaudio nlohmann_json)

# Jolt Physics: built with its own CMake (Build/ subfolder). Only engine .cpp
# files include Jolt headers; games never see them. Jolt's SIMD flags are
# PUBLIC (they propagate to ve_engine), so we stick to SSE4.2 (any x86-64 CPU
# since ~2008) rather than AVX2, to keep the engine binary portable.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
# MSVC: Jolt defaults to the static C runtime (/MT); everything else uses the
# DLL runtime (/MD), and mixing them fails to link (LNK2038).
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
foreach(opt USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(jolt
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.2.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build
    SYSTEM)                       # treat Jolt headers as system headers (no warnings from them)
FetchContent_MakeAvailable(jolt)

# meshoptimizer: mesh simplification (LOD generation) and vertex cache/fetch
# optimisation. Private to ve_engine (used by MeshData::simplified/optimize).
FetchContent_Declare(meshoptimizer
    URL https://github.com/zeux/meshoptimizer/archive/refs/tags/v0.22.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_MakeAvailable(meshoptimizer)

# Recast & Detour: navmesh building (Recast) and path finding / crowds
# (Detour, DetourCrowd). Private to ve_engine (ve::NavMesh, ve::NavAgent).
# Fetched with git: GitHub archive downloads are blocked in some sandboxes.
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_DT_POLYREF64 ON CACHE BOOL "" FORCE)  # big tiled worlds: many tiles x polys
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(recastnavigation)
foreach(t Recast Detour DetourCrowd)
    if(MSVC)
        target_compile_options(${t} PRIVATE /w)
    else()
        target_compile_options(${t} PRIVATE -w)
    endif()
endforeach()

# ENet: reliable / unreliable UDP channels for ve/net (MIT). Fetched with git
# like Recast (archive downloads are blocked in some sandboxes).
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(enet)
if(MSVC)
    target_compile_options(enet PRIVATE /w)
else()
    target_compile_options(enet PRIVATE -w)
    set_target_properties(enet PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
target_include_directories(enet SYSTEM INTERFACE ${enet_SOURCE_DIR}/include)
if(WIN32)
    # Plain signature: ENet's own CMake links these with it under MinGW, and the
    # two signatures can't be mixed on one target. (MSVC: only this call.)
    target_link_libraries(enet ws2_32 winmm)
endif()

add_library(ve_thirdparty STATIC
    ${CMAKE_CURRENT_LIST_DIR}/thirdparty/thirdparty_impl.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(ve_thirdparty SYSTEM PUBLIC
    ${vma_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${cgltf_SOURCE_DIR}
    ${stb_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}
)
target_compile_definitions(ve_thirdparty PUBLIC
    VMA_STATIC_VULKAN_FUNCTIONS=1     # we link the Vulkan loader directly
    VMA_DYNAMIC_VULKAN_FUNCTIONS=0
)
target_link_libraries(ve_thirdparty PUBLIC Vulkan::Vulkan SDL2::SDL2 ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
target_link_libraries(ve_thirdparty PUBLIC Threads::Threads)
if(NOT WIN32)
    target_link_libraries(ve_thirdparty PUBLIC m)
endif()
# Third-party code is not ours to fix: silence its warnings.
if(MSVC)
    target_compile_options(ve_thirdparty PRIVATE /w)
else()
    target_compile_options(ve_thirdparty PRIVATE -w)
endif()
