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

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawList;
struct ImFont;

namespace ve {
class Input;

class Engine;
class Texture;

namespace ui {

/// 0xRRGGBBAA in sRGB.
glm::vec4 rgba(uint32_t hex);
/// `c` with its alpha replaced by `a`.
inline glm::vec4 withAlpha(glm::vec4 c, float a) { return {c.r, c.g, c.b, a}; }

/// An axis-aligned rectangle in canvas units (y down).
struct Rect {
    glm::vec2 min{0.0f}; ///< Top-left corner.
    glm::vec2 max{0.0f}; ///< Bottom-right corner.
    /// Width and height.
    glm::vec2 size() const { return max - min; }
    /// The centre point.
    glm::vec2 center() const { return (min + max) * 0.5f; }
    /// Whether `p` is inside (min inclusive, max exclusive).
    bool contains(glm::vec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
};

/// Alignment along one axis: left / top, centre, right / bottom.
enum class Align { Start, Center, End };

// ---- Fonts ----------------------------------------------------------------------
/// Loads a TTF (asset path) under a name, baked at a few sizes; drawing picks
/// the closest baked size. Fonts load before the next frame.
void loadFont(const std::string& name, const std::string& path, std::vector<float> sizes = {16, 22, 30, 44, 72});

// ---- Drawing ---------------------------------------------------------------------
/// What elements draw with: canvas units in, pixels out (scale + opacity applied).
class Painter {
public:
    /// Fills `r` with a colour (sRGB). @param rounding corner radius in canvas units.
    void rect(const Rect& r, glm::vec4 color, float rounding = 0.0f);
    /// Fills `r` with a vertical gradient from `top` to `bottom` (sRGB).
    void gradient(const Rect& r, glm::vec4 top, glm::vec4 bottom, float rounding = 0.0f);
    /// Outlines `r`. @param thickness line width in canvas units.
    void border(const Rect& r, glm::vec4 color, float thickness = 1.0f, float rounding = 0.0f);
    /// A line from `a` to `b`.
    void line(glm::vec2 a, glm::vec2 b, glm::vec4 color, float thickness = 1.0f);
    void gradientH(const Rect& r, glm::vec4 left, glm::vec4 right); ///< left -> right (square corners)
    /// Draws a texture into `r`.
    /// @param tint multiplies the texture (sRGB, alpha = opacity).
    /// @param uv0, uv1 the part of the texture to show (0..1).
    void image(const std::shared_ptr<Texture>& texture, const Rect& r, glm::vec4 tint = glm::vec4(1.0f), glm::vec2 uv0 = glm::vec2(0.0f),
               glm::vec2 uv1 = glm::vec2(1.0f), float rounding = 0.0f);
    /// Text inside `r`, aligned; returns its size (canvas units). wrap: break lines at r's width.
    glm::vec2 text(const Rect& r, const std::string& text, const std::string& font, float size, glm::vec4 color, Align h = Align::Start,
                   Align v = Align::Center, bool wrap = false, glm::vec4 shadow = glm::vec4(0.0f));
    /// The size `text` would take (canvas units), wrapped at `wrapWidth` if > 0.
    glm::vec2 measure(const std::string& text, const std::string& font, float size, float wrapWidth = 0.0f) const;
    /// Any shape: polygon in canvas units.
    void polygon(const std::vector<glm::vec2>& points, glm::vec4 color);
    /// A circle (filled, or an outline `thickness` wide).
    void circle(glm::vec2 center, float radius, glm::vec4 color, bool filled = true, float thickness = 1.0f);

    /// Draw only inside r (nested clips intersect).
    void pushClip(const Rect& r);
    /// Ends the innermost pushClip.
    void popClip();

    float scale = 1.0f;     ///< canvas units -> pixels
    float opacity = 1.0f;   ///< multiplied into every colour
    ImDrawList* list = nullptr;  ///< The ImGui draw list it draws into (set by the canvas).
    /// Canvas units to pixels.
    glm::vec2 toPixels(glm::vec2 p) const { return p * scale; }
    /// Packs an sRGB colour (with `opacity` applied) for the draw list.
    uint32_t pack(glm::vec4 srgb) const;
};

class Canvas;

// ---- Elements ----------------------------------------------------------------------
/// A UI element: a rectangle with children, laid out by anchors (see the top
/// of this file), drawn, and taking pointer / focus input. Subclass it for
/// custom widgets (override drawSelf / layout).
class Element {
public:
    /// An element called `name` (find() looks elements up by it).
    explicit Element(std::string name = {}) : name(std::move(name)) {}
    virtual ~Element();
    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    std::string name;  ///< Identifies it for find() and tests.
    bool visible = true;  ///< Hidden elements (and their children) aren't drawn or interactive.
    bool enabled = true;     ///< disabled: drawn dimmed, no clicks / focus
    float opacity = 1.0f;  ///< Multiplies the element's and its children's alpha (0..1).
    std::string tooltip;     ///< shown after hovering a moment
    bool blocksPointer = false; ///< takes the mouse from gameplay even without onClick (panels)

