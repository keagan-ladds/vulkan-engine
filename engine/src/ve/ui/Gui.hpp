#pragma once

// =============================================================================
// Gui - the engine's game UI layer: retained-mode elements laid out on a
// canvas, drawn over the world (ImGui stays for tools: editor, F1 overlay).
//
// The engine provides the mechanism (layout, drawing, fonts, input routing,
// focus navigation for keyboard / gamepad, drag & drop, tooltips); the game
// owns the look (colours, fonts, sizes) and what the screens do.
//
//   auto& canvas = scene.spawn("Main menu").addComponent<ve::ui::Canvas>();
//   canvas.modal = true;                                  // blocks gameplay input while visible
//   auto& panel = canvas.root().add<ve::ui::Panel>();
//   panel.anchor({0.0f, 0.5f}).at({120.0f, 0.0f}).size({420.0f, 520.0f}).pivot({0.0f, 0.5f});
//   panel.color = ve::ui::rgba(0x1a1410e0);
//   auto& list = panel.add<ve::ui::Stack>();              // vertical list
//   list.fill(24.0f);
//   auto& start = list.add<ve::ui::Button>("Start");
//   start.onClick = [&] { startGame(); };
//
// Units: a canvas is laid out at a reference resolution (1920 x 1080 by
// default) and scaled to the window by height, so sizes are the same on
// every screen. Colours are written in sRGB (as in an image editor).
//
// Layout: every element has anchors in its parent (0..1; the same point for
// a fixed-size element, two points to stretch), a pivot, a position and a
// size (for stretched elements, position / size are insets: see fill()).
// Stack and Grid arrange their children themselves.
//
// Input (routed by the engine before gameplay runs each frame):
//   - pointer: hover, press, click (onClick), drag & drop (draggable /
//     onDrop), tooltips; a canvas over the pointer takes the mouse from gameplay;
//   - focus: keyboard arrows, gamepad d-pad or left stick move between
//     focusable elements (buttons) of the top modal canvas, Enter / Space /
//     pad A click, Escape / pad B call the canvas' onBack;
//   - while a modal canvas is visible, gameplay sees no keys, buttons or pad.
// =============================================================================

#include "ve/core/Input.hpp"
#include "ve/scene/Components.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace ve {

class Engine;
class Texture;

namespace ui {

// 0xRRGGBBAA in sRGB.
glm::vec4 rgba(uint32_t hex);
inline glm::vec4 withAlpha(glm::vec4 c, float a) { return {c.r, c.g, c.b, a}; }

struct Rect {
    glm::vec2 min{0.0f}, max{0.0f};
    glm::vec2 size() const { return max - min; }
    glm::vec2 center() const { return (min + max) * 0.5f; }
    bool contains(glm::vec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
};

enum class Align { Start, Center, End };

// ---- Fonts ----------------------------------------------------------------------
// Loads a TTF (asset path) under a name, baked at a few sizes; drawing picks
// the closest baked size. Fonts load before the next frame.
void loadFont(const std::string& name, const std::string& path, std::vector<float> sizes = {16, 22, 30, 44, 72});

// ---- Drawing ---------------------------------------------------------------------
// What elements draw with: canvas units in, pixels out (scale + opacity applied).
class Painter {
public:
    void rect(const Rect& r, glm::vec4 color, float rounding = 0.0f);
    void gradient(const Rect& r, glm::vec4 top, glm::vec4 bottom, float rounding = 0.0f);
    void border(const Rect& r, glm::vec4 color, float thickness = 1.0f, float rounding = 0.0f);
    void line(glm::vec2 a, glm::vec2 b, glm::vec4 color, float thickness = 1.0f);
    void gradientH(const Rect& r, glm::vec4 left, glm::vec4 right); // left -> right (square corners)
    void image(const std::shared_ptr<Texture>& texture, const Rect& r, glm::vec4 tint = glm::vec4(1.0f), glm::vec2 uv0 = glm::vec2(0.0f),
               glm::vec2 uv1 = glm::vec2(1.0f), float rounding = 0.0f);
    // Text inside `r`, aligned; returns its size (canvas units). wrap: break lines at r's width.
    glm::vec2 text(const Rect& r, const std::string& text, const std::string& font, float size, glm::vec4 color, Align h = Align::Start,
                   Align v = Align::Center, bool wrap = false, glm::vec4 shadow = glm::vec4(0.0f));
    glm::vec2 measure(const std::string& text, const std::string& font, float size, float wrapWidth = 0.0f) const;
    // Any shape: polygon in canvas units.
    void polygon(const std::vector<glm::vec2>& points, glm::vec4 color);
    void circle(glm::vec2 center, float radius, glm::vec4 color, bool filled = true, float thickness = 1.0f);

