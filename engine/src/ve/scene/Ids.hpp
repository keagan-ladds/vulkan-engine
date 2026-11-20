#pragma once

// =============================================================================
// Ids - GameObject ids and their text form in scene / prefab / save files.
//
//   uint64_t id = ve::newObjectId();        // random, never 0
//   std::string s = ve::idToHex(id);        // "00ab12cd34ef5678" (16 lowercase hex digits)
//   uint64_t back = ve::idFromHex(s);       // 0 if it isn't a valid id
//
// One implementation for every file format: scene files, prefabs and save
// games must keep writing exactly this text, or old files stop matching.
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>

namespace ve {

uint64_t newObjectId();
std::string idToHex(uint64_t id);
uint64_t idFromHex(std::string_view text);

} // namespace ve
