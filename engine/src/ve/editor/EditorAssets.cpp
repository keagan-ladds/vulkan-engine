// Editor: the terrain's biome list (inspector) and the asset editor, a window
// that edits one data file (.biome, .foliage, .grass) live. Edits go to the
// shared DataAssets copy at once (the terrain, foliage and grass rebuild from
// it within a quarter second); Save writes the file. See Editor.hpp.

#include "ve/assets/DataAsset.hpp"
#include "ve/core/Engine.hpp"
#include "ve/editor/Editor.hpp"
#include "ve/editor/EditorInternal.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/world/Terrain.hpp"
#include "ve/world/Vegetation.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace ve {

using namespace editor_internal;

namespace {

enum class AssetKind { None, Biome, Foliage, Grass };

AssetKind kindOf(const std::string& path)
{
    const std::string ext = std::filesystem::path(path).extension().string();
    if (ext == ".biome") return AssetKind::Biome;
    if (ext == ".foliage") return AssetKind::Foliage;
    if (ext == ".grass") return AssetKind::Grass;
    return AssetKind::None;
}

std::string stem(const std::string& path)
{
    return std::filesystem::path(path).stem().string();
}

std::string slug(const std::string& name)
{
    std::string s;
    for (char c : name) s += std::isalnum(static_cast<unsigned char>(c)) ? char(std::tolower(static_cast<unsigned char>(c))) : '_';
    return s.empty() ? "biome" : s;
}

// A new file's content: the type's defaults.
Json defaultContent(AssetKind kind, const std::string& name)
{
    Json j;
    switch (kind) {
    case AssetKind::Biome: {
        TerrainBiome b;
        b.name = name;
        j = biomeToJson(b);
        break;
    }
    case AssetKind::Foliage: {
        FoliageType t;
        j = BiomeVegetation::foliageJson(t);
        break;
    }
    case AssetKind::Grass: {
        GrassLayer g;
        j = BiomeVegetation::grassJson(g);
        break;
    }
    case AssetKind::None: break;
    }
    j["version"] = 1;
    return j;
}

const char* plantExtension(bool grass) { return grass ? ".grass" : ".foliage"; }

ImVec4 toImColor(const glm::vec3& c) { return ImVec4(c.r, c.g, c.b, 1.0f); }

} // namespace

// -----------------------------------------------------------------------------
// The terrain's biome list (inspector, below the terrain tools)
// -----------------------------------------------------------------------------

