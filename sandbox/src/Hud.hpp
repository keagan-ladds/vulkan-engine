#pragma once

// The in-game HUD (crosshair, prompt, vitals, hotbar, clock; F10 controls
// window), and the save / control-binding functions it and the menus use.

#include <ve/core/Event.hpp>
#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>
#include <ve/scene/SaveGame.hpp>
#include <ve/ui/Gui.hpp>

#include <cstdint>
#include <string>

namespace vfx {
class Weather;
class DayNightCycle;
} // namespace vfx

namespace savanna {

class Player;

// Save games (F5 / F9): the engine captures the level's changes and every
// ve::Saveable (player, regrowth, foliage, terrain, time of day); the
// GameState counters (Props.hpp) go in the save's "game" section.
bool saveProgress(ve::Scene& scene, const std::string& slot);
// Loads by rebuilding the level behind the loading screen (World.hpp), then applying the save.
bool loadProgress(const std::string& slot);
void applyGameData(const ve::Json& game); // the counters, from a save's "game" section
// Control bindings (F10 rebinds them): loaded over the defaults, kept with the settings (ve::save::writeConfig).
void loadControls();
void saveControls();

class Hud : public ve::GameObject {
public:
    explicit Hud(Player& player);
    void start() override;
    void update(float dt) override;
    void onGui() override;
    void setVisible(bool visible); // (the main menu hides it)
    void setShowHelp(bool show) { m_showHelp = show; }

private:
    // Looked up on every use (O(1)): the editor can delete any of them, and
    // the UI binds below may run in a frame where update() didn't.
    Player* player() const;             // null once the player is gone
    vfx::Weather* weather();            // the scene's Weather / DayNightCycle (scene objects), or null
    vfx::DayNightCycle* dayNight();
    ve::ObjectRef<Player> m_player;
    ve::ObjectRef<vfx::DayNightCycle> m_dayNight;
    ve::ObjectRef<vfx::Weather> m_weather;
    void drawControls(); // F10: actions, their bindings, rebinding
    void drawCompiling(); // "Compiling shaders" corner note (Renderer::compileEvents)
    ve::Subscription m_compileSub;
    float m_compileShownUntil = 0.0f; // (real time: kept up briefly so quick compiles don't flicker)
    std::string m_compileName;
    bool m_showHelp = true;
    bool m_showControls = false;
    bool m_controlsShotDone = false;
    bool m_wasRebinding = false;
    bool m_combat = false; // M: stand-in for a fight (pushes the "Combat" music mood)
    ve::ui::Canvas* m_canvas = nullptr; // the HUD (ve::ui elements, see start())
    bool m_wanted = true;
};

} // namespace savanna
