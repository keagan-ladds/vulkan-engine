#include "ve/ui/Gui.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/Input.hpp"
#include "ve/core/Log.hpp"
#include "ve/editor/Editor.hpp"
#include "ve/gfx/Samplers.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/ui/UiLayer.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace ve::ui {

// -----------------------------------------------------------------------------
// Colours / fonts
// -----------------------------------------------------------------------------

glm::vec4 rgba(uint32_t hex)
{
    return {float((hex >> 24) & 0xFF) / 255.0f, float((hex >> 16) & 0xFF) / 255.0f, float((hex >> 8) & 0xFF) / 255.0f,
            float(hex & 0xFF) / 255.0f};
}

void loadFont(const std::string& name, const std::string& path, std::vector<float> sizes)
{
    System::get().queueFont(name, path, std::move(sizes));
}

namespace {
float toLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
} // namespace

// -----------------------------------------------------------------------------
// Painter
// -----------------------------------------------------------------------------

uint32_t Painter::pack(glm::vec4 c) const
{
    // The backbuffer is sRGB: ImGui's vertex colours must be linear.
    return ImGui::ColorConvertFloat4ToU32(ImVec4(toLinear(std::clamp(c.r, 0.0f, 1.0f)), toLinear(std::clamp(c.g, 0.0f, 1.0f)),
                                                 toLinear(std::clamp(c.b, 0.0f, 1.0f)), std::clamp(c.a * opacity, 0.0f, 1.0f)));
}

void Painter::rect(const Rect& r, glm::vec4 color, float rounding)
{
    if (color.a * opacity <= 0.001f) return;
    const glm::vec2 a = toPixels(r.min), b = toPixels(r.max);
    list->AddRectFilled({a.x, a.y}, {b.x, b.y}, pack(color), rounding * scale);
}

void Painter::gradientH(const Rect& r, glm::vec4 left, glm::vec4 right)
{
    const glm::vec2 a = toPixels(r.min), b = toPixels(r.max);
    list->AddRectFilledMultiColor({a.x, a.y}, {b.x, b.y}, pack(left), pack(right), pack(right), pack(left));
}

void Painter::gradient(const Rect& r, glm::vec4 top, glm::vec4 bottom, float rounding)
{
    const glm::vec2 a = toPixels(r.min), b = toPixels(r.max);
    if (rounding <= 0.0f) {
        list->AddRectFilledMultiColor({a.x, a.y}, {b.x, b.y}, pack(top), pack(top), pack(bottom), pack(bottom));
        return;
    }
    // Rounded: approximate with horizontal bands (the corners keep their rounding).
    constexpr int kBands = 8;
    for (int i = 0; i < kBands; ++i) {
        const float t0 = float(i) / kBands, t1 = float(i + 1) / kBands;
        ImDrawFlags flags = ImDrawFlags_RoundCornersNone;
        if (i == 0) flags = ImDrawFlags_RoundCornersTop;
        if (i == kBands - 1) flags = ImDrawFlags_RoundCornersBottom;
        list->AddRectFilled({a.x, a.y + (b.y - a.y) * t0}, {b.x, a.y + (b.y - a.y) * t1 + (i < kBands - 1 ? 1.0f : 0.0f)},
                            pack(glm::mix(top, bottom, (t0 + t1) * 0.5f)), rounding * scale, flags);
    }
}

void Painter::border(const Rect& r, glm::vec4 color, float thickness, float rounding)
{
    if (color.a * opacity <= 0.001f || thickness <= 0.0f) return;
    const glm::vec2 a = toPixels(r.min), b = toPixels(r.max);
    list->AddRect({a.x, a.y}, {b.x, b.y}, pack(color), rounding * scale, 0, std::max(1.0f, thickness * scale));
}

void Painter::line(glm::vec2 a, glm::vec2 b, glm::vec4 color, float thickness)
{
    const glm::vec2 pa = toPixels(a), pb = toPixels(b);
    list->AddLine({pa.x, pa.y}, {pb.x, pb.y}, pack(color), std::max(1.0f, thickness * scale));
}

