#pragma once

// =============================================================================
// Reflection - runtime descriptions of GameObject / Component types and their
// fields, used by scene files (SceneSerializer) and the editor's inspector.
//
//   // In any game .cpp (the executable's object files are always linked):
//   VE_REGISTER_TYPE(Torch, "Torch")
//   {
//       t.category("Savanna");
//       t.field("flameHeight", &Torch::flameHeight).range(0.2f, 3.0f);
//       t.field("color", &Torch::color).color();
//       t.enumField("mode", &Torch::mode, {"Off", "Flicker", "Steady"});
//       t.onChanged([](Torch& torch) { torch.rebuild(); });   // after an edit or load
//   }
//
// Supported field types: bool, int, uint32_t, float, glm::vec2/3/4, std::string,
// enums (enumField), std::shared_ptr<AudioClip> (stored as its path). Anything
// else: t.property(name, kind, getter, setter) with JSON get/set functions.
//
// Types must be default constructible to be created by the editor or a scene
// file (otherwise they're inspect-only). Engine types are registered by
// ve::registerBuiltinTypes() (called by the Engine).
// =============================================================================

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ve {

class AudioClip;
class Component;
class GameObject;
class Scene;

using Json = nlohmann::json;

// Asset: a path string (assetFilter lists extensions). Json: anything else, edited as JSON text.
enum class FieldKind { Bool, Int, Float, Vec2, Vec3, Vec4, Color3, Color4, String, Enum, Asset, Json };

struct FieldInfo {
    std::string name;
    FieldKind kind = FieldKind::Float;
    std::function<Json(const void*)> get;        // object -> JSON value
    std::function<void(void*, const Json&)> set; // JSON value -> object
    std::function<void*(void*)> address;         // direct pointer for POD kinds (inspector), may be null
    float min = 0.0f, max = 0.0f;                // range; min == max: unbounded
    float speed = 0.05f;                         // drag speed in the inspector
    std::vector<std::string> enumNames;
    std::string assetFilter;                     // Asset: extensions, e.g. ".wav .ogg"
    std::string tooltip;
    bool readOnly = false;
    bool hidden = false; // saved and loaded, not shown in the inspector (e.g. bulk data)
};

struct TypeInfo {
    std::string name;
    std::string category = "General";
    std::type_index type = typeid(void);
    bool isComponent = false;
    bool creatable = false; // default constructible
    std::vector<FieldInfo> fields;
    std::function<Component*(GameObject&)> addTo;    // components
    std::function<GameObject*(Scene&)> spawn;         // GameObject classes
    std::function<void(void*)> onChanged;             // after an edit / load (may be null)

    const FieldInfo* field(const std::string& n) const
    {
        for (const auto& f : fields) {
            if (f.name == n) return &f;
        }
        return nullptr;
    }
};

class TypeRegistry;

// Fluent field options returned by TypeBuilder::field().
class FieldRef {
public:
    FieldRef(TypeInfo& info, size_t index) : m_info(&info), m_index(index) {}
    FieldRef& range(float min, float max) { f().min = min; f().max = max; return *this; }
    FieldRef& speed(float s) { f().speed = s; return *this; }
    FieldRef& color() // vec3 / vec4 shown as a colour picker
    {
        if (f().kind == FieldKind::Vec3) f().kind = FieldKind::Color3;
        if (f().kind == FieldKind::Vec4) f().kind = FieldKind::Color4;
        return *this;
    }
    FieldRef& tooltip(std::string t) { f().tooltip = std::move(t); return *this; }
    FieldRef& assets(std::string extensions) { f().assetFilter = std::move(extensions); return *this; }
    FieldRef& readOnly() { f().readOnly = true; return *this; }
    FieldRef& hidden() { f().hidden = true; return *this; }

private:
    FieldInfo& f() { return m_info->fields[m_index]; }
    TypeInfo* m_info;
    size_t m_index;
};

namespace detail {
std::shared_ptr<AudioClip> loadAudioClip(const std::string& path);
std::string audioClipPath(const std::shared_ptr<AudioClip>& clip);

template <class V>
Json vecToJson(const V& v)
{
    Json a = Json::array();
    for (int i = 0; i < V::length(); ++i) a.push_back(v[i]);
    return a;
}
// Whether a JSON value can be read as a scalar field of type M.
template <class M>
bool jsonHolds(const Json& j)
{
    if constexpr (std::is_same_v<M, bool>) return j.is_boolean();
    else if constexpr (std::is_same_v<M, std::string>) return j.is_string();
    else return j.is_number();
}
template <class V>
V vecFromJson(const Json& j, const V& fallback)
{
    V v = fallback;
    if (!j.is_array()) return v;
    for (int i = 0; i < V::length() && i < static_cast<int>(j.size()); ++i) v[i] = j[i].get<float>();
    return v;
}
} // namespace detail

template <class T>
class TypeBuilder {
public:
    explicit TypeBuilder(TypeInfo& info) : m_info(info) {}

    TypeBuilder& category(std::string c)
    {
        m_info.category = std::move(c);
        return *this;
    }

    // `member` may belong to T or to a base of T (shared settings structs).
    template <class M, class C>
    FieldRef field(const char* name, M C::*member)
    {
        static_assert(std::is_base_of_v<C, T>, "field(): member of an unrelated class");
        FieldInfo fi;
        fi.name = name;
        auto addr = [member](void* o) -> void* { return &(static_cast<T*>(o)->*member); };
        auto ref = [member](const void* o) -> const M& { return static_cast<const T*>(o)->*member; };
        auto mut = [member](void* o) -> M& { return static_cast<T*>(o)->*member; };
        if constexpr (std::is_same_v<M, bool> || std::is_same_v<M, int> || std::is_same_v<M, float> ||
                      std::is_same_v<M, std::string>) {
            fi.kind = std::is_same_v<M, bool>  ? FieldKind::Bool
                    : std::is_same_v<M, int>   ? FieldKind::Int
                    : std::is_same_v<M, float> ? FieldKind::Float
                                               : FieldKind::String;
            if constexpr (std::is_same_v<M, int>) fi.speed = 0.2f;
            fi.address = addr;
            fi.get = [ref](const void* o) { return Json(ref(o)); };
            fi.set = [mut](void* o, const Json& j) { if (detail::jsonHolds<M>(j)) mut(o) = j.get<M>(); };
        } else if constexpr (std::is_same_v<M, uint32_t>) {
            // No `address`: the inspector edits it through get/set as a clamped int.
            fi.kind = FieldKind::Int;
            fi.min = 0.0f;
            fi.max = 1e9f;
            fi.speed = 0.2f;
            fi.get = [ref](const void* o) { return Json(ref(o)); };
            fi.set = [mut](void* o, const Json& j) { if (j.is_number()) mut(o) = static_cast<uint32_t>(std::max(0, j.get<int>())); };
        } else if constexpr (std::is_same_v<M, glm::vec2> || std::is_same_v<M, glm::vec3> || std::is_same_v<M, glm::vec4>) {
            fi.kind = M::length() == 2 ? FieldKind::Vec2 : M::length() == 3 ? FieldKind::Vec3 : FieldKind::Vec4;
            fi.address = addr;
            fi.get = [ref](const void* o) { return detail::vecToJson(ref(o)); };
            fi.set = [mut](void* o, const Json& j) { mut(o) = detail::vecFromJson(j, mut(o)); };
        } else if constexpr (std::is_same_v<M, std::shared_ptr<AudioClip>>) {
            fi.kind = FieldKind::Asset;
            fi.assetFilter = ".wav .ogg .mp3 .flac";
            fi.get = [ref](const void* o) { return Json(detail::audioClipPath(ref(o))); };
            fi.set = [mut](void* o, const Json& j) {
                if (j.is_string()) mut(o) = j.get<std::string>().empty() ? nullptr : detail::loadAudioClip(j.get<std::string>());
            };
        } else {
            static_assert(sizeof(M) == 0, "Unsupported field type: use enumField() or property()");
        }
        return push(std::move(fi));
    }

    template <class E, class C>
    FieldRef enumField(const char* name, E C::*member, std::vector<std::string> names)
    {
        static_assert(std::is_enum_v<E>, "enumField needs an enum member");
        static_assert(std::is_base_of_v<C, T>, "enumField(): member of an unrelated class");
        FieldInfo fi;
        fi.name = name;
        fi.kind = FieldKind::Enum;
        fi.enumNames = std::move(names);
        fi.get = [member](const void* o) { return Json(static_cast<int>(static_cast<const T*>(o)->*member)); };
        fi.set = [member](void* o, const Json& j) {
            if (j.is_number()) static_cast<T*>(o)->*member = static_cast<E>(j.get<int>());
        };
        return push(std::move(fi));
    }

    // Custom property with JSON accessors (Asset kind: a path string).
    FieldRef property(const char* name, FieldKind kind, std::function<Json(const T&)> getter,
                      std::function<void(T&, const Json&)> setter)
    {
        FieldInfo fi;
        fi.name = name;
        fi.kind = kind;
        fi.get = [getter](const void* o) { return getter(*static_cast<const T*>(o)); };
        fi.set = [setter](void* o, const Json& j) { setter(*static_cast<T*>(o), j); };
        return push(std::move(fi));
    }

    TypeBuilder& onChanged(std::function<void(T&)> fn)
    {
        m_info.onChanged = [fn](void* o) { fn(*static_cast<T*>(o)); };
        return *this;
    }

private:
    FieldRef push(FieldInfo fi)
    {
        m_info.fields.push_back(std::move(fi));
        return FieldRef(m_info, m_info.fields.size() - 1);
    }
    TypeInfo& m_info;
};

class TypeRegistry {
public:
    static TypeRegistry& get();

    template <class T>
    void add(const std::string& name, void (*build)(TypeBuilder<T>&));

    const TypeInfo* find(const std::string& name);
    const TypeInfo* find(std::type_index type);
    const TypeInfo* of(const Component& c);
    const TypeInfo* of(const GameObject& o);
    std::vector<const TypeInfo*> all(bool components);

private:
    struct Entry {
        TypeInfo info;
        std::function<void(TypeInfo&)> build;
        bool built = false;
    };
    TypeInfo* resolve(Entry& e);
    std::vector<std::unique_ptr<Entry>> m_entries;
    std::unordered_map<std::string, Entry*> m_byName;
    std::unordered_map<std::type_index, Entry*> m_byType;
};

// Engine types (Camera, lights, AudioSource, AudioZone, Rigidbody, ModelInstance, ...).
void registerBuiltinTypes();

} // namespace ve

