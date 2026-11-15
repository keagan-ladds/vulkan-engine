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

    target_link_libraries(${TARGET} PRIVATE ve_engine)
    if(NOT ARG_NO_EFFECTS)
        target_link_libraries(${TARGET} PRIVATE ve_effects)
    endif()
    ve_set_warnings(${TARGET})
    # $<1:...> stops multi-config generators (Visual Studio) adding Debug/ Release/.
    set_target_properties(${TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${VE_OUTPUT_DIR}>")

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