void Painter::image(const std::shared_ptr<Texture>& texture, const Rect& r, glm::vec4 tint, glm::vec2 uv0, glm::vec2 uv1, float rounding)
{
    if (!texture) return;
    void* id = System::get().textureId(texture);
    if (!id) return;
    const glm::vec2 a = toPixels(r.min), b = toPixels(r.max);
    list->AddImageRounded(ImTextureID(id), {a.x, a.y}, {b.x, b.y}, {uv0.x, uv0.y}, {uv1.x, uv1.y}, pack(tint), rounding * scale);
}

glm::vec2 Painter::measure(const std::string& text, const std::string& font, float size, float wrapWidth) const
{
    ImFont* f = System::get().font(font, size * scale);
    if (!f) return glm::vec2(0.0f);
    const ImVec2 s = f->CalcTextSizeA(size * scale, FLT_MAX, wrapWidth > 0.0f ? wrapWidth * scale : 0.0f, text.c_str());
    return glm::vec2(s.x, s.y) / scale;
}

glm::vec2 Painter::text(const Rect& r, const std::string& text, const std::string& font, float size, glm::vec4 color, Align h, Align v,
                        bool wrap, glm::vec4 shadow)
{
    ImFont* f = System::get().font(font, size * scale);
    if (!f || text.empty()) return glm::vec2(0.0f);
    const float wrapPx = wrap ? r.size().x * scale : 0.0f;
    const ImVec2 s = f->CalcTextSizeA(size * scale, FLT_MAX, wrapPx, text.c_str());
    glm::vec2 pos = toPixels(r.min);
    const glm::vec2 avail = r.size() * scale;
    auto offset = [](Align a, float room) { return a == Align::Start ? 0.0f : a == Align::Center ? room * 0.5f : room; };
    pos.x += offset(h, avail.x - s.x);
    pos.y += offset(v, avail.y - s.y);
    pos = glm::floor(pos + 0.5f); // crisp glyphs
    if (shadow.a > 0.0f) {
        const float o = std::max(1.0f, size * scale * 0.06f);
        list->AddText(f, size * scale, {pos.x + o, pos.y + o}, pack(shadow), text.c_str(), nullptr, wrapPx);
    }
    list->AddText(f, size * scale, {pos.x, pos.y}, pack(color), text.c_str(), nullptr, wrapPx);
    return glm::vec2(s.x, s.y) / scale;
}

void Painter::polygon(const std::vector<glm::vec2>& points, glm::vec4 color)
{
    std::vector<ImVec2> px;
    px.reserve(points.size());
    for (const glm::vec2& p : points) px.push_back({p.x * scale, p.y * scale});
    list->AddConvexPolyFilled(px.data(), int(px.size()), pack(color));
}

void Painter::circle(glm::vec2 center, float radius, glm::vec4 color, bool filled, float thickness)
{
    const glm::vec2 c = toPixels(center);
    if (filled) list->AddCircleFilled({c.x, c.y}, radius * scale, pack(color));
    else list->AddCircle({c.x, c.y}, radius * scale, pack(color), 0, std::max(1.0f, thickness * scale));
}

// -----------------------------------------------------------------------------
// Element
// -----------------------------------------------------------------------------

Element::~Element()
{
    System::get().forget(this);
}

Element& Element::fill(glm::vec4 m)
{
    anchorMin = glm::vec2(0.0f);
    anchorMax = glm::vec2(1.0f);
    position = {m.x, m.y};
    extent = {m.z, m.w};
    return *this;
}

void Element::remove(Element& child)
{
    std::erase_if(m_children, [&](const std::unique_ptr<Element>& c) { return c.get() == &child; });
}

Element* Element::find(const std::string& n)
{
    if (name == n) return this;
    for (auto& c : m_children)
        if (Element* e = c->find(n)) return e;
    return nullptr;
}

bool Element::shown() const
{
    for (const Element* e = this; e; e = e->m_parent)
        if (!e->visible) return false;
    return true;
}

void Element::click()
{
    if (enabled && onClick) onClick();
}

