#pragma once

// Building Savanna's world (main.cpp), and loading a saved game into a freshly
// built one behind the loading screen.

#include <string>

namespace ve {
class Engine;
}

namespace savanna {

void buildWorld(ve::Engine& engine);
// Reloads the level (clear + build, behind the loading screen) and applies
// the save in `slot`; false if there is no such save.
// Reloads the level as built (a new game).
void reloadFresh(ve::Engine& engine);
bool reloadWithSave(ve::Engine& engine, const std::string& slot);

} // namespace savanna
