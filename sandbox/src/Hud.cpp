#include "Hud.hpp"

#include "Building.hpp"
#include "GameUi.hpp"
#include "Inventory.hpp"
#include "Player.hpp"
#include "Props.hpp"
#include "SceneLookup.hpp"
#include "Vegetation.hpp"
#include "World.hpp"

#include <ve/audio/Audio.hpp>
#include <ve/audio/Music.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Env.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Log.hpp>
#include <ve/gameplay/Interaction.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/scene/SaveGame.hpp>
#include <ve/scene/Scene.hpp>
#include <vfx/DayNight.hpp>
#include <vfx/PhotoMode.hpp>
#include <vfx/Weather.hpp>

#include <imgui.h>

#include <cstdio>
#include <functional>

namespace savanna {

Hud::Hud(Player& player) : GameObject("HUD"), m_player(&player)
{
}

Player* Hud::player() const
{
    return m_player.get(scene());
}

vfx::Weather* Hud::weather()
{
    return findCached(scene(), m_weather);
}

vfx::DayNightCycle* Hud::dayNight()
{
    return findCached(scene(), m_dayNight);
}

bool saveProgress(ve::Scene& scene, const std::string& slot)
{
    const GameState& gs = gameState();
    const bool ok = ve::save::saveGame(scene, slot, {{"collected", gs.collected}, {"inventory", gs.inventory.save()}});
    notify(ok ? "Game saved" : "Saving failed", 2.0f);
    return ok;
}

void applyGameData(const ve::Json& game)
{
    GameState& gs = gameState();
    gs.collected = game.value("collected", 0);
    gs.inventory.load(game.value("inventory", ve::Json::array()));
    notify("Game loaded", 2.0f);
}

bool loadProgress(const std::string& slot)
{
    const bool ok = reloadWithSave(ve::Engine::get(), slot);
    if (!ok) notify("No saved game", 2.0f);
    return ok;
}

void loadControls()
{
    const auto j = ve::save::readConfig("controls");
    if (!j) return;
    try {
        ve::Engine::get().input().loadBindings(*j);
    } catch (const std::exception& e) {
        ve::log::warn("controls: {}", e.what());
    }
}

void saveControls()
{
    ve::save::writeConfig("controls", ve::Engine::get().input().saveBindings());
}

void Hud::drawControls()
{
    ve::Input& in = input();
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Controls (F10)", &m_showControls)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Gamepad: %s", in.gamepadConnected() ? in.gamepadName().c_str() : "none connected");
    if (!in.rebinding().empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Press a key, mouse or gamepad button for '%s' (Esc cancels)",
                                                    in.rebinding().c_str());
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const std::string& action : in.actions()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(action.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(in.bindingText(action).c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(action.c_str());
            if (ImGui::SmallButton("Rebind")) in.startRebind(action);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Keyboard/mouse and gamepad bindings are replaced separately.");
    ImGui::End();
}

void Hud::update(float dt)
{
    if (m_canvas) m_canvas->visible = m_wanted && !vfx::PhotoMode::anyActive(); // a clean view for photos
    // SAVANNA_CONTROLS=1: open the controls window (screenshots).
    static const bool s_controlsShot = ve::env::flag("SAVANNA_CONTROLS");
    if (s_controlsShot && !m_showControls && !m_controlsShotDone) {
        m_showControls = m_controlsShotDone = true;
        input().setMouseCaptured(false);
    }
    // A rebind just finished (F10): keep it for next time.
    const bool rebinding = !input().rebinding().empty();
    if (m_wasRebinding && !rebinding) saveControls();
    m_wasRebinding = rebinding;
    if (input().keyPressed(ve::Key::F10)) {
        m_showControls = !m_showControls;
        input().setMouseCaptured(!m_showControls);
    }
    gameState().total = gameState().collected + Sunstone::remaining(scene()); // stones can be added in the editor
    GameState& gs = gameState();
    gs.messageTimer = std::max(0.0f, gs.messageTimer - dt);
    if (vfx::Weather* w = weather(); w && input().keyPressed(ve::Key::R) && !Builder::building(scene())) w->next(12.0f); // R rotates pieces while building
    if (input().keyPressed(ve::Key::H)) m_showHelp = !m_showHelp;
    if (input().keyPressed(ve::Key::F5)) saveProgress(scene(), "quick");
    if (input().keyPressed(ve::Key::F9)) loadProgress("quick");
    // A stand-in for a fight until there are enemies: game code pushes a mood
    // when combat starts and pops it when it ends; the music returns to
    // whatever was playing (camp or explore).
    if (input().keyPressed(ve::Key::M)) {
        m_combat = !m_combat;
        if (m_combat) audio().music().pushMood("Combat", 10, 1.0f);
        else audio().music().popMood("Combat", 4.0f);
        notify(m_combat ? "Combat music (M to stop)" : "Combat over", 2.0f);
    }
}

void Hud::onGui()
{
    if (m_showControls) drawControls(); // F10 (ImGui: a tools-style window)
}

void Hud::start()
{
    namespace ui = ve::ui;
    m_canvas = &addComponent<ui::Canvas>();
    m_canvas->tooltipFont = gui::font::body;
    ui::Element& root = m_canvas->root();
    const GameState& gs = gameState();

    // Crosshair, and the name of what's in front of us (a physics ray from the eye).
    auto& cross = root.add<ui::Image>("crosshair");
    cross.anchor({0.5f, 0.5f}).pivot({0.5f, 0.5f}).size({16.0f, 16.0f});
    cross.custom = [](ui::Painter& p, const ui::Rect& r) {
        if (!input().mouseCaptured()) return;
        p.circle(r.center(), 3.5f, ui::rgba(0x00000080));
        p.circle(r.center(), 2.4f, ui::rgba(0xffffffe0));
    };
    auto& aim = gui::label(root, "", gui::font::body, 18.0f, ui::rgba(0xffffffa8));
    aim.anchor({0.5f, 0.5f}).pivot({0.5f, 0.0f}).at({0.0f, 16.0f}).size({600.0f, 26.0f});
    aim.alignX = ui::Align::Center;
    aim.bind = [this]() -> std::string {
        if (!input().mouseCaptured()) return {};
        const Player* p = player();
        if (!p) return {};
        auto hit = physics().raycast(p->eyePosition(), p->lookDirection(), 25.0f);
        if (!hit) return {};
        const std::string name = hit->gameObject ? hit->gameObject->name() : std::string(propName(hit->userValue));
        if (name.empty() || name == "Terrain") return {};
        char b[96];
        std::snprintf(b, sizeof(b), "%s  (%.1f m)", name.c_str(), hit->distance);
        return b;
    };

    // The interaction prompt / the last event, in a pill under the crosshair.
    auto& pill = root.add<ui::Panel>("prompt");
    pill.anchor({0.5f, 0.5f}).pivot({0.5f, 0.0f}).at({0.0f, 52.0f}).size({520.0f, 44.0f});
    pill.color = ui::rgba(0x120c08b0);
    pill.borderColor = ui::withAlpha(gui::color::goldDark, 0.7f);
    pill.borderWidth = 1.0f;
    pill.rounding = 22.0f;
    auto& promptText = gui::label(pill, "", gui::font::bold, 22.0f, gui::color::gold);
    promptText.fill();
    promptText.alignX = ui::Align::Center;
    promptText.bind = [this, &pill, &promptText]() -> std::string {
        const GameState& gs = gameState();
        std::string text = gs.messageTimer > 0.0f ? gs.message : std::string();
        const Player* p = player();
        if (const auto* who = p ? p->getComponent<ve::Interactor>() : nullptr; who && text.empty())
            if (const std::string prompt = who->prompt(); !prompt.empty()) text = "[E]  " + prompt;
        pill.visible = !text.empty();
        // Fit the pill to the text.
        const float w = std::max(200.0f, float(text.size()) * 11.5f + 60.0f);
        pill.size({w, 44.0f});
        promptText.color = gs.messageTimer > 0.0f ? gui::color::text : gui::color::gold;
        return text;
    };

    // Vitals, bottom left: health and stamina.
    auto& vitals = root.add<ui::Stack>("vitals");
    vitals.anchor({0.0f, 1.0f}).pivot({0.0f, 1.0f}).at({36.0f, -40.0f}).size({380.0f, 70.0f});
    vitals.spacing = 10.0f;
    auto row = [&](const char* name, glm::vec4 a, glm::vec4 b, std::function<float()> value, float width) {
        auto& line = vitals.add<ui::Element>(name);
        line.size({10.0f, 28.0f});
        auto& tag = gui::label(line, name, gui::font::heading, 20.0f, gui::color::dim);
        tag.anchor({0.0f, 0.5f}).pivot({0.0f, 0.5f}).size({100.0f, 28.0f});
        auto& b2 = gui::bar(line, a, b, std::move(value));
        b2.anchor({0.0f, 0.5f}).pivot({0.0f, 0.5f}).at({104.0f, 0.0f}).size({width, 20.0f});
    };
    row("Health", gui::color::health, gui::color::healthEnd, [this] {
        const Player* p = player();
        return p ? p->health / p->maxHealth : 0.0f;
    }, 270.0f);
    row("Stamina", gui::color::stamina, gui::color::staminaEnd, [this] {
        const Player* p = player();
        return p ? p->stamina / p->maxStamina : 0.0f;
    }, 220.0f);

    // Hotbar, top left: the inventory's first row.
    auto& hotbar = root.add<ui::Grid>("hotbar");
    hotbar.columns = 8;
    hotbar.cell = {64.0f, 64.0f};
    hotbar.spacing = {6.0f, 6.0f};
    hotbar.at({32.0f, 28.0f}).size(hotbar.contentSize(8));
    for (int i = 0; i < 8; ++i) {
        auto& cell = hotbar.add<ui::Panel>("hot" + std::to_string(i));
        cell.color = ui::rgba(0x0d0906a0);
        cell.borderColor = ui::withAlpha(gui::color::goldDark, 0.5f);
        cell.borderWidth = 1.0f;
        cell.rounding = 4.0f;
        auto& icon = cell.add<ui::Image>("icon");
        icon.fill(8.0f);
        icon.custom = [i](ui::Painter& p, const ui::Rect& r) {
            const Inventory::Stack& s = gameState().inventory.slots[size_t(i)];
            const ItemType* t = s.item.empty() ? nullptr : itemType(s.item);
            if (!t) return;
            if (t->icon) p.image(t->icon, r);
            p.text({r.min, r.max + glm::vec2(4.0f, 6.0f)}, std::to_string(s.count), gui::font::bold, 18.0f, gui::color::text, ui::Align::End,
                   ui::Align::End, false, gui::color::shadow);
        };
        auto& key = gui::label(cell, std::to_string(i + 1), gui::font::body, 14.0f, ui::withAlpha(gui::color::dim, 0.8f));
        key.fill({5.0f, 2.0f, 0.0f, 0.0f});
        key.alignY = ui::Align::Start;
    }

    // Clock, weather and sunstones, top right.
    auto& info = root.add<ui::Panel>("info");
    info.anchor({1.0f, 0.0f}).pivot({1.0f, 0.0f}).at({-28.0f, 26.0f}).size({300.0f, 112.0f});
    info.color = ui::rgba(0x120c08a8);
    info.borderColor = ui::withAlpha(gui::color::goldDark, 0.6f);
    info.borderWidth = 1.0f;
    info.rounding = 6.0f;
    auto& clock = gui::label(info, "", gui::font::title, 40.0f, gui::color::text);
    clock.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({18.0f, 8.0f}).size({18.0f, 50.0f});
    clock.bind = [this] {
        const vfx::DayNightCycle* dn = dayNight();
        const float tod = dn ? dn->timeOfDay : 12.0f;
        char b[16];
        std::snprintf(b, sizeof(b), "%02d:%02d", int(tod), int((tod - int(tod)) * 60.0f));
        return std::string(b);
    };
    auto& weather = gui::label(info, "", gui::font::body, 20.0f, gui::color::dim);
    weather.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({18.0f, 16.0f}).size({18.0f, 36.0f});
    weather.alignX = ui::Align::End;
    weather.bind = [this] {
        const vfx::Weather* w = this->weather(); // (not the label above)
        return std::string(w ? vfx::Weather::name(w->weather()) : "") + (player() && player()->flying() ? "  ·  flying" : "");
    };
    auto& stones = gui::label(info, "", gui::font::heading, 22.0f, gui::color::gold);
    stones.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({18.0f, 64.0f}).size({18.0f, 32.0f});
    stones.bind = [] {
        const GameState& g = gameState();
        return "Sunstones  " + std::to_string(g.collected) + " / " + std::to_string(g.total);
    };
    (void)gs;

    // Controls, along the bottom (H hides them).
    auto& help = gui::label(root, "", gui::font::body, 17.0f, ui::rgba(0xf0e6d2b0));
    help.anchors({0.0f, 1.0f}, {1.0f, 1.0f}).pivot({0.0f, 1.0f}).at({0.0f, -10.0f}).size({0.0f, 24.0f});
    help.alignX = ui::Align::Center;
    help.bind = [this]() -> std::string {
        if (!m_showHelp) return {};
        return "WASD move   Shift sprint   Space jump   E use   Q throw   V shoot   Tab inventory   B build   F fly   R weather   F5/F9 save/load   "
               "Esc menu   H hide";
    };
}

void Hud::setVisible(bool visible)
{
    m_wanted = visible;
}

} // namespace savanna
