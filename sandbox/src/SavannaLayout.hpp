#pragma once
// Fixed places in the Savanna level that the (still code-defined) vegetation
// rules refer to. The terrain itself (shape, biomes, stamps) lives in
// scenes/savanna.json; these move to data with the foliage layers (Phase W4).
#include <glm/glm.hpp>

namespace savanna::layout {
constexpr float kHalfSize = 320.0f; // the world spans [-320, 320]
constexpr glm::vec2 kCamp{20.0f, 10.0f};
constexpr glm::vec2 kSwamp{-130.0f, 110.0f};
constexpr glm::vec2 kKopje{120.0f, -80.0f};
constexpr glm::vec2 kSmallKopje{-50.0f, -140.0f};
} // namespace savanna::layout
