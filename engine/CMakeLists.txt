# ve_engine - the core engine library.
file(GLOB_RECURSE VE_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB VE_ENGINE_SHADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)

add_library(ve_engine STATIC ${VE_ENGINE_SOURCES})
target_include_directories(ve_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(ve_engine PUBLIC cxx_std_20) # games linking the engine get C++20 too
target_link_libraries(ve_engine PUBLIC ve_thirdparty Vulkan::Vulkan SDL2::SDL2 glm::glm nlohmann_json::nlohmann_json)
target_link_libraries(ve_engine PRIVATE Jolt meshoptimizer Recast Detour DetourCrowd enet ve_bcenc) # physics, mesh simplification, navmesh, networking, BC texture encoding: hidden behind engine APIs
target_compile_definitions(ve_engine PUBLIC
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan clip-space depth is [0, 1]
    GLM_ENABLE_EXPERIMENTAL       # glm/gtx/* (quaternion helpers, matrix_decompose)
    SDL_MAIN_HANDLED)
if(WIN32)
    # windows.h (pulled in by SDL, miniaudio...) must not define min/max
    # macros or the rarely used APIs; the CRT's "use the _s functions" warnings
    # (getenv, fopen, localtime) aren't portable advice.
    target_compile_definitions(ve_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
    # Engine headers are UTF-8 (comments, symbols): games compiling them need it too.
    # /permissive- and /Zc:__cplusplus: standard C++ behaviour, as on GCC / Clang.
    target_compile_options(ve_engine PUBLIC /utf-8 /permissive- /Zc:__cplusplus)
endif()
ve_set_warnings(ve_engine)

# Engine shaders; the include folder is exported for effects and games.
set(VE_ENGINE_SHADER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/shaders/include CACHE INTERNAL "")
ve_compile_shaders(ve_engine SOURCES ${VE_ENGINE_SHADERS} INCLUDE_DIRS ${VE_ENGINE_SHADER_INCLUDE})

# vecook: the asset cooker as a command-line tool (engine/tools/vecook.cpp),
# built next to the game so its defaults find the game's assets and cache.
option(VE_BUILD_TOOLS "Build the engine's command-line tools (vecook)" ON)
if(VE_BUILD_TOOLS)
    add_executable(vecook ${CMAKE_CURRENT_SOURCE_DIR}/tools/vecook.cpp)
    target_link_libraries(vecook PRIVATE ve_engine)
    set_target_properties(vecook PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${VE_OUTPUT_DIR}>")
    ve_set_warnings(vecook)
endif()