void Element::arrange(const Rect& parent)
{
    const glm::vec2 ps = parent.size();
    const glm::vec2 lo = parent.min + anchorMin * ps, hi = parent.min + anchorMax * ps;
    for (int a = 0; a < 2; ++a) {
        if (anchorMin[a] == anchorMax[a]) { // fixed size around the anchor
            m_rect.min[a] = lo[a] + position[a] - pivotPoint[a] * extent[a];
            m_rect.max[a] = m_rect.min[a] + extent[a];
        } else { // stretched between the anchors, inset
            m_rect.min[a] = lo[a] + position[a];
            m_rect.max[a] = hi[a] - extent[a];
        }
    }
    layout();
}

void Element::place(const Rect& r)
{
    m_rect = r;
    layout();
}

void Element::layout()
{
    for (auto& c : m_children)
        if (c->visible) c->arrange(m_rect);
}

void Element::draw(Painter& p)
{
    if (!visible) return;
    const float saved = p.opacity;
    p.opacity *= opacity;
    if (p.opacity > 0.001f) {
        drawSelf(p);
        for (auto& c : m_children) c->draw(p);
    }
    p.opacity = saved;
}

// -----------------------------------------------------------------------------
// Widgets
// -----------------------------------------------------------------------------

void Panel::drawSelf(Painter& p)
{
    if (gradientBottom.a >= 0.0f) p.gradient(m_rect, color, gradientBottom, rounding);
    else p.rect(m_rect, color, rounding);
    if (image) p.image(image, m_rect, imageTint, glm::vec2(0.0f), glm::vec2(1.0f), rounding);
    p.border(m_rect, borderColor, borderWidth, rounding);
}

void Label::drawSelf(Painter& p)
{
    if (bind) text = bind();
    p.text(m_rect, text, font, fontSize, color, alignX, alignY, wrap, shadow);
}

void Image::drawSelf(Painter& p)
{
    if (custom) {
        custom(p, m_rect);
        return;
    }
    if (!texture) return;
    Rect r = m_rect;
    if (keepAspect && texture->height() > 0) {
        const float aspect = float(texture->width()) / float(texture->height());
        glm::vec2 s = r.size();
        if (s.x / std::max(s.y, 1e-3f) > aspect) s.x = s.y * aspect;
        else s.y = s.x / aspect;
        r.min = m_rect.center() - s * 0.5f;
        r.max = r.min + s;
    }
    p.image(texture, r, tint);
}

Button::Button(std::string text, std::string name) : Panel(name.empty() ? text : std::move(name))
{
    m_label = &add<Label>(std::move(text), "label");
    m_label->fill();
    m_label->alignX = Align::Center;
    normal = rgba(0x2a221ccc);
    hover = rgba(0x4a3a2aee);
    down = rgba(0x6a4a2aff);
    disabled = rgba(0x1a1a1a99);
}

void Button::drawSelf(Painter& p)
{
    const bool hot = enabled && (hovered() || focused() || selected);
    color = !enabled ? disabled : pressed() ? down : hot ? hover : normal;
    m_label->color = !enabled ? textDisabled : hot ? textHover : textNormal;
    Panel::drawSelf(p);
    if (focused() && enabled) p.border(m_rect, focusBorder, 2.0f, rounding);
}

void ProgressBar::drawSelf(Painter& p)
{
    if (bind) value = bind();
    value = std::clamp(value, 0.0f, 1.0f);
    // The trail catches up with a drop after a moment; rises are immediate.
    const float dt = ImGui::GetIO().DeltaTime;
    m_trail = value >= m_trail ? value : std::max(value, m_trail - dt * 0.6f);
    p.rect(m_rect, background, rounding);
    Rect inner{m_rect.min + inset, m_rect.max - inset};
    const float w = inner.size().x;
    if (m_trail > value) p.rect({{inner.min.x + w * value, inner.min.y}, {inner.min.x + w * m_trail, inner.max.y}}, trail, rounding * 0.5f);
    if (value > 0.0f) {
        const Rect f{inner.min, {inner.min.x + w * value, inner.max.y}};
        if (fillEnd.a >= 0.0f) {
            const glm::vec2 a = p.toPixels(f.min), b = p.toPixels(f.max);
            const uint32_t c0 = p.pack(fill), c1 = p.pack(glm::mix(fill, fillEnd, value));
            p.list->AddRectFilledMultiColor({a.x, a.y}, {b.x, b.y}, c0, c1, c1, c0);
        } else {
            p.rect(f, fill, rounding * 0.5f);
        }
    }
    p.border(m_rect, borderColor, borderWidth, rounding);
}

