# =============================================================================
# Helpers for games built on the engine.
#
# ve_setup_game(<target>
#     [ASSETS  <dir>]          folder copied to <build>/bin/assets on every build
#     [SHADERS <files...>]     GLSL files compiled to <build>/bin/shaders/<name>.spv
#     [NO_EFFECTS]             don't link ve_effects (sky, fog, particles...)
# )
#
# Links the engine, puts the executable in <build>/bin next to the engine's
# shaders, compiles the game's shaders with access to ve.glsl / vfx.glsl,
# and copies the assets. Example (see templates/game/CMakeLists.txt):
#
#   add_executable(mygame src/main.cpp)
#   ve_setup_game(mygame ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets SHADERS ${MY_SHADERS})
#
# Note: shaders are found by file name, so a game's shader names must not
# clash with the engine's (standard.vert, sky.frag, ...). Prefix them if unsure.
# =============================================================================

# This folder (for files games pick up from the engine, e.g. the Windows manifest).
set(VE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "vulkan-engine's cmake/ folder")

# Warning flags for first-party targets (third-party code is compiled with -w).
function(ve_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    endif()
endfunction()

function(ve_setup_game TARGET)
    cmake_parse_arguments(ARG "NO_EFFECTS" "ASSETS" "SHADERS" ${ARGN})

    # Prebuilt engine (find_package(VulkanEngine)): imported ve:: targets, and
    # the engine's compiled shaders copied next to the game's.
    if(TARGET ve::ve_engine)
        target_link_libraries(${TARGET} PRIVATE ve::ve_engine)
        set(_ve_spv ${VE_PREBUILT_ENGINE_SHADERS})
        if(NOT ARG_NO_EFFECTS)
            target_link_libraries(${TARGET} PRIVATE ve::ve_effects)
            list(APPEND _ve_spv ${VE_PREBUILT_EFFECTS_SHADERS})
        endif()
        add_custom_target(${TARGET}_engine_shaders
            COMMAND ${CMAKE_COMMAND} -E make_directory ${VE_SHADER_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ve_spv} ${VE_SHADER_OUTPUT_DIR}
            COMMENT "Copying prebuilt engine shaders"
            VERBATIM)
        add_dependencies(${TARGET} ${TARGET}_engine_shaders)
    else()
        target_link_libraries(${TARGET} PRIVATE ve_engine)
        if(NOT ARG_NO_EFFECTS)
            target_link_libraries(${TARGET} PRIVATE ve_effects)
        endif()
    endif()
    ve_set_warnings(${TARGET})
    # $<1:...> stops multi-config generators (Visual Studio) adding Debug/ Release/.
    set_target_properties(${TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${VE_OUTPUT_DIR}>")

    if(WIN32)
        # UTF-8 as the process code page (see cmake/windows/ve.manifest).
        if(MSVC)
            target_sources(${TARGET} PRIVATE ${VE_CMAKE_DIR}/windows/ve.manifest)
        else()
            target_sources(${TARGET} PRIVATE ${VE_CMAKE_DIR}/windows/ve.rc)
        endif()
        # MinGW: link its C++ runtime (libstdc++, libgcc, winpthread) into the
        # executable, so it runs without MinGW's DLLs next to it.
        if(MINGW)
            target_link_options(${TARGET} PRIVATE -static)
        endif()
        # A shared SDL2 (vcpkg, a development package) must sit next to the
        # executable. The SDL2 built from source is static: then there's nothing to copy.
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${TARGET}>>,copy_if_different,true>
                    $<TARGET_RUNTIME_DLLS:${TARGET}> $<TARGET_FILE_DIR:${TARGET}>
            COMMAND_EXPAND_LISTS)
    endif()

    if(ARG_SHADERS)
        ve_compile_shaders(${TARGET} SOURCES ${ARG_SHADERS}
            INCLUDE_DIRS ${VE_ENGINE_SHADER_INCLUDE} ${VE_EFFECTS_SHADER_INCLUDE})
    endif()

    if(ARG_ASSETS)
        get_filename_component(ASSET_DIR "${ARG_ASSETS}" ABSOLUTE)
        # Lets the engine (editor) read and save assets in the source folder,
        # not the copy in bin/ that the next build would overwrite.
        file(GENERATE OUTPUT ${VE_OUTPUT_DIR}/ve-source-assets.txt CONTENT "${ASSET_DIR}")
        add_custom_target(${TARGET}_assets
            COMMAND ${CMAKE_COMMAND} -E make_directory ${VE_OUTPUT_DIR}/assets
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${ASSET_DIR} ${VE_OUTPUT_DIR}/assets
            COMMENT "Copying ${TARGET} assets")
        add_dependencies(${TARGET} ${TARGET}_assets)
    endif()
endfunction()
