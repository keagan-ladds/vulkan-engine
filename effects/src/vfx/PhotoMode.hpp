#pragma once

// =============================================================================
// PhotoMode - pause the game, fly a free camera, set up depth of field and
// lighting, and render a high-quality screenshot (promotional shots).
//
//   scene.spawn<vfx::PhotoMode>();   // F12 toggles it (toggleKey)
//
// While active: the game is paused (Engine::gamePaused, time scale 0), a
// camera with top priority takes over, and a panel offers FOV, exposure, time
// of day, depth of field and capture settings.
//   Right mouse + move: look    WASD: move    Q/E: down/up
//   Shift: faster    Ctrl: slower    Enter: take photo    F12 / Esc: leave
//
// "Take photo" switches every effect to its maximum quality (shadows, LOD,
// clouds, fog, SSR/SSGI, DOF taps), then Renderer::beginPhotoCapture renders
// the frame offscreen at the chosen resolution and averages many sub-pixel
// jittered frames: anti-aliased edges and noise-free fog, clouds and GI.
// Takes a few seconds (the window doesn't update meanwhile). The PNG lands in
// outputDirectory/photo_<date>_<time>.png.
//
// Games hide their HUD while PhotoMode::anyActive() (the capture never
// includes UI either way).
// =============================================================================

#include <ve/core/Input.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>

#include <glm/glm.hpp>

#include <string>

namespace vfx {

class PhotoMode : public ve::GameObject {
public:
    PhotoMode();

    ve::Key toggleKey = ve::Key::F12;
    std::string outputDirectory = "screenshots";

    /// Capture settings (also in the panel).
    int resolution = 1;        ///< index into resolutionNames(): 0 window, 1 window x2, 2 1080p, 3 1440p, 4 4K
    int samples = 64;          ///< frames averaged
    bool maxQuality = true;    ///< raise every effect's quality for the capture
    float moveSpeed = 6.0f;    ///< m/s
    float lookSensitivity = 0.15f; ///< degrees per pixel

    bool active() const { return m_active; }
    static bool anyActive();
    void enter();
    void exit();
    void takePhoto();
    /// Places the photo camera (scripted shots). Yaw 0 looks down -Z; degrees.
    void setView(const glm::vec3& position, float yawDegrees, float pitchDegrees);
    const std::string& lastPhoto() const { return m_lastPhoto; }

    void update(float dt) override;
    void onGui() override;
    void onDestroy() override;

private:
    void applyMaxQuality();
    void restoreQuality();
    ve::Renderer::PhotoCaptureSettings captureSettings() const;

    bool m_active = false;
    ve::ObjectRef<> m_camera; ///< (the editor may delete it)
    float m_yaw = 0.0f, m_pitch = 0.0f; ///< degrees
    float m_fov = 60.0f;

    /// Restored on exit.
    bool m_savedPaused = false;
    float m_savedTimeScale = 1.0f;
    bool m_savedMouseCaptured = false;
    bool m_savedDofEnabled = false;
    bool m_addedDof = false;
    float m_savedExposureEV = 0.0f;
    float m_exposureEV = 0.0f;

    /// Capture state.
    int m_pendingFrames = -1; ///< >= 0: counting down to beginPhotoCapture (lets the panel show "Rendering...")
    bool m_capturing = false;
    bool m_qualityRaised = false;
    std::string m_lastPhoto;
    std::string m_status;
    float m_savedDofMaxBlur = 0.0f;

    struct SavedQuality {
        uint32_t shadowResolution = 0;
        float shadowDistance = 0.0f, lodBias = 0.0f;
        uint32_t shadowLodBias = 0;
        int cloudSteps = 0, cloudLightSteps = 0, fogSteps = 0, ssrSteps = 0, ssgiSteps = 0, ssgiRays = 0, dofTaps = 0;
    } m_saved;
};

} // namespace vfx
