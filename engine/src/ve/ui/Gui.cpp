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
    // Rounded: one rounded rect, its vertices recoloured along the gradient
    // (the anti-aliased edge keeps its fade: alpha relative to the fill's).
    const ImU32 fill = pack(top);
    const float fillAlpha = float((fill >> IM_COL32_A_SHIFT) & 0xFF);
    if (fillAlpha <= 0.0f && pack(bottom) == fill) return;
    const int first = list->VtxBuffer.Size;
    list->AddRectFilled({a.x, a.y}, {b.x, b.y}, fill | (fillAlpha <= 0.0f ? IM_COL32_A_MASK : 0u), rounding * scale);
    const float baseAlpha = fillAlpha > 0.0f ? fillAlpha : 255.0f;
    const float h = std::max(b.y - a.y, 1e-3f);
    for (int i = first; i < list->VtxBuffer.Size; ++i) {
        ImDrawVert& v = list->VtxBuffer[i];
        const float edge = float((v.col >> IM_COL32_A_SHIFT) & 0xFF) / baseAlpha; // 1 inside, -> 0 on the AA fringe
        const ImU32 c = pack(glm::mix(top, bottom, std::clamp((v.pos.y - a.y) / h, 0.0f, 1.0f)));
        const auto alpha = ImU32(std::lround(float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(edge, 0.0f, 1.0f)));
        v.col = (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
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

void Painter::pushClip(const Rect& r)
{
    const glm::vec2 a = toPixels(r.min), b = toPixels(r.max);
    list->PushClipRect({a.x, a.y}, {b.x, b.y}, true);
}

void Painter::popClip()
{
    list->PopClipRect();
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
    // Call a copy: a handler that rebuilds the UI destroys this element, and
    // with it the stored std::function that is still running.
    if (!enabled || !onClick) return;
    const std::function<void()> fn = onClick;
    fn();
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
    if (onChanged) {
        const std::function<void(float)> fn = onChanged; // (the handler may destroy this slider)
        fn(v);
    }
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

float Stack::contentLength() const
{
    const int axis = vertical ? 1 : 0;
    float total = 0.0f;
    int count = 0;
    for (const auto& c : children())
        if (c->visible) {
            total += c->extent[axis];
            ++count;
        }
    return total + spacing * float(std::max(count - 1, 0)) + (vertical ? padding.y + padding.w : padding.x + padding.z);
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
// TextInput
// -----------------------------------------------------------------------------

namespace {
bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
size_t prevBoundary(const std::string& s, size_t i)
{
    if (i == 0) return 0;
    --i;
    while (i > 0 && continuation(s[i])) --i;
    return i;
}
size_t nextBoundary(const std::string& s, size_t i)
{
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && continuation(s[i])) ++i;
    return i;
}
char32_t decode(const std::string& s, size_t& i) // UTF-8 -> code point, advances i
{
    const auto b = static_cast<unsigned char>(s[i]);
    int n = b < 0x80 ? 0 : b < 0xE0 ? 1 : b < 0xF0 ? 2 : 3;
    char32_t c = n == 0 ? b : n == 1 ? (b & 0x1F) : n == 2 ? (b & 0x0F) : (b & 0x07);
    ++i;
    for (; n > 0 && i < s.size(); --n, ++i) c = (c << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    return c;
}
} // namespace

void TextInput::setText(std::string t)
{
    text = std::move(t);
    if (text.size() > maxLength) text.resize(prevBoundary(text, maxLength + 1));
    m_cursor = std::string::npos;
}

void TextInput::edit(const Input& in, float dt)
{
    size_t cur = std::min(m_cursor, text.size());
    const std::string before = text;
    // Typed characters (filtered), at the caret.
    const std::string& typed = in.textTyped();
    for (size_t i = 0; i < typed.size();) {
        const size_t start = i;
        const char32_t c = decode(typed, i);
        if (c < 0x20 || c == 0x7F) continue;
        if (accept && !accept(c)) continue;
        const std::string piece = typed.substr(start, i - start);
        if (text.size() + piece.size() > maxLength) break;
        text.insert(cur, piece);
        cur += piece.size();
    }
    if (in.keyTyped(Key::Backspace) && cur > 0) {
        const size_t p = prevBoundary(text, cur);
        text.erase(p, cur - p);
        cur = p;
    }
    if (in.keyTyped(Key::Delete) && cur < text.size()) text.erase(cur, nextBoundary(text, cur) - cur);
    if (in.keyTyped(Key::Left)) cur = prevBoundary(text, cur);
    if (in.keyTyped(Key::Right)) cur = nextBoundary(text, cur);
    if (in.keyTyped(Key::Home)) cur = 0;
    if (in.keyTyped(Key::End)) cur = text.size();
    m_cursor = cur;
    m_blink = (text != before || in.keyTyped(Key::Left) || in.keyTyped(Key::Right)) ? 0.0f : m_blink + dt;
    // Copies of the handlers and the text: onChanged may rebuild the UI and
    // destroy this field, so nothing below may touch a member after it runs.
    const bool changed = text != before, submit = in.keyPressed(Key::Enter);
    const std::function<void(const std::string&)> changedFn = changed ? onChanged : nullptr;
    const std::function<void(const std::string&)> submitFn = submit ? onSubmit : nullptr;
    const std::string value = (changedFn || submitFn) ? text : std::string();
    if (changedFn) changedFn(value);
    if (submitFn) submitFn(value);
}

void TextInput::onPointer(glm::vec2 p)
{
    // The caret goes to the nearest character boundary under the pointer.
    Painter measure;
    measure.scale = m_lastScale;
    const float x = p.x - (m_rect.min.x + paddingX) + m_scroll;
    size_t best = 0;
    float bestD = std::abs(x);
    for (size_t i = nextBoundary(text, 0); i <= text.size(); i = nextBoundary(text, i)) {
        const float w = measure.measure(text.substr(0, i), font, fontSize).x;
        if (std::abs(w - x) < bestD) {
            bestD = std::abs(w - x);
            best = i;
        }
        if (i == text.size()) break;
    }
    m_cursor = best;
    m_blink = 0.0f;
}

void TextInput::drawSelf(Painter& p)
{
    m_lastScale = p.scale;
    const bool editing = focused() && System::get().editing() == this;
    p.rect(m_rect, background, rounding);
    p.border(m_rect, focused() ? focusBorder : border, focused() ? 2.0f : 1.0f, rounding);
    const Rect inner{m_rect.min + glm::vec2(paddingX, 0.0f), m_rect.max - glm::vec2(paddingX, 0.0f)};
    const size_t cur = std::min(m_cursor, text.size());
    // Keep the caret in view.
    const float caretX = p.measure(text.substr(0, cur), font, fontSize).x;
    const float width = inner.size().x;
    if (caretX - m_scroll > width - 2.0f) m_scroll = caretX - width + 2.0f;
    if (caretX - m_scroll < 0.0f) m_scroll = caretX;
    if (!editing) m_scroll = 0.0f;
    p.pushClip(inner);
    Rect textRect = inner;
    textRect.min.x -= m_scroll;
    textRect.max.x = std::max(textRect.max.x, textRect.min.x + caretX + 4.0f) + 4000.0f; // (never wraps or clips by alignment)
    if (text.empty() && !editing) p.text(textRect, placeholder, font, fontSize, placeholderColor, Align::Start, Align::Center);
    else p.text(textRect, text, font, fontSize, textColor, Align::Start, Align::Center);
    if (editing && std::fmod(m_blink, 1.0f) < 0.6f) {
        const float x = inner.min.x + caretX - m_scroll;
        const float h = fontSize * 0.62f;
        const float cy = m_rect.center().y;
        p.rect({{x, cy - h}, {x + 2.0f, cy + h}}, caretColor);
    }
    p.popClip();
}

// -----------------------------------------------------------------------------
// ScrollView
// -----------------------------------------------------------------------------

ScrollView::ScrollView(std::string name) : Element(std::move(name))
{
    m_content = &add<Stack>("content");
    m_content->spacing = 6.0f;
    blocksPointer = true;
}

void ScrollView::layout()
{
    if (!pressed()) m_drag = false;
    m_contentHeight = m_content->contentLength();
    scroll = std::clamp(scroll, 0.0f, maxScroll());
    const float bar = overflows() ? barWidth + 6.0f : 0.0f;
    const float h = std::max(m_rect.size().y, m_contentHeight);
    m_content->place({{m_rect.min.x, m_rect.min.y - scroll}, {m_rect.max.x - bar, m_rect.min.y - scroll + h}});
    for (auto& c : children())
        if (c.get() != m_content) c->arrange(m_rect); // anything else on the view itself (overlays)
}

void ScrollView::draw(Painter& p)
{
    if (!visible) return;
    const float saved = p.opacity;
    p.opacity *= opacity;
    if (p.opacity > 0.001f) {
        drawSelf(p);
        p.pushClip(m_rect);
        for (auto& c : children()) c->draw(p);
        p.popClip();
        if (overflows()) {
            const float viewH = m_rect.size().y;
            const Rect track{{m_rect.max.x - barWidth, m_rect.min.y}, m_rect.max};
            p.rect(track, barTrack, barWidth * 0.5f);
            const float thumbH = std::max(24.0f, viewH * viewH / m_contentHeight);
            const float y = m_rect.min.y + (maxScroll() > 0.0f ? scroll / maxScroll() : 0.0f) * (viewH - thumbH);
            p.rect({{track.min.x, y}, {track.max.x, y + thumbH}}, m_drag && m_dragBar ? withAlpha(barThumb, 0.6f) : barThumb, barWidth * 0.5f);
        }
    }
    p.opacity = saved;
}

void ScrollView::onPointer(glm::vec2 p)
{
    if (!overflows()) return;
    if (!m_drag) {
        m_drag = true;
        m_dragBar = p.x > m_rect.max.x - barWidth - 8.0f;
        m_grabY = p.y;
        m_grabScroll = scroll;
    }
    const float dy = p.y - m_grabY;
    const float viewH = m_rect.size().y;
    // The bar moves the thumb (content height / view height faster); the background drags the content.
    scroll = std::clamp(m_dragBar ? m_grabScroll + dy * m_contentHeight / std::max(viewH, 1.0f) : m_grabScroll - dy, 0.0f, maxScroll());
}

void ScrollView::scrollTo(const Element& e)
{
    const float top = e.rect().min.y - m_content->rect().min.y;
    const float bottom = e.rect().max.y - m_content->rect().min.y;
    const float viewH = m_rect.size().y;
    if (top < scroll) scroll = top;
    else if (bottom > scroll + viewH) scroll = bottom - viewH;
    scroll = std::clamp(scroll, 0.0f, maxScroll());
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
        const bool inside = e.m_rect.contains(point);
        if (!e.clipsChildren() || inside)
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
    ++m_forgotten;
    if (m_down == e) m_down = nullptr;
    if (m_dragging == e) m_dragging = nullptr;
    if (m_hover == e) m_hover = nullptr;
    for (Canvas* c : m_canvases)
        if (c->m_focus == e) c->m_focus = nullptr;
    if (m_editing == e) {
        m_editing = nullptr;
        m_editingCanvas = nullptr;
    }
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
    const uint64_t frame = Engine::get().renderer().frameNumber();
    if (auto it = m_textures.find(texture.get()); it != m_textures.end()) {
        it->second.lastUsed = frame;
        return it->second.id;
    }
    const VkSampler sampler = Engine::get().renderer().samplers().get(SamplerType::LinearClamp);
    VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(sampler, texture->image().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_textures[texture.get()] = {texture, reinterpret_cast<void*>(set), frame};
    return reinterpret_cast<void*>(set);
}

void System::evictTextures()
{
    // A cached descriptor set keeps its texture alive. Once only the cache holds
    // the texture and no frame drew it for a while, drop both. During frame N's
    // GUI, frames up to N - kFramesInFlight - 1 have finished on the GPU (the
    // renderer waited on their fences, see Device::collectGarbage), so a set
    // last used by one of those can be freed right away.
    const uint64_t frame = Engine::get().renderer().frameNumber();
    for (auto it = m_textures.begin(); it != m_textures.end();) {
        const TextureEntry& e = it->second;
        if (e.texture.use_count() == 1 && frame > e.lastUsed + Device::kFramesInFlight) {
            ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(e.id));
            it = m_textures.erase(it); // (the Texture defers its own GPU destruction)
        } else {
            ++it;
        }
    }
}

void System::releaseTextures()
{
    for (auto& [key, e] : m_textures) ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(e.id));
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
    // Handlers may rebuild the UI (an inventory refreshing its slots): they run
    // on a copy of the std::function, and when any element was destroyed
    // meanwhile, the element pointers held here are dropped until next frame.
    const auto invoke = [this](const auto& fn, auto&&... args) {
        const std::remove_cvref_t<decltype(fn)> copy = fn;
        const uint64_t before = m_forgotten;
        copy(std::forward<decltype(args)>(args)...);
        return m_forgotten != before; // true: something went away
    };
    if (hover != m_hover) {
        if (m_hover) {
            m_hover->m_hovered = false;
            if (m_hover->onHover && invoke(m_hover->onHover, false)) {
                hover = nullptr;
                hoverCanvas = nullptr;
            }
        }
        m_hover = hover;
        m_hoverTime = 0.0f;
        if (m_hover) {
            m_hover->m_hovered = true;
            if (m_hover->onHover && invoke(m_hover->onHover, true) && !m_hover) {
                hover = nullptr;
                hoverCanvas = nullptr;
            }
        }
    }
    m_hoverTime += dt;

    // A click anywhere else ends text editing (a click on another field moves it there).
    if (pointer && in.mousePressed(MouseButton::Left) && m_editing && hover != m_editing && m_editingCanvas) m_editingCanvas->focus(nullptr);
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
                    if (invoke(t->onDrop, *dragged)) hover = nullptr;
                    break;
                }
        } else if (down == hover && down->enabled && down->onClick) {
            if (invoke(down->onClick)) hover = nullptr;
        }
    }

    // --- Wheel: the scroll view under the pointer ---
    if (pointer && in.wheel() != 0.0f)
        for (Element* e = hover; e; e = e->m_parent)
            if (auto* sv = dynamic_cast<ScrollView*>(e); sv && sv->overflows()) {
                sv->scrollBy(-in.wheel() * sv->wheelStep);
                break;
            }

    // --- Text editing: the focused text field of the topmost canvas that has one ---
    m_editing = nullptr;
    m_editingCanvas = nullptr;
    for (Canvas* c : shown)
        if (c->m_focus && c->m_focus->wantsText() && c->m_focus->visible && c->m_focus->enabled) {
            // (wantsText() is virtual: a custom element may claim it without being a TextInput)
            if (auto* field = dynamic_cast<TextInput*>(c->m_focus)) {
                m_editing = field;
                m_editingCanvas = c;
                break;
            }
        }
    const bool typing = m_editing != nullptr;
    if (typing) {
        m_editing->edit(in, dt); // (its handlers may destroy it: forget() clears m_editing*)
        if (m_editingCanvas && in.keyPressed(Key::Escape)) { // stop editing (not "back")
            m_editingCanvas->focus(nullptr);
            m_editing = nullptr;
        }
    }
    in.setTextInput(m_editing != nullptr);

    // --- Focus (keyboard / gamepad) on the top modal canvas ---
    if (modal) {
        auto key = [&](Key k) { return in.keyPressed(k); };
        auto pad = [&](GamepadButton b) { return in.gamepadPressed(b); };
        glm::vec2 dir(0.0f);
        if (key(Key::Up) || pad(GamepadButton::DPadUp)) dir = {0, -1};
        if (key(Key::Down) || pad(GamepadButton::DPadDown)) dir = {0, 1};
        if ((key(Key::Left) && !typing) || pad(GamepadButton::DPadLeft)) dir = {-1, 0}; // (typing: the caret moves)
        if ((key(Key::Right) && !typing) || pad(GamepadButton::DPadRight)) dir = {1, 0};
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
            if (best) {
                modal->focus(best);
                for (Element* e = best->m_parent; e; e = e->m_parent) // inside a scroll view: show it
                    if (auto* sv = dynamic_cast<ScrollView*>(e)) sv->scrollTo(*best);
            }
        }
        // While typing, keys are text: no clicks on Space, no Escape = back, no letter shortcuts.
        if (!typing) {
            if (modal->m_focus && (key(Key::Enter) || key(Key::Space) || pad(GamepadButton::A))) modal->m_focus->click();
            if (modal->onBack && (key(Key::Escape) || pad(GamepadButton::B))) invoke(modal->onBack);
            for (const Canvas::Shortcut& sc : std::vector<Canvas::Shortcut>(modal->shortcuts)) // (an action may change the list)
                if (sc.action && ((sc.key != Key::Unknown && key(sc.key)) || (sc.pad != GamepadButton::Count && pad(sc.pad)))) {
                    sc.action();
                    break;
                }
        } else if (pad(GamepadButton::B)) {
            modal->focus(nullptr); // (pad: leave the field)
        }
    }

    m_block = modal != nullptr;
    m_wantsMouse = overUi || m_down != nullptr;
    in.setUiCapture(imguiKeyboard || m_block || typing, imguiMouse || m_wantsMouse || m_block, m_block);
}

void System::draw(Engine& engine)
{
    evictTextures();
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