    // Layout (canvas units).
    glm::vec2 anchorMin{0.0f}; ///< Anchor in the parent (0..1 per axis); equal to anchorMax for a fixed-size element.
    glm::vec2 anchorMax{0.0f}; ///< Second anchor: differs from anchorMin for an element that stretches.
    glm::vec2 pivotPoint{0.0f};  ///< The point of the element placed at `position`: (0,0) top left, (0.5,0.5) centre.
    glm::vec2 position{0.0f};  ///< Offset from the anchor point (canvas units); for stretched elements, see fill().
    glm::vec2 extent{100.0f, 40.0f};  ///< Size in canvas units (stretched elements: insets, see fill()).
    /// Fluent setters.
    /// Anchors to one point of the parent (0..1 each axis): a fixed-size element.
    Element& anchor(glm::vec2 a) { anchorMin = anchorMax = a; return *this; }
    /// Anchors to two points of the parent: the element stretches between them.
    Element& anchors(glm::vec2 lo, glm::vec2 hi) { anchorMin = lo; anchorMax = hi; return *this; }
    /// Sets the pivot (see pivotPoint).
    Element& pivot(glm::vec2 p) { pivotPoint = p; return *this; }
    /// Sets the position (see position).
    Element& at(glm::vec2 p) { position = p; return *this; }
    /// Sets the size (see extent).
    Element& size(glm::vec2 s) { extent = s; return *this; }
    /// Stretch over the parent, inset by margins (left, top, right, bottom).
    Element& fill(glm::vec4 margins = glm::vec4(0.0f));
    /// fill() with the same margin on every side.
    Element& fill(float margin) { return fill(glm::vec4(margin)); }

    /// Events.
    std::function<void()> onClick;
    std::function<void(bool hovered)> onHover;  ///< Called when the pointer enters (true) or leaves (false).
    bool draggable = false;                         ///< can be picked up (inventory items)
    std::function<bool(Element& dragged)> onDrop;   ///< something dropped here; return true if taken
    std::function<void(Painter&, glm::vec2 at)> dragVisual; ///< drawn under the pointer while dragged

    /// Children.
    template <class T, class... Args>
    T& add(Args&&... args)
    {
        auto e = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *e;
        e->m_parent = this;
        m_children.push_back(std::move(e));
        return ref;
    }
    /// Removes (and destroys) a child.
    void remove(Element& child);
    /// Removes (and destroys) every child.
    void clear() { m_children.clear(); }
    /// The children, in drawing order (later ones on top).
    const std::vector<std::unique_ptr<Element>>& children() const { return m_children; }
    /// The parent element (null for a canvas root).
    Element* parent() const { return m_parent; }
    Element* find(const std::string& name);              ///< depth first, this included
    template <class T>
    /// find() cast to T (null if not found or not a T).
    T* findAs(const std::string& n) { return dynamic_cast<T*>(find(n)); }

    /// State (as of the last frame).
    const Rect& rect() const { return m_rect; } ///< canvas units
    /// Whether the pointer is over it (last frame).
    bool hovered() const { return m_hovered; }
    /// Whether it is held down (pointer or focus + confirm).
    bool pressed() const { return m_pressed; }
    /// Whether it has keyboard / gamepad focus.
    bool focused() const { return m_focused; }
    bool shown() const;                          ///< this and every parent visible
    void click();                                ///< as if clicked (tests, scripts)