void Slider::set(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    if (v == value) return;
    value = v;
    if (onChanged) onChanged(value);
}

void Slider::onPointer(glm::vec2 p)
{
    const float knobR = m_rect.size().y * 0.5f;
    set((p.x - m_rect.min.x - knobR) / std::max(m_rect.size().x - 2.0f * knobR, 1.0f));
}

bool Slider::onNavigate(glm::vec2 dir)
{
    if (dir.x == 0.0f) return false;
    set(value + step * glm::sign(dir.x));
    return true;
}

void Slider::drawSelf(Painter& p)
{
    const float h = m_rect.size().y, knobR = h * 0.5f;
    const float y = m_rect.center().y, x0 = m_rect.min.x + knobR, x1 = m_rect.max.x - knobR;
    const float th = std::max(h * 0.22f, 3.0f);
    p.rect({{x0, y - th * 0.5f}, {x1, y + th * 0.5f}}, track, th * 0.5f);
    const float xv = x0 + (x1 - x0) * value;
    p.rect({{x0, y - th * 0.5f}, {xv, y + th * 0.5f}}, fill, th * 0.5f);
    p.circle({xv, y}, knobR * (hovered() || pressed() ? 0.8f : 0.68f), knob);
    if (focused()) p.circle({xv, y}, knobR * 0.95f, focusColor, false, 2.0f);
}

void Stack::layout()
{
    const int axis = vertical ? 1 : 0, crossAxis = 1 - axis;
    const Rect inner{m_rect.min + glm::vec2(padding.x, padding.y), m_rect.max - glm::vec2(padding.z, padding.w)};
    float total = 0.0f;
    int count = 0;
    for (auto& c : children())
        if (c->visible) {
            total += c->extent[axis];
            ++count;
        }
    total += spacing * float(std::max(count - 1, 0));
    const float room = inner.size()[axis] - total;
    float cursor = inner.min[axis] + (main == Align::Center ? room * 0.5f : main == Align::End ? room : 0.0f);
    for (auto& c : children()) {
        if (!c->visible) continue;
        Rect r;
        r.min[axis] = cursor;
        r.max[axis] = cursor + c->extent[axis];
        if (crossFill) {
            r.min[crossAxis] = inner.min[crossAxis];
            r.max[crossAxis] = inner.max[crossAxis];
        } else {
            const float s = c->extent[crossAxis], free = inner.size()[crossAxis] - s;
            r.min[crossAxis] = inner.min[crossAxis] + (cross == Align::Center ? free * 0.5f : cross == Align::End ? free : 0.0f);
            r.max[crossAxis] = r.min[crossAxis] + s;
        }
        c->place(r);
        cursor += c->extent[axis] + spacing;
    }
}

void Grid::layout()
{
    int i = 0;
    const int cols = std::max(columns, 1);
    for (auto& c : children()) {
        if (!c->visible) continue;
        const glm::vec2 at = m_rect.min + glm::vec2(padding.x, padding.y) +
                             glm::vec2(float(i % cols), float(i / cols)) * (cell + spacing);
        c->place({at, at + cell});
        ++i;
    }
}

glm::vec2 Grid::contentSize(int count) const
{
    const int cols = std::max(columns, 1), rows = (count + cols - 1) / cols;
    return glm::vec2(padding.x + padding.z, padding.y + padding.w) +
           glm::vec2(float(std::min(count, cols)), float(rows)) * cell + glm::vec2(float(std::max(std::min(count, cols) - 1, 0)), float(std::max(rows - 1, 0))) * spacing;
}

// -----------------------------------------------------------------------------
// Canvas
// -----------------------------------------------------------------------------

Canvas::Canvas() : m_root(std::make_unique<Element>("root"))
{
    m_root->fill();
}

Canvas::~Canvas()
{
    if (m_registered) System::get().remove(this);
}

void Canvas::start()
{
    System::get().add(this);
    m_registered = true;
}