    float scale = 1.0f;     // canvas units -> pixels
    float opacity = 1.0f;   // multiplied into every colour
    ImDrawList* list = nullptr;
    glm::vec2 toPixels(glm::vec2 p) const { return p * scale; }
    uint32_t pack(glm::vec4 srgb) const;
};

class Canvas;

// ---- Elements ----------------------------------------------------------------------
class Element {
public:
    explicit Element(std::string name = {}) : name(std::move(name)) {}
    virtual ~Element();
    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    std::string name;
    bool visible = true;
    bool enabled = true;     // disabled: drawn dimmed, no clicks / focus
    float opacity = 1.0f;
    std::string tooltip;     // shown after hovering a moment
    bool blocksPointer = false; // takes the mouse from gameplay even without onClick (panels)

    // Layout (canvas units).
    glm::vec2 anchorMin{0.0f}, anchorMax{0.0f};
    glm::vec2 pivotPoint{0.0f};
    glm::vec2 position{0.0f};
    glm::vec2 extent{100.0f, 40.0f};
    // Fluent setters.
    Element& anchor(glm::vec2 a) { anchorMin = anchorMax = a; return *this; }
    Element& anchors(glm::vec2 lo, glm::vec2 hi) { anchorMin = lo; anchorMax = hi; return *this; }
    Element& pivot(glm::vec2 p) { pivotPoint = p; return *this; }
    Element& at(glm::vec2 p) { position = p; return *this; }
    Element& size(glm::vec2 s) { extent = s; return *this; }
    // Stretch over the parent, inset by margins (left, top, right, bottom).
    Element& fill(glm::vec4 margins = glm::vec4(0.0f));
    Element& fill(float margin) { return fill(glm::vec4(margin)); }

    // Events.
    std::function<void()> onClick;
    std::function<void(bool hovered)> onHover;
    bool draggable = false;                         // can be picked up (inventory items)
    std::function<bool(Element& dragged)> onDrop;   // something dropped here; return true if taken
    std::function<void(Painter&, glm::vec2 at)> dragVisual; // drawn under the pointer while dragged

    // Children.
    template <class T, class... Args>
    T& add(Args&&... args)
    {
        auto e = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *e;
        e->m_parent = this;
        m_children.push_back(std::move(e));
        return ref;
    }
    void remove(Element& child);
    void clear() { m_children.clear(); }
    const std::vector<std::unique_ptr<Element>>& children() const { return m_children; }
    Element* parent() const { return m_parent; }
    Element* find(const std::string& name);              // depth first, this included
    template <class T>
    T* findAs(const std::string& n) { return dynamic_cast<T*>(find(n)); }

    // State (as of the last frame).
    const Rect& rect() const { return m_rect; } // canvas units
    bool hovered() const { return m_hovered; }
    bool pressed() const { return m_pressed; }
    bool focused() const { return m_focused; }
    bool shown() const;                          // this and every parent visible
    void click();                                // as if clicked (tests, scripts)

    virtual bool focusable() const { return false; }
    virtual bool interactive() const { return onClick || draggable || onDrop || !tooltip.empty(); }
    // While pressed: the pointer, in canvas units (sliders follow it).
    virtual void onPointer(glm::vec2) {}
    // Focused and a direction pressed: return true to keep it (a slider
    // steps) instead of moving the focus.
    virtual bool onNavigate(glm::vec2) { return false; }