    /// Whether keyboard / gamepad focus can land on it (buttons, sliders, text fields).
    virtual bool focusable() const { return false; }
    virtual bool clipsChildren() const { return false; } ///< children outside the rect are hidden (scroll views)
    virtual bool wantsText() const { return false; }     ///< takes typed text while focused (text fields)
    /// Whether it reacts to the pointer (gets hover / press / click).
    virtual bool interactive() const { return onClick || draggable || onDrop || !tooltip.empty(); }
    /// While pressed: the pointer, in canvas units (sliders follow it).
    virtual void onPointer(glm::vec2) {}
    /// Focused and a direction pressed: return true to keep it (a slider
    /// steps) instead of moving the focus.
    virtual bool onNavigate(glm::vec2) { return false; }

    /// For subclasses / custom elements.
    void arrange(const Rect& parent);                ///< rect from the anchors, then layout()
    void place(const Rect& r);                       ///< rect given (by a Stack / Grid), then layout()
    virtual void layout();                           ///< the children (default: each by its anchors)
    virtual void draw(Painter& p);                   ///< self, then children
    /// Draws the element itself (not its children); override in custom elements.
    virtual void drawSelf(Painter&) {}

protected:
    Rect m_rect{}; ///< Laid-out rectangle (canvas units), set by arrange() / place().

private:
    friend class Canvas;
    friend class System;
    Element* m_parent = nullptr;
    std::vector<std::unique_ptr<Element>> m_children;
    bool m_hovered = false, m_pressed = false, m_focused = false;
};

/// A filled box: colour or vertical gradient, border, rounding, optional image.
class Panel : public Element {
public:
    using Element::Element;
    glm::vec4 color{0.0f};           ///< Fill colour (sRGB; alpha 0 = no fill).
    glm::vec4 gradientBottom{-1.0f}; ///< alpha >= 0: vertical gradient from color to this
    glm::vec4 borderColor{0.0f};     ///< Border colour (sRGB).
    float borderWidth = 0.0f;        ///< Border thickness (canvas units; 0 = none).
    float rounding = 0.0f;           ///< Corner radius (canvas units).
    std::shared_ptr<Texture> image;  ///< Drawn over the fill, stretched to the rect (null = none).
    glm::vec4 imageTint{1.0f};       ///< Multiplies the image (sRGB, alpha = opacity).
    void drawSelf(Painter& p) override;
};

/// Text: one line or wrapped, aligned in its rect, optionally bound to a function.
class Label : public Element {
public:
    /// A label showing `text`.
    explicit Label(std::string text = {}, std::string name = {}) : Element(std::move(name)), text(std::move(text)) {}
    std::string text;                 ///< What it shows (UTF-8).
    std::string font = "default";     ///< A font name given to loadFont ("default" = ImGui's).
    float fontSize = 22.0f;           ///< Canvas units.
    glm::vec4 color{1.0f};            ///< Text colour (sRGB).
    glm::vec4 shadow{0.0f, 0.0f, 0.0f, 0.0f}; ///< alpha > 0: a drop shadow
    Align alignX = Align::Start;      ///< Horizontal alignment in the rect.
    Align alignY = Align::Center;     ///< Vertical alignment in the rect.
    bool wrap = false;                ///< Break lines at the rect's width.
    std::function<std::string()> bind; ///< if set, the text comes from here each frame
    void drawSelf(Painter& p) override;
};

/// A texture, or anything the game draws itself (icons, minimaps).
class Image : public Element {
public:
    using Element::Element;
    std::shared_ptr<Texture> texture;                  ///< What to show (null: only `custom` draws).
    glm::vec4 tint{1.0f};                              ///< Multiplies the texture (sRGB, alpha = opacity).
    bool keepAspect = true;                            ///< Fit inside the rect without stretching.
    std::function<void(Painter&, const Rect&)> custom; ///< Draws extra things over it (the rect in canvas units).
    void drawSelf(Painter& p) override;
};

/// A clickable panel with a label; looks per state. Focusable.
class Button : public Panel {
public:
    /// A button labelled `text`.
    explicit Button(std::string text = {}, std::string name = {});
    /// Its label (font, size, alignment...).
    Label& label() { return *m_label; }
    glm::vec4 normal{0.0f};        ///< Fill colour at rest (sRGB).
    glm::vec4 hover{0.0f};         ///< Fill while hovered, focused or selected.
    glm::vec4 down{0.0f};          ///< Fill while pressed.
    glm::vec4 disabled{0.0f};      ///< Fill while disabled.
    glm::vec4 textNormal{1.0f};    ///< Label colour at rest.
    glm::vec4 textHover{1.0f};     ///< Label colour while hovered / focused.
    glm::vec4 textDisabled{0.5f, 0.5f, 0.5f, 1.0f}; ///< Label colour while disabled.
    glm::vec4 focusBorder{1.0f, 0.8f, 0.4f, 1.0f};  ///< Border drawn while it has keyboard / pad focus.
    bool selected = false; ///< stays in the hover look (tabs, chosen items)
    bool focusable() const override { return enabled && visible; }
    bool interactive() const override { return true; }
    void drawSelf(Painter& p) override;

private:
    Label* m_label = nullptr;
};

/// A bar (health, stamina, loading): value 0..1, with a trail that follows
/// drops a moment later (shows how much was just lost).
class ProgressBar : public Element {
public:
    using Element::Element;
    float value = 1.0f;                           ///< 0..1.
    std::function<float()> bind; ///< if set, the value comes from here each frame
    glm::vec4 fill{0.8f, 0.2f, 0.2f, 1.0f};       ///< Bar colour (sRGB).
    glm::vec4 fillEnd{-1.0f}; ///< alpha >= 0: horizontal gradient to this
    glm::vec4 trail{1.0f, 0.9f, 0.6f, 0.8f};      ///< Colour of the trail after a drop.
    glm::vec4 background{0.0f, 0.0f, 0.0f, 0.6f}; ///< Behind the bar.
    glm::vec4 borderColor{0.0f};                  ///< Border colour.
    float borderWidth = 0.0f;                     ///< Border thickness (0 = none).
    float rounding = 0.0f;                        ///< Corner radius.
    float inset = 2.0f;                           ///< Gap between the background's edge and the bar.
    void drawSelf(Painter& p) override;

private:
    float m_trail = 1.0f;
};

/// A value 0..1 set by dragging, clicking, or left / right when focused.
class Slider : public Element {
public:
    using Element::Element;
    float value = 0.5f;                       ///< 0..1.
    float step = 0.05f;                       ///< keyboard / gamepad step
    std::function<void(float)> onChanged;     ///< Called with the new value when the user changes it.
    glm::vec4 track{0.0f, 0.0f, 0.0f, 0.55f}; ///< The groove's colour (sRGB).
    glm::vec4 fill{0.9f, 0.7f, 0.35f, 1.0f};  ///< The filled part's colour.
    glm::vec4 knob{1.0f, 0.95f, 0.85f, 1.0f}; ///< The handle's colour.
    glm::vec4 focusColor{1.0f, 0.8f, 0.4f, 1.0f}; ///< Outline while focused.
    bool focusable() const override { return enabled && visible; }
    bool interactive() const override { return true; }
    void onPointer(glm::vec2 p) override;
    bool onNavigate(glm::vec2 dir) override;
    void drawSelf(Painter& p) override;
    /// Sets the value (clamped to 0..1) and calls onChanged if it changed.
    void set(float v);
};

/// Lays its visible children out in a row or column: each keeps its size along
/// the axis, and fills the cross axis if crossFill (else uses `cross` alignment).
class Stack : public Element {
public:
    using Element::Element;
    bool vertical = true;        ///< A column (true) or a row.
    float spacing = 8.0f;        ///< Gap between children (canvas units).
    glm::vec4 padding{0.0f}; ///< left, top, right, bottom
    bool crossFill = true;       ///< Children fill the cross axis (a column's width).
    Align cross = Align::Start;  ///< Cross-axis alignment when not crossFill.
    Align main = Align::Start; ///< where the children sit along the axis if there's room
    void layout() override;
    float contentLength() const; ///< the visible children's extents + spacing + padding along the axis
};

/// Fixed-size cells, left to right, top to bottom.
class Grid : public Element {
public:
    using Element::Element;
    int columns = 4;              ///< Cells per row.
    glm::vec2 cell{64.0f, 64.0f}; ///< Cell size (canvas units).
    glm::vec2 spacing{6.0f};      ///< Gaps between cells (x, y).
    glm::vec4 padding{0.0f};      ///< Left, top, right, bottom.
    void layout() override;
    glm::vec2 contentSize(int count) const; ///< the size `count` cells take
};

/// A one-line text field. Click it (or move the focus to it) and type; Enter
/// submits, Escape stops editing. Typed text comes from the OS (keyboard
/// layouts, IME); while a field is edited, gameplay gets no keys.
class TextInput : public Element {
public:
    using Element::Element;
    std::string text;                 ///< The current text (UTF-8); setText() to change it from code.
    std::string placeholder;          ///< shown (dimmed) while empty
    size_t maxLength = 64;            ///< bytes
    std::string font = "default";     ///< Font name (loadFont).
    float fontSize = 22.0f;           ///< Canvas units.
    glm::vec4 background{0.0f, 0.0f, 0.0f, 0.45f};     ///< Field fill (sRGB).
    glm::vec4 border{1.0f, 1.0f, 1.0f, 0.25f};         ///< Border at rest.
    glm::vec4 focusBorder{1.0f, 0.8f, 0.4f, 1.0f};     ///< Border while edited / focused.
    glm::vec4 textColor{1.0f};                         ///< Text colour.
    glm::vec4 placeholderColor{1.0f, 1.0f, 1.0f, 0.4f};///< Placeholder colour.
    glm::vec4 caretColor{1.0f, 0.85f, 0.5f, 1.0f};     ///< Caret colour.
    float rounding = 4.0f;            ///< Corner radius.
    float paddingX = 10.0f;           ///< Space left and right of the text.
    std::function<void(const std::string&)> onChanged; ///< Called with the new text after each edit.
    std::function<void(const std::string&)> onSubmit; ///< Enter
    std::function<bool(char32_t)> accept;             ///< which characters may be typed (null = any printable)

