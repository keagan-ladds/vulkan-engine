#include "MainMenu.hpp"

#include "Building.hpp"
#include "GameUi.hpp"
#include "Inventory.hpp"
#include "Player.hpp"
#include "Props.hpp"
#include "SavannaLayout.hpp"
#include "World.hpp"

#include <ve/audio/Audio.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Log.hpp>
#include <ve/core/Time.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/SaveGame.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Terrain.hpp>

#include <vfx/DayNight.hpp>
#include <vfx/Weather.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>

namespace savanna {

namespace ui = ve::ui;

namespace {
constexpr float kTitleTime = 17.6f; // the title's sunset
constexpr float kNewGameTime = 8.5f;
} // namespace

MainMenu::MainMenu(bool showTitle) : GameObject("Main menu"), m_showTitle(showTitle)
{
    runInEditor = false;
}

void MainMenu::start()
{
    gui::loadFonts();
    loadSettings();
    buildTitle();
    buildPause();
    buildPanels();
    applySettings();
    if (m_showTitle) toTitle();
    else enterPlaying();
}

void MainMenu::onDestroy()
{
    if (ve::GameObject* cam = scene().findById(m_camera)) cam->destroy();
    engine().time().timeScale = 1.0f;
}

// -----------------------------------------------------------------------------
// Screens
// -----------------------------------------------------------------------------

void MainMenu::toTitle()
{
    m_screen = Screen::Title;
    m_leaving = -1.0f;
    m_title->visible = true;
    m_title->fade = 1.0f;
    m_pause->visible = false;
    showPanel("");
    engine().time().timeScale = 1.0f;
    // A camera of its own, above the camp looking west into the sunset.
    if (!scene().findById(m_camera)) {
        auto& cam = scene().spawn("Title camera");
        auto& c = cam.addComponent<ve::Camera>();
        c.priority = 100;
        c.fovDegrees = 48.0f;
        c.farPlane = 6000.0f;
        m_camera = cam.id();
    }
    if (auto* weather = scene().findFirst<vfx::Weather>()) weather->setWeather(vfx::Weather::Type::Clear, 0.5f);
    m_title->focus(nullptr);
    input().setMouseCaptured(false);
    if (ui::Element* cont = m_title->root().find("Continue")) cont->enabled = ve::save::read("quick").has_value();
}

void MainMenu::enterPlaying()
{
    m_newGameReload = true; // once played, a new game starts from a rebuilt world
    m_screen = Screen::Playing;
    m_title->visible = false;
    m_pause->visible = false;
    showPanel("");
    engine().time().timeScale = 1.0f;
    if (ve::GameObject* cam = scene().findById(m_camera)) cam->destroy();
    m_camera = 0;
    input().setMouseCaptured(true);
}

void MainMenu::newGame()
{
    s_played = true;
    if (m_newGameReload) { // the world was played: start again from the level as built
        reloadFresh(engine());
        return;
    }
    if (auto* dn = scene().findFirst<vfx::DayNightCycle>()) dn->timeOfDay = kNewGameTime;
    m_leaving = 0.6f; // fade the title out, then play
}

void MainMenu::continueGame()
{
    s_played = true;
    if (!loadProgress("quick")) newGame(); // (reloads the level with the save)
}

void MainMenu::pause()
{
    m_screen = Screen::Paused;
    m_pause->visible = true;
    m_pause->focus(nullptr);
    engine().time().timeScale = 0.0f;
    input().setMouseCaptured(false);
}

void MainMenu::resume()
{
    enterPlaying();
}

void MainMenu::showPanel(const std::string& name)
{
    m_panel = name;
    if (!m_panels) return;
    m_panels->visible = !name.empty();
    // The menu under a panel steps aside (the windows are see-through).
    if (m_title) m_title->visible = m_screen == Screen::Title && name.empty();
    if (m_pause) m_pause->visible = m_screen == Screen::Paused && name.empty();
    for (const auto& c : m_panels->root().children())
        if (c->name.rfind("panel:", 0) == 0) c->visible = c->name == "panel:" + name;
    if (name == "load" || name == "controls") { // their contents change: rebuild
        if (ui::Element* p = m_panels->root().find("panel:" + name))
            if (ui::Element* content = p->find("content")) {
                content->clear();
                if (name == "load") buildLoad(*content);
                else buildControls(*content);
            }
    }
    m_panels->focus(nullptr);
}

void MainMenu::update(float dt)
{
    m_t += dt;
    if (Hud* hud = scene().findFirst<Hud>()) hud->setVisible(m_screen == Screen::Playing);

    if (m_screen == Screen::Title) {
        // Hold the light at dusk and drift the camera slowly along the camp's edge.
        if (auto* dn = scene().findFirst<vfx::DayNightCycle>(); dn && m_leaving < 0.0f) dn->timeOfDay = kTitleTime;
        if (ve::GameObject* cam = scene().findById(m_camera)) {
            const float s = std::sin(m_t * 0.05f), c = std::cos(m_t * 0.037f);
            const glm::vec2 base = layout::kCamp + glm::vec2(34.0f + 4.0f * s, 4.0f + 6.0f * c);
            const ve::Terrain* t = ve::Terrain::find(scene());
            const float ground = t ? t->height(base.x, base.y) : 3.0f;
            cam->transform().setPosition({base.x, ground + 9.0f + 0.8f * c, base.y});
            const float yaw = glm::radians(96.0f + 5.0f * s), pitch = glm::radians(-5.0f);
            cam->transform().setRotation(glm::angleAxis(yaw, glm::vec3(0, 1, 0)) * glm::angleAxis(pitch, glm::vec3(1, 0, 0)));
        }
        input().setMouseCaptured(false);
        if (m_leaving >= 0.0f) {
            m_leaving -= time().unscaledDeltaTime;
            m_title->fade = std::max(0.0f, m_leaving / 0.6f);
            if (m_leaving <= 0.0f) enterPlaying();
        }
        return;
    }
    if (m_screen == Screen::Playing) {
        // Escape (unless building: it stops building) / pad Start: pause.
        const bool escape = input().keyPressed(ve::Key::Escape) || input().gamepadPressed(ve::GamepadButton::Start);
        const Builder* b = scene().findFirst<Builder>();
        if (escape && !(b && (b->active() || b->justStopped()))) pause();
    }
}

// -----------------------------------------------------------------------------
// Building the screens
// -----------------------------------------------------------------------------

void MainMenu::buildTitle()
{
    m_title = &addComponent<ui::Canvas>();
    m_title->modal = true;
    m_title->order = 30;
    m_title->visible = false;
    m_title->tooltipFont = gui::font::body;
    ui::Element& root = m_title->root();

    // Darken the left side for the text, and the bottom a little (a vignette).
    auto& shade = root.add<ui::Image>("shade");
    shade.fill();
    shade.custom = [](ui::Painter& p, const ui::Rect& r) {
        const float w = r.size().x * 0.62f, h = r.size().y * 0.3f;
        p.gradientH({r.min, {r.min.x + w, r.max.y}}, ui::rgba(0x0a0604e8), ui::rgba(0x0a060400));
        p.gradient({{r.min.x, r.max.y - h}, r.max}, ui::rgba(0x00000000), ui::rgba(0x000000a0));
    };

    auto& title = gui::label(root, "SAVANNA", gui::font::title, 124.0f, gui::color::gold);
    title.at({132.0f, 150.0f}).size({1000.0f, 150.0f});
    title.shadow = ui::rgba(0x000000d0);
    auto& sub = gui::label(root, "A   V U L K A N - E N G I N E   S A N D B O X", gui::font::heading, 24.0f, gui::color::dim);
    sub.at({140.0f, 300.0f}).size({1000.0f, 36.0f});
    sub.shadow = ui::rgba(0x000000c0);
    auto& rule = root.add<ui::Panel>("rule");
    rule.at({140.0f, 350.0f}).size({430.0f, 1.5f});
    rule.color = ui::withAlpha(gui::color::goldDark, 0.9f);

    auto& list = root.add<ui::Stack>("menu");
    list.at({122.0f, 392.0f}).size({460.0f, 460.0f});
    list.spacing = 4.0f;
    gui::menuButton(list, "Continue", [this] { continueGame(); });
    gui::menuButton(list, "New game", [this] { newGame(); });
    gui::menuButton(list, "Load game", [this] { showPanel("load"); });
    gui::menuButton(list, "Settings", [this] { showPanel("settings"); });
    gui::menuButton(list, "Controls", [this] { showPanel("controls"); });
    gui::menuButton(list, "Quit", [this] { engine().quit(); });

    auto& foot = gui::label(root, "Made with vulkan-engine", gui::font::body, 20.0f, ui::withAlpha(gui::color::dim, 0.8f));
    foot.anchor({0.0f, 1.0f}).pivot({0.0f, 1.0f}).at({140.0f, -36.0f}).size({600.0f, 26.0f});
    auto& keys = gui::label(root, "Arrows / D-pad  select     Enter / A  confirm     Esc / B  back", gui::font::body, 20.0f,
                            ui::withAlpha(gui::color::dim, 0.8f));
    keys.anchor({1.0f, 1.0f}).pivot({1.0f, 1.0f}).at({-48.0f, -36.0f}).size({760.0f, 26.0f});
    keys.alignX = ui::Align::End;
}

void MainMenu::buildPause()
{
    m_pause = &addComponent<ui::Canvas>();
    m_pause->modal = true;
    m_pause->order = 30;
    m_pause->visible = false;
    m_pause->onBack = [this] { resume(); };
    m_pause->shortcuts = {{ve::Key::Unknown, ve::GamepadButton::Start, [this] { resume(); }}};
    ui::Element& root = m_pause->root();
    auto& dim = root.add<ui::Panel>("dim");
    dim.fill();
    dim.color = ui::rgba(0x050302a8);
    dim.blocksPointer = true;
    auto& title = gui::label(root, "PAUSED", gui::font::title, 64.0f, gui::color::gold);
    title.anchor({0.5f, 0.0f}).pivot({0.5f, 0.0f}).at({0.0f, 190.0f}).size({600.0f, 80.0f});
    title.alignX = ui::Align::Center;
    auto& list = root.add<ui::Stack>("menu");
    list.anchor({0.5f, 0.0f}).pivot({0.5f, 0.0f}).at({0.0f, 300.0f}).size({360.0f, 520.0f});
    list.spacing = 2.0f;
    auto centred = [](ui::Button& b) -> ui::Button& {
        b.label().alignX = ui::Align::Center;
        b.label().position = glm::vec2(0.0f);
        return b;
    };
    centred(gui::menuButton(list, "Resume", [this] { resume(); }, 32.0f));
    centred(gui::menuButton(list, "Save game", [this] {
        saveProgress(scene(), "quick");
        resume();
    }, 32.0f));
    centred(gui::menuButton(list, "Load game", [this] { showPanel("load"); }, 32.0f));
    centred(gui::menuButton(list, "Settings", [this] { showPanel("settings"); }, 32.0f));
    centred(gui::menuButton(list, "Controls", [this] { showPanel("controls"); }, 32.0f));
    centred(gui::menuButton(list, "Main menu", [this] { toTitle(); }, 32.0f));
    centred(gui::menuButton(list, "Quit game", [this] { engine().quit(); }, 32.0f));
}

void MainMenu::buildPanels()
{
    m_panels = &addComponent<ui::Canvas>();
    m_panels->modal = true;
    m_panels->order = 40;
    m_panels->visible = false;
    m_panels->tooltipFont = gui::font::body;
    m_panels->onBack = [this] { showPanel(""); };
    ui::Element& root = m_panels->root();
    auto& dim = root.add<ui::Panel>("dim");
    dim.fill();
    dim.color = ui::rgba(0x00000060);
    dim.blocksPointer = true;
    auto panel = [&](const char* name, const char* title, glm::vec2 size) -> ui::Element& {
        auto& holder = root.add<ui::Element>(std::string("panel:") + name);
        holder.fill();
        holder.visible = false;
        ui::Element& content = gui::window(holder, title, size);
        ve::ui::Element& frame = *content.parent(); // the window
        auto& back = gui::boxButton(frame, "Back", [this] { showPanel(""); }, 20.0f);
        back.anchor({1.0f, 0.0f}).pivot({1.0f, 0.0f}).at({-18.0f, 16.0f}).size({110.0f, 40.0f});
        return content;
    };
    panel("load", "Load game", {760.0f, 620.0f});
    ui::Element& settings = panel("settings", "Settings", {760.0f, 580.0f});
    buildSettings(settings);
    panel("controls", "Controls", {900.0f, 760.0f});
}

void MainMenu::buildLoad(ui::Element& content)
{
    auto& list = content.add<ui::Stack>("slots");
    list.fill();
    list.spacing = 10.0f;
    const auto slots = ve::save::slots();
    if (slots.empty()) {
        auto& none = gui::label(list, "No saved games yet: F5, or Save game in the pause menu.", gui::font::body, 22.0f, gui::color::dim);
        none.size({10.0f, 40.0f});
        return;
    }
    for (const ve::save::SlotInfo& s : slots) {
        std::string when;
        try {
            const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(s.time);
            when = std::format("{:%Y-%m-%d  %H:%M}", std::chrono::floor<std::chrono::minutes>(sys));
        } catch (...) {
        }
        const std::string slot = s.name;
        auto& b = gui::boxButton(list, slot + "      " + when, [this, slot] {
            s_played = true;
            loadProgress(slot);
        }, 22.0f);
        b.size({10.0f, 58.0f});
        b.label().alignX = ui::Align::Start;
        b.label().position = {20.0f, 0.0f};
    }
}

void MainMenu::buildSettings(ui::Element& content)
{
    auto& list = content.add<ui::Stack>("rows");
    list.fill();
    list.spacing = 14.0f;
    auto slider = [&](const char* name, float* value) {
        auto& row = list.add<ui::Element>(name);
        row.size({10.0f, 44.0f});
        auto& l = gui::label(row, name, gui::font::body, 22.0f, gui::color::text);
        l.anchor({0.0f, 0.5f}).pivot({0.0f, 0.5f}).size({280.0f, 40.0f});
        auto& s = row.add<ui::Slider>(std::string("slider:") + name);
        s.anchors({0.0f, 0.5f}, {1.0f, 0.5f}).pivot({0.0f, 0.5f}).at({290.0f, 0.0f}).size({20.0f, 30.0f}); // across, 30 high
        s.value = *value;
        s.fill = gui::color::gold;
        s.onChanged = [this, value](float v) {
            *value = v;
            applySettings();
            saveSettings();
        };
    };
    slider("Master volume", &m_settings.master);
    slider("Music", &m_settings.music);
    slider("Effects", &m_settings.effects);
    slider("Ambience", &m_settings.ambience);
    slider("Mouse sensitivity", &m_settings.mouse);
    slider("Field of view", &m_settings.fov);
    auto toggle = [&](const char* name, bool* value) {
        auto& row = list.add<ui::Element>(name);
        row.size({10.0f, 48.0f});
        auto& l = gui::label(row, name, gui::font::body, 22.0f, gui::color::text);
        l.anchor({0.0f, 0.5f}).pivot({0.0f, 0.5f}).size({280.0f, 40.0f});
        auto& b = gui::boxButton(row, *value ? "On" : "Off", {}, 20.0f);
        b.name = std::string("toggle:") + name;
        b.anchor({0.0f, 0.5f}).pivot({0.0f, 0.5f}).at({290.0f, 0.0f}).size({120.0f, 42.0f});
        b.onClick = [this, value, &b] {
            *value = !*value;
            b.label().text = *value ? "On" : "Off";
            applySettings();
            saveSettings();
        };
    };
    toggle("Temporal anti-aliasing", &m_settings.taa);
    toggle("Controls help", &m_settings.help);
}

void MainMenu::buildControls(ui::Element& content)
{
    // Every action the game binds, with its keys / buttons; Rebind takes the next one pressed.
    auto& grid = content.add<ui::Stack>("actions");
    grid.fill();
    grid.spacing = 6.0f;
    ve::Input& in = input();
    for (const std::string& action : in.actions()) {
        auto& row = grid.add<ui::Panel>(action);
        row.size({10.0f, 44.0f});
        row.color = ui::rgba(0x0e0a0770);
        row.rounding = 4.0f;
        auto& name = gui::label(row, action, gui::font::bold, 20.0f, gui::color::text);
        name.anchor({0.0f, 0.5f}).pivot({0.0f, 0.5f}).at({14.0f, 0.0f}).size({230.0f, 40.0f});
        auto& keys = gui::label(row, "", gui::font::body, 20.0f, gui::color::dim);
        keys.anchors({0.0f, 0.5f}, {1.0f, 0.5f}).pivot({0.0f, 0.5f}).at({250.0f, 0.0f}).size({150.0f, 40.0f});
        keys.bind = [this, action] {
            ve::Input& in = input();
            if (in.rebinding() == action) return std::string("Press a key or button...  (Esc cancels)");
            std::string s;
            for (const std::string& n : in.bindingNames(action)) s += (s.empty() ? "" : ",  ") + n;
            return s;
        };
        auto& rebind = gui::boxButton(row, "Rebind", [this, action] { input().startRebind(action); }, 18.0f);
        rebind.anchor({1.0f, 0.5f}).pivot({1.0f, 0.5f}).at({-8.0f, 0.0f}).size({120.0f, 36.0f});
    }
}

// -----------------------------------------------------------------------------
// Settings
// -----------------------------------------------------------------------------

void MainMenu::applySettings()
{
    ve::AudioSystem& a = audio();
    a.setMasterVolume(m_settings.master);
    a.setBusVolume("Music", m_settings.music);
    a.setBusVolume("SFX", m_settings.effects);
    a.setBusVolume("UI", m_settings.effects);
    a.setBusVolume("Ambient", m_settings.ambience);
    a.setBusVolume("Weather", m_settings.ambience);
    if (!std::getenv("SAVANNA_TAA")) renderer().settings().temporalAA = m_settings.taa; // (tests choose)
    if (auto* player = dynamic_cast<Player*>(scene().find("Player"))) {
        player->mouseSensitivity = glm::mix(0.0008f, 0.0043f, m_settings.mouse);
        if (ve::Camera* cam = player->camera()) cam->fovDegrees = glm::mix(55.0f, 95.0f, m_settings.fov);
    }
    if (Hud* hud = scene().findFirst<Hud>()) hud->setShowHelp(m_settings.help);
}

void MainMenu::saveSettings() const
{
    const ve::Json j{{"master", m_settings.master}, {"music", m_settings.music}, {"effects", m_settings.effects}, {"ambience", m_settings.ambience},
                     {"mouse", m_settings.mouse}, {"fov", m_settings.fov}, {"taa", m_settings.taa}, {"help", m_settings.help}};
    std::error_code ec;
    std::filesystem::create_directories(ve::save::folder(), ec);
    std::ofstream(ve::save::folder() / "settings.json") << j.dump(2) << '\n';
}

void MainMenu::loadSettings()
{
    std::ifstream in(ve::save::folder() / "settings.json");
    if (!in) return;
    try {
        ve::Json j;
        in >> j;
        m_settings.master = j.value("master", m_settings.master);
        m_settings.music = j.value("music", m_settings.music);
        m_settings.effects = j.value("effects", m_settings.effects);
        m_settings.ambience = j.value("ambience", m_settings.ambience);
        m_settings.mouse = j.value("mouse", m_settings.mouse);
        m_settings.fov = j.value("fov", m_settings.fov);
        m_settings.taa = j.value("taa", m_settings.taa);
        m_settings.help = j.value("help", m_settings.help);
    } catch (const std::exception& e) {
        ve::log::warn("Settings: {}", e.what());
    }
}

} // namespace savanna