    // For subclasses / custom elements.
    void arrange(const Rect& parent);                // rect from the anchors, then layout()
    void place(const Rect& r);                       // rect given (by a Stack / Grid), then layout()
    virtual void layout();                           // the children (default: each by its anchors)
    virtual void draw(Painter& p);                   // self, then children
    virtual void drawSelf(Painter&) {}

protected:
    Rect m_rect{};

private:
    friend class Canvas;
    friend class System;
    Element* m_parent = nullptr;
    std::vector<std::unique_ptr<Element>> m_children;
    bool m_hovered = false, m_pressed = false, m_focused = false;
};

// A filled box: colour or vertical gradient, border, rounding, optional image.
class Panel : public Element {
public:
    using Element::Element;
    glm::vec4 color{0.0f};
    glm::vec4 gradientBottom{-1.0f}; // alpha >= 0: vertical gradient from color to this
    glm::vec4 borderColor{0.0f};
    float borderWidth = 0.0f;
    float rounding = 0.0f;
    std::shared_ptr<Texture> image;
    glm::vec4 imageTint{1.0f};
    void drawSelf(Painter& p) override;
};

class Label : public Element {
public:
    explicit Label(std::string text = {}, std::string name = {}) : Element(std::move(name)), text(std::move(text)) {}
    std::string text;
    std::string font = "default";
    float fontSize = 22.0f;
    glm::vec4 color{1.0f};
    glm::vec4 shadow{0.0f, 0.0f, 0.0f, 0.0f}; // alpha > 0: a drop shadow
    Align alignX = Align::Start, alignY = Align::Center;
    bool wrap = false;
    std::function<std::string()> bind; // if set, the text comes from here each frame
    void drawSelf(Painter& p) override;
};

// A texture, or anything the game draws itself (icons, minimaps).
class Image : public Element {
public:
    using Element::Element;
    std::shared_ptr<Texture> texture;
    glm::vec4 tint{1.0f};
    bool keepAspect = true;
    std::function<void(Painter&, const Rect&)> custom;
    void drawSelf(Painter& p) override;
};

// A clickable panel with a label; looks per state. Focusable.
class Button : public Panel {
public:
    explicit Button(std::string text = {}, std::string name = {});
    Label& label() { return *m_label; }
    glm::vec4 normal{0.0f}, hover{0.0f}, down{0.0f}, disabled{0.0f};
    glm::vec4 textNormal{1.0f}, textHover{1.0f}, textDisabled{0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 focusBorder{1.0f, 0.8f, 0.4f, 1.0f};
    bool selected = false; // stays in the hover look (tabs, chosen items)
    bool focusable() const override { return enabled && visible; }
    bool interactive() const override { return true; }
    void drawSelf(Painter& p) override;

private:
    Label* m_label = nullptr;
};

// A bar (health, stamina, loading): value 0..1, with a trail that follows
// drops a moment later (shows how much was just lost).
class ProgressBar : public Element {
public:
    using Element::Element;
    float value = 1.0f;
    std::function<float()> bind; // if set, the value comes from here each frame
    glm::vec4 fill{0.8f, 0.2f, 0.2f, 1.0f};
    glm::vec4 fillEnd{-1.0f}; // alpha >= 0: horizontal gradient to this
    glm::vec4 trail{1.0f, 0.9f, 0.6f, 0.8f};
    glm::vec4 background{0.0f, 0.0f, 0.0f, 0.6f};
    glm::vec4 borderColor{0.0f};
    float borderWidth = 0.0f;
    float rounding = 0.0f;
    float inset = 2.0f;
    void drawSelf(Painter& p) override;

private:
    float m_trail = 1.0f;
};

// A value 0..1 set by dragging, clicking, or left / right when focused.
class Slider : public Element {
public:
    using Element::Element;
    float value = 0.5f;
    float step = 0.05f;                       // keyboard / gamepad step
    std::function<void(float)> onChanged;
    glm::vec4 track{0.0f, 0.0f, 0.0f, 0.55f}, fill{0.9f, 0.7f, 0.35f, 1.0f}, knob{1.0f, 0.95f, 0.85f, 1.0f};
    glm::vec4 focusColor{1.0f, 0.8f, 0.4f, 1.0f};
    bool focusable() const override { return enabled && visible; }
    bool interactive() const override { return true; }
    void onPointer(glm::vec2 p) override;
    bool onNavigate(glm::vec2 dir) override;
    void drawSelf(Painter& p) override;
    void set(float v);
};

// Lays its visible children out in a row or column: each keeps its size along
// the axis, and fills the cross axis if crossFill (else uses `cross` alignment).
class Stack : public Element {
public:
    using Element::Element;
    bool vertical = true;
    float spacing = 8.0f;
    glm::vec4 padding{0.0f}; // left, top, right, bottom
    bool crossFill = true;
    Align cross = Align::Start;
    Align main = Align::Start; // where the children sit along the axis if there's room
    void layout() override;
};

// Fixed-size cells, left to right, top to bottom.
class Grid : public Element {
public:
    using Element::Element;
    int columns = 4;
    glm::vec2 cell{64.0f, 64.0f};
    glm::vec2 spacing{6.0f};
    glm::vec4 padding{0.0f};
    void layout() override;
    glm::vec2 contentSize(int count) const; // the size `count` cells take
};

// ---- Canvas ----------------------------------------------------------------------------
class Canvas : public Component {
public:
    Canvas();
    ~Canvas() override;
    Element& root() { return *m_root; }