    bool focusable() const override { return enabled && visible; }
    bool interactive() const override { return true; }
    bool wantsText() const override { return true; }
    void onPointer(glm::vec2 p) override; ///< places the caret
    void drawSelf(Painter& p) override;
    /// Replaces the text (caret at the end); doesn't call onChanged.
    void setText(std::string t);
    /// The caret's byte offset (npos = at the end).
    size_t cursor() const { return m_cursor; }
    /// System, while it's being edited: typed text and the editing keys.
    void edit(const Input& in, float dt);

private:
    size_t m_cursor = std::string::npos; ///< byte offset (npos = the end)
    float m_scroll = 0.0f;               ///< horizontal, when the text is wider than the field
    float m_blink = 0.0f;
    float m_lastScale = 1.0f;
};

/// A scrolling list: add items to content() (a vertical Stack); wheel, drag
/// the bar or the background, and keyboard / pad focus scrolls to the focused item.
class ScrollView : public Element {
public:
    /// An empty scroll view (add items to content()).
    explicit ScrollView(std::string name = {});
    /// The scrolled list: add the items here.
    Stack& content() { return *m_content; }
    float scroll = 0.0f;              ///< canvas units from the top
    float wheelStep = 70.0f;          ///< Canvas units scrolled per mouse-wheel notch.
    float barWidth = 8.0f;            ///< Scroll bar width (canvas units).
    glm::vec4 barTrack{1.0f, 1.0f, 1.0f, 0.08f}; ///< Scroll bar background (sRGB).
    glm::vec4 barThumb{1.0f, 1.0f, 1.0f, 0.35f}; ///< Scroll bar handle.