void Editor::drawTerrainBiomes(Terrain& terrain)
{
    if (!ImGui::CollapsingHeader("Biomes", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID("biomes");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The first biome fills whatever the others leave; later ones paint over earlier ones. "
                        "Biomes from files are shared between scenes: Edit changes the file (live).");
    ImGui::PopTextWrapPos();

    const uint64_t terrainId = terrain.id();
    int remove = -1, swapWithPrevious = -1;
    std::string openNext;
    bool anyPlants = false;
    for (size_t i = 0; i < terrain.biomes.size(); ++i) {
        TerrainBiome& b = terrain.biomes[i];
        anyPlants |= !b.foliage.empty() || !b.grass.empty();
        ImGui::PushID(int(i));
        ImGui::ColorButton("##color", toImColor(b.color), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(14, 14));
        ImGui::SameLine();
        ImGui::Text("%zu  %s", i, b.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", b.asset.empty() ? "(in scene)" : b.asset.c_str());

        if (!b.asset.empty()) {
            if (ImGui::SmallButton("Edit")) openNext = b.asset;
        } else if (ImGui::SmallButton("Save to file...")) {
            const size_t index = i;
            newAssetDialog("biomes/" + slug(b.name) + ".biome", biomeToJson(b), [this, terrainId, index](const std::string& path) {
                auto* t = dynamic_cast<Terrain*>(m_engine->scene().findById(terrainId));
                if (!t || index >= t->biomes.size()) return;
                beginEdit(*t);
                t->biomes[index] = loadBiomeAsset(path);
                endEdit("Move biome to " + path);
            });
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) swapWithPrevious = int(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == terrain.biomes.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) swapWithPrevious = int(i) + 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(terrain.biomes.size() <= 1);
        if (ImGui::SmallButton("Remove")) remove = int(i);
        ImGui::EndDisabled();

        if (!b.foliage.empty() || !b.grass.empty()) {
            std::string grows;
            for (const auto* list : {&b.foliage, &b.grass})
                for (const BiomePlant& p : *list) grows += (grows.empty() ? "" : ", ") + stem(p.type);
            ImGui::TextDisabled("   grows: %s", grows.c_str());
        }
        // In-scene biomes are edited right here (with undo).
        if (b.asset.empty() && ImGui::TreeNode("Settings")) {
            if (const TypeInfo* t = TypeRegistry::get().find("TerrainBiome"))
                for (const FieldInfo& f : t->fields) drawField(f, &b, &terrain);
            if (ImGui::TreeNode("Ground material")) {
                if (const TypeInfo* t = TypeRegistry::get().find("TerrainMaterial"))
                    for (const FieldInfo& f : t->fields) drawField(f, &b.material, &terrain);
                ImGui::TreePop();
            }
            drawPlantList("Foliage", b.foliage, false, openNext, &terrain);
            drawPlantList("Grass", b.grass, true, openNext, &terrain);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (swapWithPrevious > 0) {
        beginEdit(terrain);
        std::swap(terrain.biomes[size_t(swapWithPrevious) - 1], terrain.biomes[size_t(swapWithPrevious)]);
        endEdit("Reorder biomes");
    }
    if (remove >= 0) {
        beginEdit(terrain);
        terrain.biomes.erase(terrain.biomes.begin() + remove);
        endEdit("Remove biome");
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-130.0f);
    if (ImGui::BeginCombo("##addbiome", "Add a biome file...")) {
        const auto files = listAssets(".biome");
        if (files.empty()) ImGui::TextDisabled("No .biome files in assets/");
        for (const auto& path : files) {
            if (!ImGui::Selectable(path.c_str())) continue;
            beginEdit(terrain);
            terrain.biomes.push_back(loadBiomeAsset(path));
            endEdit("Add biome " + stem(path));
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New biome file")) {
        newAssetDialog("biomes/new_biome.biome", defaultContent(AssetKind::Biome, "new_biome"),
                       [this, terrainId](const std::string& path) {
                           if (auto* t = dynamic_cast<Terrain*>(m_engine->scene().findById(terrainId))) {
                               beginEdit(*t);
                               t->biomes.push_back(loadBiomeAsset(path));
                               endEdit("Add biome " + stem(path));
                           }
                           openAsset(path);
                       });
    }

    if (anyPlants && !m_engine->scene().findFirst<BiomeVegetation>()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "The biomes list plants, but nothing grows them yet:");
        if (ImGui::Button("Add BiomeVegetation", ImVec2(-1.0f, 0.0f)))
            createObject({{"type", "BiomeVegetation"}, {"name", "Biome vegetation"}}, nullptr, "Create Biome vegetation");
    }
    ImGui::PopID();
    if (!openNext.empty()) openAsset(openNext);
}

bool Editor::drawPlantList(const char* label, std::vector<BiomePlant>& list, bool grass, std::string& openNext, GameObject* owner)
{
    ImGui::PushID(label);
    ImGui::SeparatorText(label);
    bool changed = false;
    // One-frame edits (combo, buttons) on an in-scene biome: one undo step each.
    auto oneShot = [&](const std::string& what) {
        changed = true;
        if (!owner) return;
        m_editing = true; // the selection's "before" state is cached (refreshEditBefore)
        endEdit(what);
    };
    int remove = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        BiomePlant& p = list[i];
        ImGui::PushID(int(i));
        ImGui::SetNextItemWidth(-175.0f);
        if (ImGui::BeginCombo("##type", p.type.empty() ? "(choose a file)" : p.type.c_str())) {
            for (const auto& path : listAssets(plantExtension(grass))) {
                if (ImGui::Selectable(path.c_str(), path == p.type)) {
                    p.type = path;
                    oneShot(std::string("Set ") + label);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(55.0f);
        if (ImGui::DragFloat("##density", &p.density, 0.01f, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
        if (owner && ImGui::IsItemActivated()) beginEdit(*owner);
        if (owner && ImGui::IsItemDeactivated()) endEdit("Plant density");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("density in this biome (0..1)");
        ImGui::SameLine();
        ImGui::BeginDisabled(p.type.empty());
        if (ImGui::SmallButton("Edit")) openNext = p.type;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) remove = int(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        list.erase(list.begin() + remove);
        oneShot(std::string("Remove ") + label);
    }
    if (ImGui::SmallButton("+ Add")) {
        list.push_back(BiomePlant{});
        oneShot(std::string("Add ") + label);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(grass ? "New .grass file" : "New .foliage file")) {
        const AssetKind kind = grass ? AssetKind::Grass : AssetKind::Foliage;
        const std::string dir = grass ? "grass/" : "foliage/";
        // The list may belong to the open biome file or to an in-scene biome:
        // find it again by what it is when the file has been created.
        const std::string biomePath = owner ? std::string() : m_assetPath;
        auto* listPtr = &list;
        const uint64_t ownerId = owner ? owner->id() : 0;
        newAssetDialog(dir + "new" + plantExtension(grass), defaultContent(kind, "new"),
                       [this, biomePath, grass, listPtr, ownerId](const std::string& path) {
                           if (!biomePath.empty()) {
                               if (m_assetPath != biomePath) return;
                               (grass ? m_assetBiome.grass : m_assetBiome.foliage).push_back({path, 1.0f});
                               commitAssetCopy();
                           } else if (auto* t = dynamic_cast<Terrain*>(m_engine->scene().findById(ownerId))) {
                               // (still the same biome list unless the biomes were reordered meanwhile)
                               for (TerrainBiome& b : t->biomes) {
                                   auto& l = grass ? b.grass : b.foliage;
                                   if (&l != listPtr) continue;
                                   beginEdit(*t);
                                   l.push_back({path, 1.0f});
                                   endEdit("Add plant " + stem(path));
                               }
                           }
                           openAsset(path, true);
                       });
    }
    ImGui::PopID();
    return changed;
}

// -----------------------------------------------------------------------------
// Assets menu
// -----------------------------------------------------------------------------

void Editor::drawAssetsMenu()
{
    if (!ImGui::BeginMenu("Assets")) return;
    struct Kind {
        const char* label;
        AssetKind kind;
        const char* ext;
        const char* dir;
    };
    for (const Kind& k : {Kind{"Biomes", AssetKind::Biome, ".biome", "biomes/"}, Kind{"Foliage", AssetKind::Foliage, ".foliage", "foliage/"},
                          Kind{"Grass", AssetKind::Grass, ".grass", "grass/"}}) {
        if (!ImGui::BeginMenu(k.label)) continue;
        if (ImGui::MenuItem("New...")) {
            const std::string path = std::string(k.dir) + "new" + k.ext;
            newAssetDialog(path, defaultContent(k.kind, "new"), [this](const std::string& p) { openAsset(p); });
        }
        ImGui::Separator();
        const auto files = listAssets(k.ext);
        if (files.empty()) ImGui::TextDisabled("No %s files in assets/", k.ext);
        for (const auto& f : files)
            if (ImGui::MenuItem(f.c_str(), nullptr, f == m_assetPath)) openAsset(f);
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// -----------------------------------------------------------------------------
// Asset editor window
// -----------------------------------------------------------------------------

void Editor::openAsset(const std::string& path, bool keepHistory)
{
    if (path == m_assetPath) return;
    if (keepHistory && !m_assetPath.empty()) m_assetHistory.push_back(m_assetPath);
    else if (!keepHistory) m_assetHistory.clear();
    m_assetPath = path;
    loadAssetCopy();
}

void Editor::loadAssetCopy()
{
    const Json& j = *DataAssets::get(m_assetPath);
    switch (kindOf(m_assetPath)) {
    case AssetKind::Biome:
        m_assetBiome = biomeFromJson(j, stem(m_assetPath));
        m_assetBiome.asset = m_assetPath;
        break;
    case AssetKind::Foliage:
        if (!m_assetFoliage) m_assetFoliage = std::make_unique<FoliageType>();
        BiomeVegetation::applyFoliageJson(*m_assetFoliage, j);
        break;
    case AssetKind::Grass:
        if (!m_assetGrass) m_assetGrass = std::make_unique<GrassLayer>();
        BiomeVegetation::applyGrassJson(*m_assetGrass, j);
        break;
    case AssetKind::None: break;
    }
    m_assetSeen = DataAssets::generation();
}

void Editor::commitAssetCopy()
{
    Json j;
    switch (kindOf(m_assetPath)) {
    case AssetKind::Biome: j = biomeToJson(m_assetBiome); break;
    case AssetKind::Foliage: j = BiomeVegetation::foliageJson(*m_assetFoliage); break;
    case AssetKind::Grass: j = BiomeVegetation::grassJson(*m_assetGrass); break;
    case AssetKind::None: return;
    }
    j["version"] = 1;
    DataAssets::set(m_assetPath, std::move(j));
    m_assetSeen = DataAssets::generation();
}

void Editor::drawAssetEditor()
{
    if (m_assetPath.empty()) return;
    const AssetKind kind = kindOf(m_assetPath);
    // Changed elsewhere (file reloaded from disk, reverted, another window): re-read,
    // unless a field is being dragged right now.
    if (DataAssets::generation() != m_assetSeen && !ImGui::IsAnyItemActive()) loadAssetCopy();

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kInspectorWidth - 430.0f, kMenuHeight + 30.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 620.0f), ImGuiCond_FirstUseEver);
    const bool dirty = DataAssets::dirty(m_assetPath);
    const std::string title = m_assetPath + (dirty ? " *" : "") + "###assetEditor";
    bool open = true;
    if (!ImGui::Begin(title.c_str(), &open)) {
        ImGui::End();
        if (!open) m_assetPath.clear();
        return;
    }
    std::string openNext;
    if (!m_assetHistory.empty()) {
        if (ImGui::Button("< Back")) openNext = "<back>";
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Save")) {
        const bool ok = DataAssets::save(m_assetPath);
        m_status = ok ? "Saved " + m_assetPath : "Can't write " + m_assetPath;
        m_statusTime = 3.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) DataAssets::revert(m_assetPath);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(dirty ? "unsaved (live in the scene)" : "saved");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Writes to %s", DataAssets::writablePath(m_assetPath).string().c_str());
    ImGui::Separator();

    ImGui::PushItemWidth(-150.0f);
    bool changed = false;
    auto fieldsOf = [&](const char* typeName, void* object) {
        const TypeInfo* t = TypeRegistry::get().find(typeName);
        if (!t) return;
        for (const FieldInfo& f : t->fields) {
            if (f.name == "biomes") continue; // (from the biomes that list it)
            changed |= drawField(f, object, nullptr);
        }
    };
    switch (kind) {
    case AssetKind::Biome:
        if (ImGui::CollapsingHeader("Look and placement", ImGuiTreeNodeFlags_DefaultOpen)) fieldsOf("TerrainBiome", &m_assetBiome);
        if (ImGui::CollapsingHeader("Ground material (optional)")) fieldsOf("TerrainMaterial", &m_assetBiome.material);
        if (ImGui::CollapsingHeader("What grows here", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Density 0..1 per file; a BiomeVegetation object in the scene grows them. A file "
                                "listed by several biomes is one layer (its density blends across their borders).");
            ImGui::PopTextWrapPos();
            changed |= drawPlantList("Foliage", m_assetBiome.foliage, false, openNext, nullptr);
            changed |= drawPlantList("Grass", m_assetBiome.grass, true, openNext, nullptr);
        }
        break;
    case AssetKind::Foliage:
        ImGui::TextDisabled("Layer name (for 'avoid'): %s", stem(m_assetPath).c_str());
        fieldsOf("FoliageType", m_assetFoliage.get());
        break;
    case AssetKind::Grass:
        fieldsOf("GrassLayer", m_assetGrass.get());
        break;
    case AssetKind::None:
        ImGui::TextDisabled("Not an editable asset type.");
        break;
    }
    ImGui::PopItemWidth();
    ImGui::End();

    if (changed) commitAssetCopy();
    if (!open) {
        m_assetPath.clear();
        m_assetHistory.clear();
    } else if (openNext == "<back>") {
        const std::string back = m_assetHistory.back();
        m_assetHistory.pop_back();
        m_assetPath = back;
        loadAssetCopy();
    } else if (!openNext.empty()) {
        openAsset(openNext, true);
    }
}

// -----------------------------------------------------------------------------
// New asset file popup
// -----------------------------------------------------------------------------

void Editor::newAssetDialog(const std::string& path, Json content, std::function<void(const std::string&)> onCreated)
{
    std::snprintf(m_newAssetBuffer, sizeof(m_newAssetBuffer), "%s", path.c_str());
    m_newAssetExt = std::filesystem::path(path).extension().string();
    m_newAssetContent = std::move(content);
    m_newAssetDone = std::move(onCreated);
    m_openNewAsset = true;
}

void Editor::drawNewAssetPopup()
{
    if (m_openNewAsset) {
        ImGui::OpenPopup("New asset file");
        m_openNewAsset = false;
    }
    if (!ImGui::BeginPopupModal("New asset file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Path (relative to the assets folder):");
    ImGui::SetNextItemWidth(360.0f);
    const bool enter = ImGui::InputText("##newasset", m_newAssetBuffer, sizeof(m_newAssetBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string path = m_newAssetBuffer;
    const bool badExt = std::filesystem::path(path).extension().string() != m_newAssetExt;
    const bool exists = !badExt && DataAssets::exists(path);
    if (badExt) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "The file must end in %s", m_newAssetExt.c_str());
    if (exists) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "That file already exists");
    ImGui::BeginDisabled(badExt || exists);
    if (ImGui::Button("Create") || (enter && !badExt && !exists)) {
        Json content = m_newAssetContent;
        if (kindOf(path) == AssetKind::Biome && content.is_object()) content["name"] = stem(path);
        if (DataAssets::create(path, content)) {
            m_assetCacheAge = 1e9f; // list it
            m_status = "Created " + path;
            if (m_newAssetDone) m_newAssetDone(path);
        } else {
            m_status = "Can't write " + path;
        }
        m_statusTime = 3.0f;
        m_newAssetDone = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_newAssetDone = nullptr;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace ve