void Canvas::onDestroy()
{
    System::get().remove(this);
    m_registered = false;
}

void Canvas::focus(Element* e)
{
    if (m_focus) m_focus->m_focused = false;
    m_focus = e;
    if (m_focus) m_focus->m_focused = true;
}

Element* Canvas::hit(glm::vec2 point)
{
    std::function<Element*(Element&)> rec = [&](Element& e) -> Element* {
        if (!e.visible) return nullptr;
        for (auto it = e.m_children.rbegin(); it != e.m_children.rend(); ++it) // last drawn is on top
            if (Element* h = rec(**it)) return h;
        if (&e != m_root.get() && e.m_rect.contains(point) && (e.interactive() || e.blocksPointer)) return &e;
        return nullptr;
    };
    return rec(*m_root);
}

// -----------------------------------------------------------------------------
// System
// -----------------------------------------------------------------------------

System& System::get()
{
    static System s;
    return s;
}

void System::add(Canvas* c)
{
    if (std::find(m_canvases.begin(), m_canvases.end(), c) == m_canvases.end()) m_canvases.push_back(c);
}

void System::remove(Canvas* c)
{
    std::erase(m_canvases, c);
    if (m_downCanvas == c) {
        m_downCanvas = nullptr;
        m_down = m_dragging = nullptr;
    }
    m_hover = nullptr;
}

void System::forget(const Element* e)
{
    if (m_down == e) m_down = nullptr;
    if (m_dragging == e) m_dragging = nullptr;
    if (m_hover == e) m_hover = nullptr;
    for (Canvas* c : m_canvases)
        if (c->m_focus == e) c->m_focus = nullptr;
}

bool System::shownNow(const Canvas& c, Engine& engine) const
{
    if (!c.visible || c.fade <= 0.01f || !c.isActive()) return false;
    if (c.hideInEditor && engine.editor() && engine.editor()->paused()) return false;
    return true;
}

void System::queueFont(const std::string& name, const std::string& path, std::vector<float> sizes)
{
    m_pending.push_back({name, path, std::move(sizes)});
}

void System::applyPendingFonts()
{
    if (m_pending.empty()) return;
    // The atlas texture is rebuilt: wait for frames using it, then let the backend recreate it.
    Engine::get().renderer().device().waitIdle();
    ImGui_ImplVulkan_DestroyFontsTexture();
    ImGuiIO& io = ImGui::GetIO();
    for (const PendingFont& f : m_pending) {
        std::string file;
        try {
            file = Engine::get().assets().resolve(f.path).string();
        } catch (const std::exception& e) {
            log::warn("UI font '{}': {}", f.name, e.what());
            continue;
        }
        for (float size : f.sizes) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg)) m_fonts.push_back({f.name, size, font});
            else log::warn("UI font '{}': can't load {}", f.name, file);
        }
    }
    m_pending.clear();
    io.Fonts->Build();
}

ImFont* System::font(const std::string& name, float pixelSize) const
{
    // The smallest baked size at least as big (crisp when scaled down), else the biggest.
    const FontEntry* best = nullptr;
    for (const FontEntry& f : m_fonts) {
        if (f.name != name) continue;
        if (!best) best = &f;
        else if (f.size >= pixelSize && (best->size < pixelSize || f.size < best->size)) best = &f;
        else if (best->size < pixelSize && f.size > best->size) best = &f;
    }
    return best ? best->font : ImGui::GetFont();
}

void* System::textureId(const std::shared_ptr<Texture>& texture)
{
    for (auto& [t, id] : m_textures)
        if (t == texture) return id;
    const VkSampler sampler = Engine::get().renderer().samplers().get(SamplerType::LinearClamp);
    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(sampler, texture->image().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_textures.push_back({texture, reinterpret_cast<void*>(set)});
    return reinterpret_cast<void*>(set);
}

void System::releaseTextures()
{
    for (auto& [t, id] : m_textures) ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(id));
    m_textures.clear();
}

