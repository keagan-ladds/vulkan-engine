#pragma once

// The game's menus, on the engine's UI elements (ve/ui/Gui.hpp) in the
// game's style (GameUi.hpp):
//   - Title: the game starts on a sunset over the savanna (the real world, a
//     slowly drifting camera, the clock held at dusk) with the menu over it,
//     much like Valheim: Continue, New game, Load game, Settings, Controls, Quit.
//   - Pause (Escape / pad Start while playing): Resume, Save, Load, Settings,
//     Controls, Main menu, Quit. The world stops.
//   - Panels over either: Load (save slots), Settings (volumes, mouse,
//     field of view, TAA, help), Controls (every action, rebind).
// Tests and the editor start straight in the game (no title).

#include <ve/scene/GameObject.hpp>
#include <ve/ui/Gui.hpp>

#include <glm/glm.hpp>

#include <string>

namespace savanna {

class MainMenu : public ve::GameObject {
public:
    enum class Screen { Title, Playing, Paused };
    explicit MainMenu(bool showTitle);
    void start() override;
    void update(float dt) override;
    void onDestroy() override;

    Screen screen() const { return m_screen; }
    void newGame();
    void continueGame();
    void pause();
    void resume();
    void toTitle();
    void showPanel(const std::string& name); // "load", "save", "settings", "controls", "multiplayer"; "" = close
    const std::string& panel() const { return m_panel; }
    ve::ui::Canvas& title() { return *m_title; }
    ve::ui::Canvas& pauseMenu() { return *m_pause; }
    ve::ui::Canvas& panels() { return *m_panels; }

    // The first title of this run has been left (a reloaded level goes straight into the game).
    static inline bool s_played = false;

private:
    void buildTitle();
    void buildPause();
    void buildPanels();
    void buildLoad(ve::ui::Element& content);
    void buildSettings(ve::ui::Element& content);
    void buildControls(ve::ui::Element& content);
    void buildSave(ve::ui::Element& content);
    void buildMultiplayer(ve::ui::Element& content);
    void saveAs(const std::string& name);
    void enterPlaying();
    void applySettings();
    void saveSettings() const;
    void loadSettings();

    Screen m_screen = Screen::Playing;
    bool m_showTitle;
    ve::ui::Canvas* m_title = nullptr;
    ve::ui::Canvas* m_pause = nullptr;
    ve::ui::Canvas* m_panels = nullptr;
    std::string m_panel;
    uint64_t m_camera = 0; // the title's camera
    uint64_t m_hudId = 0, m_builderId = 0, m_dayNightId = 0; // (looked up every frame: SceneLookup.hpp)
    float m_t = 0.0f;
    float m_leaving = -1.0f; // fading the title out (seconds left)
    bool m_newGameReload = false;
    bool m_joining = false; // waiting for the host to let us in
    std::string m_lastSave;

    struct Settings {
        float master = 0.9f, music = 0.7f, effects = 0.9f, ambience = 0.8f;
        float mouse = 0.4f; // 0..1 -> sensitivity
        float fov = 0.25f;  // 0..1 -> 55..95 degrees (65)
        bool taa = true, help = true;
        std::string playerName = "Player", address = "127.0.0.1";
        int port = 7777;
    } m_settings;
};

} // namespace savanna
