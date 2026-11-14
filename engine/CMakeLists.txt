# ve_engine - the core engine library.
file(GLOB_RECURSE VE_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB VE_ENGINE_SHADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)

add_library(ve_engine STATIC ${VE_ENGINE_SOURCES})
target_include_directories(ve_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(ve_engine PUBLIC cxx_std_20) # games linking the engine get C++20 too
target_link_libraries(ve_engine PUBLIC ve_thirdparty Vulkan::Vulkan SDL2::SDL2 glm::glm)
target_link_libraries(ve_engine PRIVATE Jolt) # physics backend, hidden behind ve/physics
target_compile_definitions(ve_engine PUBLIC
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan clip-space depth is [0, 1]
    GLM_ENABLE_EXPERIMENTAL       # glm/gtx/* (quaternion helpers, matrix_decompose)
    SDL_MAIN_HANDLED)
ve_set_warnings(ve_engine)

# Engine shaders; the include folder is exported for effects and games.
set(VE_ENGINE_SHADER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/shaders/include CACHE INTERNAL "")
ve_compile_shaders(ve_engine SOURCES ${VE_ENGINE_SHADERS} INCLUDE_DIRS ${VE_ENGINE_SHADER_INCLUDE})
