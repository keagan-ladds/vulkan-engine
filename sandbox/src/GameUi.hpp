#pragma once

// The game's look on top of the engine's UI elements (ve/ui/Gui.hpp): fonts,
// colours and a few builders, so every screen (main menu, pause, HUD,
// inventory, build menu) shares one style. Warm gold on dark, Roman-capital
// headings (Cinzel), plain body text (Lato).

#include <ve/ui/Gui.hpp>

#include <functional>
#include <string>

namespace savanna::gui {

namespace font {
inline const char* title = "title";     // Cinzel SemiBold
inline const char* heading = "heading"; // Cinzel Regular
inline const char* body = "body";       // Lato Regular
inline const char* bold = "bold";       // Lato Bold
} // namespace font

namespace color {
inline const glm::vec4 text = ve::ui::rgba(0xf3e7cfff);
inline const glm::vec4 dim = ve::ui::rgba(0xb9a585ff);
inline const glm::vec4 gold = ve::ui::rgba(0xe8b45cff);
inline const glm::vec4 goldDark = ve::ui::rgba(0x8a6630ff);
inline const glm::vec4 panel = ve::ui::rgba(0x17110ce6);
inline const glm::vec4 panelLight = ve::ui::rgba(0x2a2018f0);
inline const glm::vec4 slot = ve::ui::rgba(0x0e0a07d0);
inline const glm::vec4 shadow = ve::ui::rgba(0x000000c0);
inline const glm::vec4 health = ve::ui::rgba(0xc8322cff);
inline const glm::vec4 healthEnd = ve::ui::rgba(0xf06a3cff);
inline const glm::vec4 stamina = ve::ui::rgba(0xd8a032ff);
inline const glm::vec4 staminaEnd = ve::ui::rgba(0xf5d36aff);
} // namespace color

void loadFonts(); // once, at startup

// A framed window with a title (inventory, build menu, settings). Returns the content area.
ve::ui::Element& window(ve::ui::Element& parent, const std::string& title, glm::vec2 size);
// Main-menu style: gold text, glows on hover / focus.
ve::ui::Button& menuButton(ve::ui::Element& parent, const std::string& text, std::function<void()> onClick, float fontSize = 34.0f);
// A boxed button (dialogs, tabs).
ve::ui::Button& boxButton(ve::ui::Element& parent, const std::string& text, std::function<void()> onClick, float fontSize = 22.0f);
// A labelled bar (health, stamina).
ve::ui::ProgressBar& bar(ve::ui::Element& parent, glm::vec4 from, glm::vec4 to, std::function<float()> value);
ve::ui::Label& label(ve::ui::Element& parent, const std::string& text, const char* font, float size, glm::vec4 color = color::text);

} // namespace savanna::gui
