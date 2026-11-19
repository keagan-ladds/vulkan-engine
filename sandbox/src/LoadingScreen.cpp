#include "LoadingScreen.hpp"

#include <ve/core/Engine.hpp>
#include <ve/scene/GameObject.hpp>

#include <imgui.h>

#include <cmath>
#include <cstdlib>
#include <string>

namespace savanna {

namespace {

// The backbuffer is sRGB: ImGui colours are linear values, so convert the
// intended display colours (0..255 sRGB) first.
ImU32 srgb(int r, int g, int b, int a = 255)
{
    auto lin = [](int c) { return int(std::pow(float(c) / 255.0f, 2.2f) * 255.0f + 0.5f); };
    return IM_COL32(lin(r), lin(g), lin(b), a);
}

// Title, a progress bar with what's being loaded, and a rotating tip.
class LoadingScreen : public ve::GameObject {
public:
    LoadingScreen() : GameObject("LoadingScreen") {}
    void update(float dt) override
    {
        m_t += dt;
        // SAVANNA_LOADING_SHOT=<png>: capture the loading screen once it's half way (tests / docs).
        if (const char* shot = std::getenv("SAVANNA_LOADING_SHOT"); shot && !m_shot && engine().loadProgress() >= 0.5f) {
            renderer().requestCapture(shot);
            m_shot = true;
        }
    }
    void onGui() override
    {
        static const char* kTips[] = {
            "Chop acacias with the left mouse button; they grow back as stumps.",
            "Berry bushes regrow after a while.",
            "Sunstones glow at dusk. Collect all twelve.",
            "F2 opens the editor: sculpt the terrain from the inspector.",
            "F12 is photo mode.",
        };
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 size = io.DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(srgb(28, 20, 14)));
        ImGui::Begin("##loading", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // A setting sun over a horizon line (drawn, no assets needed while loading).
        const ImVec2 c(size.x * 0.5f, size.y * 0.42f);
        const float r = std::min(size.x, size.y) * 0.09f;
        dl->AddCircleFilled(c, r, srgb(236, 136, 48), 64);
        dl->AddRectFilled(ImVec2(0, c.y + r * 0.35f), size, srgb(14, 10, 7));
        dl->AddLine(ImVec2(size.x * 0.2f, c.y + r * 0.35f), ImVec2(size.x * 0.8f, c.y + r * 0.35f), srgb(236, 136, 48, 160), 2.0f);

        const ve::Engine& e = engine();
        const float progress = e.loadProgress();
        auto centred = [&](const std::string& text, float y, ImU32 color, float scale) {
            ImGui::SetWindowFontScale(scale);
            const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2((size.x - ts.x) * 0.5f, y), color, text.c_str());
            ImGui::SetWindowFontScale(1.0f);
        };
        centred("SAVANNA", c.y + r * 0.6f, srgb(240, 220, 190), 2.6f);

        // Progress bar.
        const float bw = std::min(520.0f, size.x * 0.6f), bh = 8.0f, by = size.y * 0.72f, bx = (size.x - bw) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), srgb(60, 45, 32), 4.0f);
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * progress, by + bh), srgb(236, 160, 70), 4.0f);
        const int dots = int(m_t * 2.0f) % 4;
        centred(e.loadStatus() + std::string(size_t(dots), '.') + "   " + std::to_string(int(progress * 100.0f + 0.5f)) + " %", by + 18.0f,
                srgb(200, 180, 150), 1.0f);
        centred(kTips[int(m_t / 5.0f) % int(std::size(kTips))], size.y * 0.86f, srgb(150, 130, 110), 1.0f);

        ImGui::End();
        ImGui::PopStyleColor();
    }

private:
    float m_t = 0.0f;
    bool m_shot = false;
};

} // namespace

std::unique_ptr<ve::Scene> makeLoadingScene()
{
    auto scene = std::make_unique<ve::Scene>();
    scene->spawn<LoadingScreen>();
    return scene;
}

} // namespace savanna
