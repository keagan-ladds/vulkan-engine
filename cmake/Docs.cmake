# =============================================================================
# API reference: `cmake --build build --target docs` -> build/docs/html/index.html
#
# Doxygen over the public headers (engine/src, effects/src) plus the docs/*.md
# guides, styled with doxygen-awesome-css. Only added when Doxygen is installed
# (it is optional: nothing else needs it). tools/check_docs.py uses the same
# configuration to list undocumented declarations.
# =============================================================================
option(VE_DOCS "Add a 'docs' target when Doxygen is installed" ${PROJECT_IS_TOP_LEVEL})
if(NOT VE_DOCS)
    return()
endif()
find_package(Doxygen QUIET)
if(NOT DOXYGEN_FOUND)
    message(STATUS "Doxygen not found: no 'docs' target (install doxygen for the API reference)")
    return()
endif()

include(FetchContent)
FetchContent_Declare(doxygen_awesome
    GIT_REPOSITORY https://github.com/jothepro/doxygen-awesome-css.git
    GIT_TAG v2.3.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(doxygen_awesome)

set(VE_DOCS_OUTPUT "${CMAKE_BINARY_DIR}/docs")
set(VE_DOCS_THEME "${doxygen_awesome_SOURCE_DIR}")
configure_file(${PROJECT_SOURCE_DIR}/docs/api/Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)
add_custom_target(docs
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VE_DOCS_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E env VE_DOCS_EXTRACT_ALL=YES VE_DOCS_WARN_UNDOCUMENTED=NO
            $<TARGET_FILE:Doxygen::doxygen> ${CMAKE_BINARY_DIR}/Doxyfile
    COMMAND ${CMAKE_COMMAND} -E echo "API reference: ${VE_DOCS_OUTPUT}/html/index.html"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating the API reference (Doxygen)"
    VERBATIM)