    bool clipsChildren() const override { return true; }
    bool interactive() const override { return overflows(); }
    void onPointer(glm::vec2 p) override;
    void layout() override;
    void draw(Painter& p) override;
    /// Whether the content is taller than the view (it can scroll).
    bool overflows() const { return m_contentHeight > m_rect.size().y + 0.5f; }
    /// The largest `scroll` value (content height - view height, or 0).
    float maxScroll() const { return std::max(0.0f, m_contentHeight - m_rect.size().y); }
    /// The content's height (canvas units, as of the last layout).
    float contentHeight() const { return m_contentHeight; }
    /// Scrolls by `amount` canvas units (positive = down), clamped.
    void scrollBy(float amount) { scroll = std::clamp(scroll + amount, 0.0f, maxScroll()); }
    void scrollTo(const Element& e); ///< the least scrolling that shows e (an element in the content)

private:
    Stack* m_content = nullptr;
    float m_contentHeight = 0.0f;
    bool m_drag = false, m_dragBar = false;
    float m_grabY = 0.0f, m_grabScroll = 0.0f;
};

// ---- Canvas ----------------------------------------------------------------------------
/// A screen of game UI (HUD, menu, inventory): a component whose root element
/// fills the window. Canvases draw over the world, in `order`, and take input
/// before gameplay (see the top of this file).
class Canvas : public Component {
public:
    Canvas();
    ~Canvas() override;
    /// The root element (fills the canvas): add the UI to it.
    Element& root() { return *m_root; }

