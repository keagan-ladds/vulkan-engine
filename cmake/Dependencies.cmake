# =============================================================================
# Third-party dependencies
#
# System packages (install via apt / Vulkan SDK):
#   Vulkan headers + loader + glslc, SDL2, glm
#
# Fetched automatically at configure time (pinned versions):
#   Vulkan Memory Allocator, Dear ImGui, cgltf, stb, miniaudio, nlohmann/json
#
# The fetched libraries are header-only or loose source files, so we compile
# them ourselves into one static library `ve_thirdparty` with warnings off.
# `SOURCE_SUBDIR _none` stops FetchContent from running their own CMake
# scripts (we only want the files).
# =============================================================================
include(FetchContent)

find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(SDL2 REQUIRED)

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