// --- Template implementation (needs GameObject/Scene) ---------------------------
#include "ve/scene/Scene.hpp"

namespace ve {

template <class T>
void TypeRegistry::add(const std::string& name, void (*build)(TypeBuilder<T>&))
{
    auto e = std::make_unique<Entry>();
    e->info.name = name;
    e->info.type = typeid(T);
    e->info.isComponent = std::is_base_of_v<Component, T>;
    e->info.creatable = std::is_default_constructible_v<T>;
    if constexpr (std::is_default_constructible_v<T>) {
        if constexpr (std::is_base_of_v<Component, T>) {
            e->info.addTo = [](GameObject& go) -> Component* { return &go.addComponent<T>(); };
        } else {
            e->info.spawn = [](Scene& s) -> GameObject* { return &s.spawn<T>(); };
        }
    }
    e->build = [build](TypeInfo& info) {
        TypeBuilder<T> b(info);
        if (build) build(b);
    };
    m_byName[name] = e.get();
    m_byType[typeid(T)] = e.get();
    m_entries.push_back(std::move(e));
}

} // namespace ve

#define VE_REFLECT_CONCAT_(a, b) a##b
#define VE_REFLECT_CONCAT(a, b) VE_REFLECT_CONCAT_(a, b)
// Registers a type and opens the body of its build function (parameter `t`).
#define VE_REGISTER_TYPE(T, NAME)                                                                            \
    static void VE_REFLECT_CONCAT(veReflect_, __LINE__)(::ve::TypeBuilder<T>&);                              \
    [[maybe_unused]] static const bool VE_REFLECT_CONCAT(veRegistered_, __LINE__) =                                           \
        (::ve::TypeRegistry::get().add<T>(NAME, &VE_REFLECT_CONCAT(veReflect_, __LINE__)), true);            \
    static void VE_REFLECT_CONCAT(veReflect_, __LINE__)([[maybe_unused]] ::ve::TypeBuilder<T> & t)
