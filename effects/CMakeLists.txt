# ve_effects - optional visual add-ons (sky/clouds, fog, bloom, particles, weather).
# Built only on the public engine API; a game can skip linking it entirely.
file(GLOB_RECURSE VE_EFFECTS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB VE_EFFECTS_SHADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp)

add_library(ve_effects STATIC ${VE_EFFECTS_SOURCES})
target_include_directories(ve_effects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ve_effects PUBLIC ve_engine)
ve_set_warnings(ve_effects)

set(VE_EFFECTS_SHADER_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/shaders/include CACHE INTERNAL "")
if(VE_EFFECTS_SHADERS)
    ve_compile_shaders(ve_effects SOURCES ${VE_EFFECTS_SHADERS}
        INCLUDE_DIRS ${VE_ENGINE_SHADER_INCLUDE} ${VE_EFFECTS_SHADER_INCLUDE})
endif()

# Noise generation runs at startup; keep it fast even in Debug builds.
if(NOT MSVC)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/vfx/Noise.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()