    glm::vec2 referenceSize{1920.0f, 1080.0f}; ///< scaled to the window by height
    int order = 0;           ///< higher draws on top and gets input first
    bool modal = false;      ///< while visible: focus navigation here, gameplay input blocked
    bool hideInEditor = true; ///< Not drawn while the editor is open in Edit mode.
    bool visible = true;      ///< Hidden canvases aren't drawn and take no input.
    float fade = 1.0f;       ///< overall opacity (fade in / out)
    std::function<void()> onBack; ///< Escape / pad B
    /// Keys / pad buttons this canvas reacts to while it's the top modal one
    /// (gameplay can't read them then): e.g. the inventory closing on Tab.
    struct Shortcut {
        Key key = Key::Unknown;                   ///< A key (Unknown = none).
        GamepadButton pad = GamepadButton::Count; ///< A pad button (Count = none).
        std::function<void()> action;             ///< What it does.
    };
    std::vector<Shortcut> shortcuts;              ///< See Shortcut.
    std::string tooltipFont = "default";          ///< Font for tooltips (loadFont name).
    glm::vec4 tooltipBackground = rgba(0x101010e6); ///< Tooltip box colour (sRGB).
    glm::vec4 tooltipText = rgba(0xf0e6d2ff);       ///< Tooltip text colour (sRGB).

    void focus(Element* e);   ///< keyboard / gamepad focus (null = none)
    /// The element with keyboard / gamepad focus, or null.
    Element* focused() const { return m_focus; }
    /// Canvas units to pixels (window height / referenceSize.y).
    float scale() const { return m_scale; }
    Element* hit(glm::vec2 point); ///< topmost element under a canvas point that takes the pointer

    void start() override;
    void onDestroy() override;

private:
    friend class System;
    std::unique_ptr<Element> m_root;
    Element* m_focus = nullptr;
    float m_scale = 1.0f;
    bool m_registered = false;
};

/// The engine side: owns the canvases' input routing and drawing (UiLayer calls it).
class System {
public:
    /// The UI system (one per process).
    static System& get();
    /// Before gameplay: pointer, focus and drag & drop; says what to take from gameplay.
    void route(Engine& engine);
    void draw(Engine& engine);   ///< after gameplay's onGui
    /// The pointer is over a canvas element: gameplay shouldn't react to the mouse.
    bool wantsMouse() const { return m_wantsMouse; }
    /// A modal canvas is up (or a text field is edited): gameplay gets no input.
    bool blocksGameplay() const { return m_block; }

    /// Engine internal: a Canvas registers itself.
    void add(Canvas* c);
    /// Engine internal: a Canvas unregisters itself.
    void remove(Canvas* c);
    void applyPendingFonts();   ///< before ImGui::NewFrame
    /// The baked size of font `name` closest to `pixelSize` (null = ImGui's default font).
    ImFont* font(const std::string& name, float pixelSize) const;
    void* textureId(const std::shared_ptr<Texture>& texture); ///< kept alive while cached
    void forget(const Element* e); ///< an element is going away
    void releaseTextures();     ///< at shutdown
    /// loadFont's implementation: the font is baked before the next frame.
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
    /// ImGui descriptor sets for textures drawn by Image elements (evictTextures drops unused ones).
    struct TextureEntry {
        std::shared_ptr<Texture> texture;
        void* id = nullptr;
        uint64_t lastUsed = 0; ///< renderer frame number
    };
    std::unordered_map<const Texture*, TextureEntry> m_textures;
    void evictTextures();
    bool m_wantsMouse = false, m_block = false;
    /// Pointer state.
    Element* m_down = nullptr;
    Canvas* m_downCanvas = nullptr;
    Element* m_dragging = nullptr;
    glm::vec2 m_downAt{0.0f};
    Element* m_hover = nullptr;
    float m_hoverTime = 0.0f;
    float m_navRepeat = 0.0f;
    glm::vec2 m_pointer{0.0f};
    uint64_t m_forgotten = 0;       ///< counts destroyed elements (handlers that rebuild the UI)
    TextInput* m_editing = nullptr; ///< the text field being typed into
    Canvas* m_editingCanvas = nullptr;
public:
    /// The text field being typed into, or null.
    TextInput* editing() const { return m_editing; }
};

} // namespace ui
} // namespace ve