    glm::vec2 referenceSize{1920.0f, 1080.0f}; // scaled to the window by height
    int order = 0;           // higher draws on top and gets input first
    bool modal = false;      // while visible: focus navigation here, gameplay input blocked
    bool hideInEditor = true;
    bool visible = true;
    float fade = 1.0f;       // overall opacity (fade in / out)
    std::function<void()> onBack; // Escape / pad B
    // Keys / pad buttons this canvas reacts to while it's the top modal one
    // (gameplay can't read them then): e.g. the inventory closing on Tab.
    struct Shortcut {
        Key key = Key::Unknown;
        GamepadButton pad = GamepadButton::Count;
        std::function<void()> action;
    };
    std::vector<Shortcut> shortcuts;
    std::string tooltipFont = "default";
    glm::vec4 tooltipBackground = rgba(0x101010e6), tooltipText = rgba(0xf0e6d2ff);

    void focus(Element* e);   // keyboard / gamepad focus (null = none)
    Element* focused() const { return m_focus; }
    float scale() const { return m_scale; }
    Element* hit(glm::vec2 point); // topmost element under a canvas point that takes the pointer

    void start() override;
    void onDestroy() override;

private:
    friend class System;
    std::unique_ptr<Element> m_root;
    Element* m_focus = nullptr;
    float m_scale = 1.0f;
    bool m_registered = false;
};

// The engine side: owns the canvases' input routing and drawing (UiLayer calls it).
class System {
public:
    static System& get();
    // Before gameplay: pointer, focus and drag & drop; says what to take from gameplay.
    void route(Engine& engine);
    void draw(Engine& engine);   // after gameplay's onGui
    bool wantsMouse() const { return m_wantsMouse; }
    bool blocksGameplay() const { return m_block; }

    // Engine internal.
    void add(Canvas* c);
    void remove(Canvas* c);
    void applyPendingFonts();   // before ImGui::NewFrame
    ImFont* font(const std::string& name, float pixelSize) const;
    void* textureId(const std::shared_ptr<Texture>& texture); // kept alive while cached
    void forget(const Element* e); // an element is going away
    void releaseTextures();     // at shutdown
    void queueFont(const std::string& name, const std::string& path, std::vector<float> sizes);

private:
    bool shownNow(const Canvas& c, Engine& engine) const;
    std::vector<Canvas*> m_canvases;
    struct FontEntry {
        std::string name;
        float size;
        ImFont* font;
    };
    std::vector<FontEntry> m_fonts;
    struct PendingFont {
        std::string name, path;
        std::vector<float> sizes;
    };
    std::vector<PendingFont> m_pending;
    std::vector<std::pair<std::shared_ptr<Texture>, void*>> m_textures;
    bool m_wantsMouse = false, m_block = false;
    // Pointer state.
    Element* m_down = nullptr;
    Canvas* m_downCanvas = nullptr;
    Element* m_dragging = nullptr;
    glm::vec2 m_downAt{0.0f};
    Element* m_hover = nullptr;
    float m_hoverTime = 0.0f;
    float m_navRepeat = 0.0f;
    glm::vec2 m_pointer{0.0f};
};

} // namespace ui
} // namespace ve
