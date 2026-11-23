#include "ve/scene/Decal.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Texture.hpp"

namespace ve {

void DecalProjector::reload()
{
    struct Slot {
        const std::string& path;
        std::shared_ptr<Texture>& texture;
        bool srgb;
    };
    Slot slots[3] = {{albedo, albedoTexture, true}, {normal, normalTexture, false}, {mask, maskTexture, false}};
    for (int i = 0; i < 3; ++i) {
        Slot& s = slots[i];
        if (s.path == m_loaded[i]) continue;
        m_loaded[i] = s.path;
        s.texture.reset(); // (a path that was cleared drops its texture)
        if (s.path.empty()) continue;
        try {
            s.texture = assets().loadTexture(s.path, s.srgb);
        } catch (const std::exception& e) {
            log::warn("DecalProjector on '{}': {}", gameObject().name(), e.what());
        }
    }
}

} // namespace ve
