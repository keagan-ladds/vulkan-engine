#pragma once

// Rebuilding Savanna's world (main.cpp) for a new game, or to load a saved
// game into a freshly built one, behind the loading screen.

#include <string>

namespace ve {
class Engine;
}

namespace savanna {

// Reloads the level as built (a new game), behind the loading screen.
void reloadFresh(ve::Engine& engine);
// Reloads the level (clear + build, behind the loading screen) and applies
// the save in `slot`; false if there is no such save.
bool reloadWithSave(ve::Engine& engine, const std::string& slot);

} // namespace savanna
