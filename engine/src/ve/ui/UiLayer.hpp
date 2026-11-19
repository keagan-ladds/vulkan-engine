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

union SDL_Event;

namespace ve {

class Engine;

class UiLayer {
public:
    void init(Engine& engine);
    void shutdown();

    void processEvent(const SDL_Event& event);
    void beginFrame();
    void drawDebugOverlay();
    // Ends the ImGui frame if the renderer skipped drawing (e.g. minimized).
    void endFrameIfNotRendered();

    bool debugOverlay = false;
    bool wantsKeyboard() const;
    bool wantsMouse() const;

private:
    friend class UiPass;
    Engine* m_engine = nullptr;
    bool m_initialized = false;
    bool m_rendered = false;
    float m_frameTimes[120]{};
    int m_frameTimeIndex = 0;
};

class UiPass : public RenderPass {
public:
    explicit UiPass(UiLayer& layer) : RenderPass("UI", RenderStage::Overlay), m_layer(layer) { runsWhileLoading = true; }
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;

private:
    UiLayer& m_layer;
};

} // namespace ve
