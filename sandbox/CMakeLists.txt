# savanna - the engine's test scene / demo game. Built like any game would be
# (see cmake/Game.cmake and templates/game).
file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB SANDBOX_SHADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag)

add_executable(savanna ${SANDBOX_SOURCES})
target_include_directories(savanna PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
ve_setup_game(savanna
    ASSETS  ${CMAKE_CURRENT_SOURCE_DIR}/assets
    SHADERS ${SANDBOX_SHADERS})
