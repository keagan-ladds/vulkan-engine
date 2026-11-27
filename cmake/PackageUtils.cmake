# Helpers shared by cmake/Package.cmake (build-tree package) and
# cmake/Install.cmake (installed package).
include_guard(GLOBAL)

# Every non-imported target that `roots` link to, directly or through static
# libraries' LINK_ONLY dependencies: the set export() needs to be complete.
function(_ve_link_closure out)
    set(seen "")
    set(queue ${ARGN})
    while(queue)
        list(POP_FRONT queue t)
        # $<LINK_ONLY:x>, $<BUILD_INTERFACE:x> and nestings of the two name a target too.
        while(t MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE):(.+)>$")
            set(t "${CMAKE_MATCH_2}")
        endwhile()
        if(t MATCHES "[$<>]" OR NOT TARGET "${t}")
            continue() # plain library names (ws2_32, m) and generator expressions
        endif()
        get_target_property(aliased "${t}" ALIASED_TARGET)
        if(aliased)
            set(t "${aliased}")
        endif()
        get_target_property(imported "${t}" IMPORTED)
        if(imported OR t IN_LIST seen)
            continue() # Vulkan::Vulkan, Threads::Threads: found again by the game
        endif()
        list(APPEND seen "${t}")
        foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(libs "${t}" ${prop})
            if(libs)
                list(APPEND queue ${libs})
            endif()
        endforeach()
    endwhile()
    set(${out} "${seen}" PARENT_SCOPE)
endfunction()

