# =============================================================================
# Installable engine package: headers + static libraries + tools + shaders.
#
#   cmake --build build --config Release
#   cmake --install build --config Release --component VulkanEngine --prefix <dir>
#
# installs a relocatable package (copy or zip <dir> anywhere):
#
#   <dir>/include/ve/...                 engine headers (not the *Internal.hpp ones)
#   <dir>/include/vfx/...                effects headers
#   <dir>/include/glm, nlohmann, ...     third-party headers the engine's headers use
#   <dir>/lib/*.a | *.lib                the engine, effects and every library they link
#   <dir>/lib/cmake/VulkanEngine/        find_package(VulkanEngine) files, ve_setup_game()
#   <dir>/bin/vecook                     the cook / package tool
#   <dir>/share/vulkan-engine/shaders/   compiled engine / effects shaders, and the
#                                        GLSL includes (ve.glsl, vfx.glsl) for game shaders
#
# A game uses it with find_package (templates/game does when it finds one):
#
#   cmake -S mygame -B build -DCMAKE_PREFIX_PATH=<dir>
#   # CMakeLists.txt: find_package(VulkanEngine REQUIRED) + ve_setup_game(mygame ...)
#
# Install a Debug and a Release build into the same <dir> to get both (Debug
# libraries carry a "d" suffix); the game links the one matching its build type.
# The game needs the same compiler (MSVC vs MinGW GCC libraries don't mix) and
# the Vulkan SDK (headers, loader, glslc for its own shaders).
#
# (--component VulkanEngine: the bundled third-party projects add install rules
# of their own; they're not part of the package.)
# =============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(PackageUtils)

set(_ve_component VulkanEngine)
set(_ve_cmake_dest "${CMAKE_INSTALL_LIBDIR}/cmake/VulkanEngine")
set(_ve_share "${CMAKE_INSTALL_DATADIR}/vulkan-engine")

