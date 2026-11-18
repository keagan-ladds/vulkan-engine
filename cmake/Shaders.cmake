# =============================================================================
# Uses the glslc path cached by find_package(Vulkan) rather than the
# Vulkan::glslc imported target: imported targets are only visible in the
# directory that created them, and games call this from their own directory.
#
# ve_compile_shaders(<target>
#     SOURCES      <glsl files...>        (.vert .frag .comp ...)
#     INCLUDE_DIRS <dirs...>              (searched by #include "...")
# )
#
# Compiles each GLSL file to <bin>/shaders/<filename>.spv (e.g. standard.frag
# -> standard.frag.spv) and makes <target> depend on the results. Shader file
# names must therefore be unique across the engine, effects and game.
#
# glslc writes a depfile (-MD) listing every #included file, so editing
# ve.glsl correctly rebuilds every shader that includes it. Next to each .spv
# go <name>.spv.cmd (the command) and <name>.spv.d (the depfile), which the
# running app uses to recompile edited shaders (shader hot-reload).
# =============================================================================
set(VE_SHADER_DEFINES "" CACHE STRING "Extra preprocessor defines for every shader (list; perf experiments, see docs/08-perf-tests.md)")

function(ve_compile_shaders TARGET)
    cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDE_DIRS" ${ARGN})

    set(INCLUDE_FLAGS "")
    foreach(DIR ${ARG_INCLUDE_DIRS})
        list(APPEND INCLUDE_FLAGS "-I${DIR}")
    endforeach()

    set(OUTPUTS "")
    foreach(SRC ${ARG_SOURCES})
        get_filename_component(SRC_ABS "${SRC}" ABSOLUTE)
        get_filename_component(NAME "${SRC}" NAME)
        set(OUT "${VE_SHADER_OUTPUT_DIR}/${NAME}.spv")
        # Stage macro so shared includes can expose stage-specific helpers
        # (e.g. ve.glsl's instancing functions use gl_InstanceIndex: vertex only).
        get_filename_component(EXT "${SRC}" LAST_EXT)
        set(STAGE_DEFINE "")
        if(EXT STREQUAL ".vert")
            set(STAGE_DEFINE "-DVE_VERTEX_STAGE")
        elseif(EXT STREQUAL ".frag")
            set(STAGE_DEFINE "-DVE_FRAGMENT_STAGE")
        elseif(EXT STREQUAL ".comp")
            set(STAGE_DEFINE "-DVE_COMPUTE_STAGE")
        endif()
        # Extra defines for every shader (-DVE_SHADER_DEFINES="A;B=1"): perf experiments.
        set(EXTRA_DEFINES "")
        foreach(D IN LISTS VE_SHADER_DEFINES)
            list(APPEND EXTRA_DEFINES "-D${D}")
        endforeach()
        # The same command, one argument per line, for shader hot-reload
        # (engine/src/ve/render/ShaderHotReload.hpp).
        set(CMD_ARGS "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.3 -O -g ${STAGE_DEFINE} ${EXTRA_DEFINES} ${INCLUDE_FLAGS}
                     -MD -MF "${OUT}.d" "${SRC_ABS}" -o "${OUT}")
        list(JOIN CMD_ARGS "\n" CMD_TEXT)
        file(CONFIGURE OUTPUT "${OUT}.cmd" CONTENT "${CMD_TEXT}\n" @ONLY)
        add_custom_command(
            OUTPUT  "${OUT}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${VE_SHADER_OUTPUT_DIR}"
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -g ${STAGE_DEFINE} ${EXTRA_DEFINES} ${INCLUDE_FLAGS}
                    -MD -MF "${OUT}.d" "${SRC_ABS}" -o "${OUT}"
            DEPENDS "${SRC_ABS}"
            DEPFILE "${OUT}.d"
            COMMENT "glslc ${NAME}"
            VERBATIM)
        list(APPEND OUTPUTS "${OUT}")
    endforeach()

    add_custom_target(${TARGET}_shaders DEPENDS ${OUTPUTS})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()