void System::route(Engine& engine)
{
    Input& in = engine.input();
    UiLayer& layer = engine.ui();
    const bool imguiKeyboard = layer.wantsKeyboard(), imguiMouse = layer.wantsMouse();
    in.setUiCapture(false, false); // read the raw state below
    const float dt = engine.time().unscaledDeltaTime;

    std::vector<Canvas*> shown;
    for (Canvas* c : m_canvases)
        if (shownNow(*c, engine)) shown.push_back(c);
    std::stable_sort(shown.begin(), shown.end(), [](const Canvas* a, const Canvas* b) { return a->order > b->order; });
    Canvas* modal = nullptr;
    for (Canvas* c : shown)
        if (c->modal) {
            modal = c;
            break;
        }

    // --- Pointer ---
    const bool pointer = !imguiMouse && !in.mouseCaptured();
    m_pointer = in.mousePosition();
    Element* hover = nullptr;
    Canvas* hoverCanvas = nullptr;
    bool overUi = false;
    if (pointer) {
        for (Canvas* c : shown) {
            if (Element* h = c->hit(m_pointer / c->m_scale)) {
                hover = h;
                hoverCanvas = c;
                overUi = true;
                break;
            }
            if (c->modal) { // nothing below a modal canvas gets the pointer
                overUi = true;
                break;
            }
        }
    }
    if (hover != m_hover) {
        if (m_hover) {
            m_hover->m_hovered = false;
            if (m_hover->onHover) m_hover->onHover(false);
        }
        m_hover = hover;
        m_hoverTime = 0.0f;
        if (m_hover) {
            m_hover->m_hovered = true;
            if (m_hover->onHover) m_hover->onHover(true);
        }
    }
    m_hoverTime += dt;

    if (pointer && in.mousePressed(MouseButton::Left) && hover && hover->enabled) {
        m_down = hover;
        m_downCanvas = hoverCanvas;
        m_downAt = m_pointer;
        hover->m_pressed = true;
        if (hover->focusable()) hoverCanvas->focus(hover);
    }
    if (m_down && m_downCanvas) m_down->onPointer(m_pointer / m_downCanvas->m_scale);
    if (m_down && m_down->draggable && !m_dragging && glm::length(m_pointer - m_downAt) > 6.0f) m_dragging = m_down;
    if (m_down && !in.mouseDown(MouseButton::Left)) {
        Element* down = m_down;
        down->m_pressed = false;
        m_down = nullptr;
        if (m_dragging) {
            Element* dragged = m_dragging;
            m_dragging = nullptr;
            for (Element* t = hover; t; t = t->m_parent)
                if (t->onDrop && t != dragged) {
                    t->onDrop(*dragged);
                    break;
                }
        } else if (down == hover && down->enabled && down->onClick) {
            down->onClick();
        }
    }

    // --- Focus (keyboard / gamepad) on the top modal canvas ---
    if (modal) {
        auto key = [&](Key k) { return in.keyPressed(k); };
        auto pad = [&](GamepadButton b) { return in.gamepadPressed(b); };
        glm::vec2 dir(0.0f);
        if (key(Key::Up) || pad(GamepadButton::DPadUp)) dir = {0, -1};
        if (key(Key::Down) || pad(GamepadButton::DPadDown)) dir = {0, 1};
        if (key(Key::Left) || pad(GamepadButton::DPadLeft)) dir = {-1, 0};
        if (key(Key::Right) || pad(GamepadButton::DPadRight)) dir = {1, 0};
        // The stick, with repeat.
        const glm::vec2 stick(in.gamepadAxis(GamepadAxis::LeftX), in.gamepadAxis(GamepadAxis::LeftY));
        m_navRepeat -= dt;
        if (dir == glm::vec2(0.0f) && glm::length(stick) > 0.6f && m_navRepeat <= 0.0f) {
            dir = std::abs(stick.x) > std::abs(stick.y) ? glm::vec2(glm::sign(stick.x), 0) : glm::vec2(0, glm::sign(stick.y));
            m_navRepeat = 0.22f;
        }
        if (glm::length(stick) < 0.3f) m_navRepeat = 0.0f;

        if (dir != glm::vec2(0.0f) && modal->m_focus && modal->m_focus->onNavigate(dir)) dir = glm::vec2(0.0f); // a slider took it
        if (dir != glm::vec2(0.0f)) {
            std::vector<Element*> candidates;
            std::function<void(Element&)> collect = [&](Element& e) {
                if (!e.visible) return;
                if (e.focusable()) candidates.push_back(&e);
                for (auto& c : e.m_children) collect(*c);
            };
            collect(*modal->m_root);
            Element* cur = modal->m_focus;
            Element* best = nullptr;
            if (!cur) {
                // First focus: the top-left one.
                for (Element* e : candidates)
                    if (!best || e->m_rect.min.y + e->m_rect.min.x * 0.1f < best->m_rect.min.y + best->m_rect.min.x * 0.1f) best = e;
            } else {
                float bestScore = FLT_MAX;
                const glm::vec2 from = cur->m_rect.center();
                for (Element* e : candidates) {
                    if (e == cur) continue;
                    const glm::vec2 d = e->m_rect.center() - from;
                    const float along = glm::dot(d, dir);
                    if (along <= 1.0f) continue;
                    const float across = std::abs(dir.x != 0.0f ? d.y : d.x);
                    const float score = along + across * 2.5f;
                    if (score < bestScore) {
                        bestScore = score;
                        best = e;
                    }
                }
            }
            if (best) modal->focus(best);
        }
        if (modal->m_focus && (key(Key::Enter) || key(Key::Space) || pad(GamepadButton::A))) modal->m_focus->click();
        if (modal->onBack && (key(Key::Escape) || pad(GamepadButton::B))) modal->onBack();
        for (const Canvas::Shortcut& sc : std::vector<Canvas::Shortcut>(modal->shortcuts)) // (an action may change the list)
            if (sc.action && ((sc.key != Key::Unknown && key(sc.key)) || (sc.pad != GamepadButton::Count && pad(sc.pad)))) {
                sc.action();
                break;
            }
    }

    m_block = modal != nullptr;
    m_wantsMouse = overUi || m_down != nullptr;
    in.setUiCapture(imguiKeyboard || m_block, imguiMouse || m_wantsMouse || m_block, m_block);
}