# --- Targets -------------------------------------------------------------------------
# Everything ve_engine / ve_effects link (static libraries need all of it),
# made installable: include paths that point into this build become
# BUILD_INTERFACE-only, and the targets whose headers games use get the
# package's include/ folder instead.
_ve_link_closure(_ve_install_targets ve_engine ve_effects)
set(_ve_public_header_targets ve_engine ve_effects ve_thirdparty glm glm-header-only nlohmann_json)
foreach(t IN LISTS _ve_install_targets)
    get_target_property(type ${t} TYPE)
    foreach(prop INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
        get_target_property(dirs ${t} ${prop})
        if(NOT dirs)
            continue()
        endif()
        set(fixed "")
        foreach(d IN LISTS dirs)
            if(d MATCHES "^\\$<")
                list(APPEND fixed "${d}") # already says what to do when installed
            else()
                list(APPEND fixed "$<BUILD_INTERFACE:${d}>")
            endif()
        endforeach()
        if(t IN_LIST _ve_public_header_targets)
            list(APPEND fixed "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
        endif()
        set_target_properties(${t} PROPERTIES ${prop} "${fixed}")
    endforeach()
    # Interface sources (nlohmann_json adds its .natvis debugger file on MSVC)
    # are for builds of this tree: the package doesn't carry them.
    get_target_property(srcs ${t} INTERFACE_SOURCES)
    if(srcs)
        set(fixed "")
        foreach(s IN LISTS srcs)
            if(s MATCHES "^\\$<")
                list(APPEND fixed "${s}")
            else()
                list(APPEND fixed "$<BUILD_INTERFACE:${s}>")
            endif()
        endforeach()
        set_target_properties(${t} PROPERTIES INTERFACE_SOURCES "${fixed}")
    endif()
    # Debug and Release builds can share one package folder.
    if(NOT type STREQUAL "INTERFACE_LIBRARY")
        get_target_property(postfix ${t} DEBUG_POSTFIX)
        if(NOT postfix)
            set_target_properties(${t} PROPERTIES DEBUG_POSTFIX d)
        endif()
    endif()
endforeach()

install(TARGETS ${_ve_install_targets}
    EXPORT VulkanEngineTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${_ve_component}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ${_ve_component}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${_ve_component})
install(EXPORT VulkanEngineTargets
    NAMESPACE ve::
    DESTINATION ${_ve_cmake_dest}
    COMPONENT ${_ve_component})
if(TARGET vecook)
    install(TARGETS vecook RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${_ve_component})
endif()

# --- Headers ---------------------------------------------------------------------------
install(DIRECTORY ${PROJECT_SOURCE_DIR}/engine/src/ ${PROJECT_SOURCE_DIR}/effects/src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT ${_ve_component}
    FILES_MATCHING PATTERN "*.hpp"
    PATTERN "*Internal.hpp" EXCLUDE)
# Third-party headers the engine's public headers include: glm (unless the
# system's is used: then the game finds it too), nlohmann/json, VMA, Dear ImGui.
if(TARGET glm AND NOT glm_FOUND)
    install(DIRECTORY ${glm_SOURCE_DIR}/glm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT ${_ve_component}
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.inl" PATTERN "*.h")
endif()
# SDL2 built from source: its target points games at include/SDL2 (its
# headers, plus the ones its build generates: SDL_config.h, SDL_revision.h).
if(TARGET SDL2-static AND NOT SDL2_FOUND)
    install(DIRECTORY ${sdl2_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SDL2 COMPONENT ${_ve_component}
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY ${sdl2_BINARY_DIR}/include/SDL2/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SDL2 COMPONENT ${_ve_component}
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY ${sdl2_BINARY_DIR}/include-config-$<LOWER_CASE:$<CONFIG>>/SDL2/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SDL2
        COMPONENT ${_ve_component} FILES_MATCHING PATTERN "*.h")
endif()
install(DIRECTORY ${nlohmann_json_SOURCE_DIR}/include/nlohmann DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT ${_ve_component})
install(FILES ${vma_SOURCE_DIR}/include/vk_mem_alloc.h ${imgui_SOURCE_DIR}/imgui.h ${imgui_SOURCE_DIR}/imconfig.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT ${_ve_component})

# --- Shaders ---------------------------------------------------------------------------
get_property(_ve_engine_spv GLOBAL PROPERTY VE_SHADER_OUTPUTS_ve_engine)
get_property(_ve_effects_spv GLOBAL PROPERTY VE_SHADER_OUTPUTS_ve_effects)
install(FILES ${_ve_engine_spv} DESTINATION ${_ve_share}/shaders/engine COMPONENT ${_ve_component})
if(_ve_effects_spv)
    install(FILES ${_ve_effects_spv} DESTINATION ${_ve_share}/shaders/effects COMPONENT ${_ve_component})
endif()
install(DIRECTORY ${VE_ENGINE_SHADER_INCLUDE}/ ${VE_EFFECTS_SHADER_INCLUDE}/
    DESTINATION ${_ve_share}/shaders/include COMPONENT ${_ve_component})

# --- CMake package ---------------------------------------------------------------------
install(FILES ${PROJECT_SOURCE_DIR}/cmake/Shaders.cmake ${PROJECT_SOURCE_DIR}/cmake/Game.cmake
    DESTINATION ${_ve_cmake_dest} COMPONENT ${_ve_component})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/cmake/windows DESTINATION ${_ve_cmake_dest} COMPONENT ${_ve_component})

set(VE_INSTALL_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(VE_INSTALL_SYSTEM_SDL2 OFF)
if(SDL2_FOUND)
    set(VE_INSTALL_SYSTEM_SDL2 ON)
endif()
set(VE_INSTALL_SYSTEM_GLM OFF)
if(glm_FOUND)
    set(VE_INSTALL_SYSTEM_GLM ON)
endif()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/install/VulkanEngineConfig.cmake" @ONLY CONTENT [=[
# Generated by vulkan-engine (cmake/Install.cmake). Do not edit.
# An installed engine package: find_package(VulkanEngine) + ve_setup_game().
if(TARGET ve::ve_engine)
    return()
endif()

get_filename_component(_ve_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(VulkanEngine_VERSION "@PROJECT_VERSION@")
set(VulkanEngine_PREFIX "${_ve_prefix}")

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "@VE_INSTALL_COMPILER_ID@")
    message(FATAL_ERROR "VulkanEngine at ${_ve_prefix} was built with @VE_INSTALL_COMPILER_ID@, "
                        "this project uses ${CMAKE_CXX_COMPILER_ID}: the libraries can't be mixed.")
endif()

include(CMakeFindDependencyMacro)
find_dependency(Vulkan COMPONENTS glslc)
find_dependency(Threads)
if(@VE_INSTALL_SYSTEM_SDL2@)
    find_dependency(SDL2 CONFIG)
endif()
if(@VE_INSTALL_SYSTEM_GLM@)
    find_dependency(glm CONFIG)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/VulkanEngineTargets.cmake")

# What ve_setup_game() / ve_compile_shaders() expect: everything goes to the
# game's own <build>/bin (executable, shaders/, assets/).
set(VE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(VE_SHADER_OUTPUT_DIR "${VE_OUTPUT_DIR}/shaders")
set(VE_ENGINE_SHADER_INCLUDE "${_ve_prefix}/@CMAKE_INSTALL_DATADIR@/vulkan-engine/shaders/include")
set(VE_EFFECTS_SHADER_INCLUDE "${VE_ENGINE_SHADER_INCLUDE}")
file(GLOB VE_PREBUILT_ENGINE_SHADERS "${_ve_prefix}/@CMAKE_INSTALL_DATADIR@/vulkan-engine/shaders/engine/*.spv")
file(GLOB VE_PREBUILT_EFFECTS_SHADERS "${_ve_prefix}/@CMAKE_INSTALL_DATADIR@/vulkan-engine/shaders/effects/*.spv")
set(VE_PREBUILT_VECOOK "${_ve_prefix}/@CMAKE_INSTALL_BINDIR@/vecook${CMAKE_EXECUTABLE_SUFFIX}")
include("${CMAKE_CURRENT_LIST_DIR}/Shaders.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Game.cmake")
set(VulkanEngine_FOUND TRUE)
]=])
write_basic_package_version_file("${CMAKE_BINARY_DIR}/install/VulkanEngineConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_BINARY_DIR}/install/VulkanEngineConfig.cmake" "${CMAKE_BINARY_DIR}/install/VulkanEngineConfigVersion.cmake"
    DESTINATION ${_ve_cmake_dest} COMPONENT ${_ve_component})
