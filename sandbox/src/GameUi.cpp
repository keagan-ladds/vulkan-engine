#include "GameUi.hpp"

namespace savanna::gui {

using namespace ve::ui;

void loadFonts()
{
    static bool done = false;
    if (done) return;
    done = true;
    // Baked at the sizes the screens use (at 1080p and a bit beyond).
    loadFont(font::title, "fonts/Cinzel_600SemiBold.ttf", {40, 64, 128});
    loadFont(font::heading, "fonts/Cinzel_400Regular.ttf", {24, 34, 48});
    loadFont(font::body, "fonts/Lato_400Regular.ttf", {16, 20, 26, 34});
    loadFont(font::bold, "fonts/Lato_700Bold.ttf", {18, 24, 32});
}

Label& label(Element& parent, const std::string& text, const char* f, float size, glm::vec4 c)
{
    auto& l = parent.add<Label>(text);
    l.font = f;
    l.fontSize = size;
    l.color = c;
    l.shadow = color::shadow;
    l.size({10.0f, size * 1.35f});
    return l;
}

Element& window(Element& parent, const std::string& title, glm::vec2 size)
{
    auto& w = parent.add<Panel>("window");
    w.anchor({0.5f, 0.5f}).pivot({0.5f, 0.5f}).size(size);
    w.color = color::panel;
    w.gradientBottom = ve::ui::rgba(0x0d0907f0);
    w.borderColor = color::goldDark;
    w.borderWidth = 1.5f;
    w.rounding = 6.0f;
    w.blocksPointer = true;
    auto& t = label(w, title, font::heading, 34.0f, color::gold);
    t.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({0.0f, 14.0f}).size({0.0f, 46.0f});
    t.alignX = Align::Center;
    // A thin rule under the title.
    auto& rule = w.add<Panel>("rule");
    rule.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({40.0f, 64.0f}).size({40.0f, 1.5f});
    rule.color = withAlpha(color::goldDark, 0.8f);
    auto& content = w.add<Element>("content");
    content.fill({24.0f, 80.0f, 24.0f, 24.0f});
    return content;
}

Button& menuButton(Element& parent, const std::string& text, std::function<void()> onClick, float fontSize)
{
    auto& b = parent.add<Button>(text);
    b.size({10.0f, fontSize * 1.5f});
    b.normal = glm::vec4(0.0f);
    b.hover = ve::ui::rgba(0xe8b45c1c);
    b.down = ve::ui::rgba(0xe8b45c38);
    b.disabled = glm::vec4(0.0f);
    b.textNormal = color::text;
    b.textHover = color::gold;
    b.textDisabled = withAlpha(color::dim, 0.5f);
    b.focusBorder = glm::vec4(0.0f); // the gold text shows focus
    b.rounding = 4.0f;
    b.label().font = font::heading;
    b.label().fontSize = fontSize;
    b.label().alignX = Align::Start;
    b.label().position = {18.0f, 0.0f};
    b.label().shadow = color::shadow;
    b.onClick = std::move(onClick);
    return b;
}

Button& boxButton(Element& parent, const std::string& text, std::function<void()> onClick, float fontSize)
{
    auto& b = parent.add<Button>(text);
    b.size({160.0f, fontSize * 1.9f});
    b.normal = color::panelLight;
    b.hover = ve::ui::rgba(0x4a3622f0);
    b.down = ve::ui::rgba(0x6a4a28ff);
    b.borderColor = color::goldDark;
    b.borderWidth = 1.0f;
    b.rounding = 4.0f;
    b.textNormal = color::text;
    b.textHover = color::gold;
    b.focusBorder = color::gold;
    b.label().font = font::bold;
    b.label().fontSize = fontSize;
    b.onClick = std::move(onClick);
    return b;
}

ProgressBar& bar(Element& parent, glm::vec4 from, glm::vec4 to, std::function<float()> value)
{
    auto& p = parent.add<ProgressBar>();
    p.fill = from;
    p.fillEnd = to;
    p.background = ve::ui::rgba(0x0b0806c8);
    p.borderColor = withAlpha(color::goldDark, 0.9f);
    p.borderWidth = 1.0f;
    p.rounding = 3.0f;
    p.inset = 2.5f;
    p.bind = std::move(value);
    return p;
}

} // namespace savanna::gui
