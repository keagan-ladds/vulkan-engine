#pragma once

// Savanna's loading screen: a small scene of its own (one object drawing UI)
// that the engine shows while the world loads (ve::Engine::beginLoading).

#include <ve/scene/Scene.hpp>

#include <memory>

namespace savanna {

std::unique_ptr<ve::Scene> makeLoadingScene();

} // namespace savanna
