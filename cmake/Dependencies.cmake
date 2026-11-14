# =============================================================================
# Third-party dependencies
#
# System packages (install via apt / Vulkan SDK):
#   Vulkan headers + loader + glslc, SDL2, glm
#
# Fetched automatically at configure time (pinned versions):
#   Vulkan Memory Allocator, Dear ImGui, cgltf, stb, miniaudio
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

FetchContent_MakeAvailable(vma imgui cgltf stb miniaudio)

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
