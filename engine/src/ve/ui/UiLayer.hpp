#pragma once

// =============================================================================
// UiLayer - Dear ImGui integration.
//
// Any GameObject/Component can draw UI in its onGui() hook with plain ImGui
// calls (#include <imgui.h>). The UI is rendered on top of the final image by
// UiPass. F1 toggles the engine's debug overlay (stats, renderer settings,
// every render pass with an enable toggle and its own settings).
// =============================================================================

#include "ve/render/RenderGraph.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

union SDL_Event;

namespace ve {

class Engine;

/// ImGui integration: input, frames, fonts, and the F1 debug overlay (Engine::ui()).
class UiLayer {
public:
    /// Engine: sets up ImGui for the window and renderer.
    void init(Engine& engine);
    /// Engine: tears ImGui down.
    void shutdown();

    /// Engine: feeds an SDL event to ImGui.
    void processEvent(const SDL_Event& event);
    /// Engine: starts the ImGui frame (before gameplay's onGui).
    void beginFrame();
    /// Draws the F1 overlay (stats, settings, profiler) when debugOverlay is on.
    void drawDebugOverlay();
    /// Ends the ImGui frame if the renderer skipped drawing (e.g. minimized).
    void endFrameIfNotRendered();

    bool debugOverlay = false;  ///< Show the F1 overlay.
    /// Adds a collapsible section to the F1 overlay (after the built-in ones):
    /// `draw` makes ImGui calls. Modules the ui module can't see (network...)
    /// add theirs this way.
    void addOverlaySection(const std::string& title, std::function<void()> draw);
    /// ImGui wants the keyboard (a text field is active): gameplay shouldn't read keys.
    bool wantsKeyboard() const;
    /// ImGui wants the mouse (pointer over a window): gameplay shouldn't read the mouse.
    bool wantsMouse() const;

private:
    friend class UiPass;
    Engine* m_engine = nullptr;
    bool m_initialized = false;
    bool m_rendered = false;
    std::vector<std::pair<std::string, std::function<void()>>> m_sections; ///< addOverlaySection
    float m_frameTimes[120]{};
    int m_frameTimeIndex = 0;
};

/// The render pass that draws ImGui (and the game UI) over the frame.
class UiPass : public RenderPass {
public:
    /// Draws `layer`'s ImGui frame; also runs during loading screens.
    explicit UiPass(UiLayer& layer) : RenderPass("UI", RenderStage::Overlay), m_layer(layer) { runsWhileLoading = true; }
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;

private:
    UiLayer& m_layer;
};

} // namespace ve