void System::draw(Engine& engine)
{
    ImGuiIO& io = ImGui::GetIO();
    std::vector<Canvas*> shown;
    for (Canvas* c : m_canvases)
        if (shownNow(*c, engine)) shown.push_back(c);
    std::stable_sort(shown.begin(), shown.end(), [](const Canvas* a, const Canvas* b) { return a->order < b->order; });
    Painter p;
    p.list = ImGui::GetBackgroundDrawList();
    for (Canvas* c : shown) {
        c->m_scale = io.DisplaySize.y / std::max(c->referenceSize.y, 1.0f);
        p.scale = c->m_scale;
        p.opacity = c->fade;
        const glm::vec2 size = glm::vec2(io.DisplaySize.x, io.DisplaySize.y) / c->m_scale;
        c->m_root->place({glm::vec2(0.0f), size});
        c->m_root->draw(p);
    }
    // Over everything: what's being dragged, and the hovered element's tooltip.
    Canvas* top = shown.empty() ? nullptr : shown.back();
    const float scale = top ? top->m_scale : 1.0f;
    p.scale = scale;
    p.opacity = 1.0f;
    if (m_dragging && m_dragging->dragVisual) m_dragging->dragVisual(p, m_pointer / scale);
    if (m_hover && !m_dragging && !m_hover->tooltip.empty() && m_hoverTime > 0.45f && top) {
        const std::string& font = top->tooltipFont;
        const glm::vec2 s = p.measure(m_hover->tooltip, font, 20.0f, 420.0f);
        glm::vec2 at = m_pointer / scale + glm::vec2(18.0f, 20.0f);
        const glm::vec2 screen = glm::vec2(io.DisplaySize.x, io.DisplaySize.y) / scale;
        at = glm::min(at, screen - s - glm::vec2(24.0f));
        const Rect r{at, at + s + glm::vec2(20.0f, 14.0f)};
        p.rect(r, top->tooltipBackground, 6.0f);
        p.text({r.min + glm::vec2(10.0f, 7.0f), r.max - glm::vec2(10.0f, 7.0f)}, m_hover->tooltip, font, 20.0f, top->tooltipText, Align::Start,
               Align::Start, true);
    }
}

} // namespace ve::ui
