#include "vfx/PhotoMode.hpp"

#include "vfx/Atmosphere.hpp"
#include "vfx/DayNight.hpp"
#include "vfx/DepthOfField.hpp"
#include "vfx/ScreenSpaceGI.hpp"
#include "vfx/ScreenSpaceReflections.hpp"
#include "vfx/VolumetricFog.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Log.hpp>
#include <ve/core/Time.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/passes/BuiltinPasses.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace vfx {

namespace {

int g_activeCount = 0;

const char* kResolutions[] = {"Window", "Window x2", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
const int kSampleChoices[] = {16, 32, 64, 128, 256};

DayNightCycle* findDayNight(ve::Scene& scene)
{
    for (const auto& o : scene.objects())
        if (auto* d = dynamic_cast<DayNightCycle*>(o.get())) return d;
    return nullptr;
}

std::string timestamp()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

} // namespace

PhotoMode::PhotoMode() : GameObject("Photo mode")
{
    runInEditor = true; // keeps updating while the game is paused (by us)
}

bool PhotoMode::anyActive()
{
    return g_activeCount > 0;
}

void PhotoMode::enter()
{
    if (m_active) return;
    ve::Engine& e = engine();
    m_active = true;
    ++g_activeCount;
    m_savedPaused = e.gamePaused;
    m_savedTimeScale = e.time().timeScale;
    m_savedMouseCaptured = input().mouseCaptured();
    e.gamePaused = true;
    e.time().timeScale = 0.0f; // freezes shader time too (wind, water, cloud drift)
    input().setMouseCaptured(false);

    // Start from the current view.
    glm::vec3 pos(0.0f, 2.0f, 0.0f);
    glm::quat rot(1, 0, 0, 0);
    if (ve::Camera* cam = scene().mainCamera()) {
        pos = cam->transform().position();
        rot = cam->transform().rotation();
        m_fov = cam->fovDegrees;
    }
    const glm::vec3 f = rot * glm::vec3(0, 0, -1);
    m_yaw = glm::degrees(std::atan2(-f.x, -f.z));
    m_pitch = glm::degrees(std::asin(std::clamp(f.y, -1.0f, 1.0f)));
    m_camera = &scene().spawn("Photo camera");
    m_camera->runInEditor = true;
    m_camera->hideInHierarchy = true;
    auto& c = m_camera->addComponent<ve::Camera>();
    c.priority = 1 << 20;
    c.fovDegrees = m_fov;
    m_camera->transform().setPosition(pos);
    m_camera->transform().setRotation(rot);

    ve::RenderGraph& g = renderer().graph();
    auto* dof = g.findPass<DepthOfFieldPass>();
    m_addedDof = dof == nullptr;
    if (!dof) {
        dof = &g.addPass<DepthOfFieldPass>();
        dof->enabled = false;
    }
    m_savedDofEnabled = dof->enabled;
    if (auto* comp = g.findPass<ve::CompositePass>()) m_savedExposureEV = m_exposureEV = comp->exposureCompensation;
    m_status.clear();
}

void PhotoMode::exit()
{
    if (!m_active || m_capturing || m_pendingFrames >= 0) return;
    ve::Engine& e = engine();
    m_active = false;
    --g_activeCount;
    e.gamePaused = m_savedPaused;
    e.time().timeScale = m_savedTimeScale;
    input().setMouseCaptured(m_savedMouseCaptured);
    if (m_camera) m_camera->destroy();
    m_camera = nullptr;
    ve::RenderGraph& g = renderer().graph();
    if (auto* dof = g.findPass<DepthOfFieldPass>()) {
        if (m_addedDof) g.removePass(dof->name());
        else dof->enabled = m_savedDofEnabled;
    }
    if (auto* comp = g.findPass<ve::CompositePass>()) comp->exposureCompensation = m_savedExposureEV;
}

void PhotoMode::onDestroy()
{
    if (m_qualityRaised) restoreQuality();
    m_capturing = false;
    m_pendingFrames = -1;
    exit();
}

ve::Renderer::PhotoCaptureSettings PhotoMode::captureSettings() const
{
    ve::Renderer::PhotoCaptureSettings s;
    const VkExtent2D win = const_cast<PhotoMode*>(this)->renderer().renderExtent();
    switch (resolution) {
    case 0: s.width = win.width, s.height = win.height; break;
    case 1: s.width = win.width * 2, s.height = win.height * 2; break;
    case 2: s.width = 1920, s.height = 1080; break;
    case 3: s.width = 2560, s.height = 1440; break;
    default: s.width = 3840, s.height = 2160; break;
    }
    s.samples = uint32_t(std::max(samples, 1));
    s.path = outputDirectory + "/photo_" + timestamp() + ".png";
    return s;
}

void PhotoMode::setView(const glm::vec3& position, float yawDegrees, float pitchDegrees)
{
    m_yaw = yawDegrees;
    m_pitch = std::clamp(pitchDegrees, -89.0f, 89.0f);
    if (m_camera) {
        m_camera->transform().setPosition(position);
        m_camera->transform().setRotation(glm::angleAxis(glm::radians(m_yaw), glm::vec3(0, 1, 0)) *
                                          glm::angleAxis(glm::radians(m_pitch), glm::vec3(1, 0, 0)));
    }
}

void PhotoMode::takePhoto()
{
    if (!m_active || m_capturing || m_pendingFrames >= 0) return;
    // Two frames: one shows "Rendering..." (the window freezes on it), then capture.
    m_pendingFrames = 2;
    m_status = "Rendering...";
}

void PhotoMode::applyMaxQuality()
{
    ve::Renderer& r = renderer();
    ve::RendererSettings& rs = r.settings();
    m_saved.shadowResolution = rs.directionalShadowResolution;
    m_saved.shadowDistance = rs.maxShadowDistance;
    m_saved.lodBias = rs.lodBias;
    m_saved.shadowLodBias = rs.shadowLodBias;
    rs.directionalShadowResolution = std::max(rs.directionalShadowResolution, 4096u);
    rs.maxShadowDistance = std::max(rs.maxShadowDistance, 350.0f);
    rs.lodBias = std::max(rs.lodBias, 4.0f);
    rs.shadowLodBias = 0;
    r.applySettings();

    ve::RenderGraph& g = r.graph();
    if (auto* a = g.findPass<AtmospherePass>()) {
        m_saved.cloudSteps = a->cloudSteps;
        m_saved.cloudLightSteps = a->cloudLightSteps;
        a->cloudSteps = std::max(a->cloudSteps, 128);
        a->cloudLightSteps = std::max(a->cloudLightSteps, 12);
    }
    if (auto* f = g.findPass<VolumetricFogPass>()) {
        m_saved.fogSteps = f->steps;
        f->steps = std::max(f->steps, 96);
    }
    if (auto* s = g.findPass<ScreenSpaceReflectionsPass>()) {
        m_saved.ssrSteps = s->steps;
        s->steps = std::max(s->steps, 96);
    }
    if (auto* s = g.findPass<ScreenSpaceGIPass>()) {
        m_saved.ssgiSteps = s->steps;
        m_saved.ssgiRays = s->rays;
        s->steps = std::max(s->steps, 24);
        s->rays = std::max(s->rays, 8);
    }
    if (auto* d = g.findPass<DepthOfFieldPass>()) {
        m_saved.dofTaps = d->taps;
        d->taps = std::max(d->taps, 64);
    }
    m_qualityRaised = true;
}

void PhotoMode::restoreQuality()
{
    ve::Renderer& r = renderer();
    ve::RendererSettings& rs = r.settings();
    rs.directionalShadowResolution = m_saved.shadowResolution;
    rs.maxShadowDistance = m_saved.shadowDistance;
    rs.lodBias = m_saved.lodBias;
    rs.shadowLodBias = m_saved.shadowLodBias;
    r.applySettings();
    ve::RenderGraph& g = r.graph();
    if (auto* a = g.findPass<AtmospherePass>()) {
        a->cloudSteps = m_saved.cloudSteps;
        a->cloudLightSteps = m_saved.cloudLightSteps;
    }
    if (auto* f = g.findPass<VolumetricFogPass>()) f->steps = m_saved.fogSteps;
    if (auto* s = g.findPass<ScreenSpaceReflectionsPass>()) s->steps = m_saved.ssrSteps;
    if (auto* s = g.findPass<ScreenSpaceGIPass>()) {
        s->steps = m_saved.ssgiSteps;
        s->rays = m_saved.ssgiRays;
    }
    if (auto* d = g.findPass<DepthOfFieldPass>()) d->taps = m_saved.dofTaps;
    m_qualityRaised = false;
}

void PhotoMode::update(float)
{
    ve::Input& in = input();
    if (!m_active) {
        if (in.keyPressed(toggleKey)) enter();
        return;
    }

    // ---- Capture sequencing ----
    ve::Renderer& r = renderer();
    if (m_pendingFrames >= 0 && --m_pendingFrames < 0) {
        if (maxQuality) applyMaxQuality();
        const auto s = captureSettings();
        // DOF blur is in pixels: keep the look at a higher resolution.
        if (auto* dof = r.graph().findPass<DepthOfFieldPass>()) {
            m_savedDofMaxBlur = dof->maxBlur;
            dof->maxBlur *= float(s.height) / float(std::max(1u, r.renderExtent().height));
        }
        r.beginPhotoCapture(s);
        m_capturing = true;
    }
    if (m_capturing && !r.photoCaptureActive()) {
        m_capturing = false;
        if (m_qualityRaised) restoreQuality();
        if (auto* dof = r.graph().findPass<DepthOfFieldPass>()) dof->maxBlur = m_savedDofMaxBlur;
        m_lastPhoto = r.lastPhotoPath();
        m_status = m_lastPhoto.empty() ? "Failed to save the photo (see the log)" : "Saved " + m_lastPhoto;
    }
    if (m_capturing || m_pendingFrames >= 0) return;

    if (in.keyPressed(toggleKey) || in.keyPressed(ve::Key::Escape)) {
        exit();
        return;
    }
    if (in.keyPressed(ve::Key::Enter)) takePhoto();

    // ---- Free camera (real time: the game clock is stopped) ----
    const float dt = time().unscaledDeltaTime;
    const bool look = in.mouseDown(ve::MouseButton::Right);
    if (look != in.mouseCaptured()) in.setMouseCaptured(look);
    if (look) {
        const glm::vec2 d = in.mouseDelta();
        m_yaw -= d.x * lookSensitivity;
        m_pitch = std::clamp(m_pitch - d.y * lookSensitivity, -89.0f, 89.0f);
    }
    const glm::quat rot = glm::angleAxis(glm::radians(m_yaw), glm::vec3(0, 1, 0)) *
                          glm::angleAxis(glm::radians(m_pitch), glm::vec3(1, 0, 0));
    glm::vec3 move(0.0f);
    if (in.keyDown(ve::Key::W)) move.z -= 1.0f;
    if (in.keyDown(ve::Key::S)) move.z += 1.0f;
    if (in.keyDown(ve::Key::A)) move.x -= 1.0f;
    if (in.keyDown(ve::Key::D)) move.x += 1.0f;
    float speed = moveSpeed;
    if (in.keyDown(ve::Key::LShift)) speed *= 4.0f;
    if (in.keyDown(ve::Key::LCtrl)) speed *= 0.25f;
    glm::vec3 delta = rot * move;
    if (in.keyDown(ve::Key::E)) delta.y += 1.0f;
    if (in.keyDown(ve::Key::Q)) delta.y -= 1.0f;
    if (m_camera) {
        if (glm::dot(delta, delta) > 0.0f)
            m_camera->transform().setPosition(m_camera->transform().position() + glm::normalize(delta) * speed * dt);
        m_camera->transform().setRotation(rot);
        if (auto* c = m_camera->getComponent<ve::Camera>()) c->fovDegrees = m_fov;
    }
}

void PhotoMode::onGui()
{
    if (!m_active) return;
    ve::Renderer& r = renderer();
    ve::RenderGraph& g = r.graph();

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Photo mode");
    if (m_capturing || m_pendingFrames >= 0) {
        ImGui::TextUnformatted("Rendering the photo...");
        ImGui::ProgressBar(r.photoCaptureProgress());
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("RMB look, WASD move, Q/E down/up, Shift/Ctrl speed");

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Field of view", &m_fov, 10.0f, 110.0f, "%.0f deg");
        ImGui::SliderFloat("Move speed", &moveSpeed, 0.5f, 50.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
        if (auto* comp = g.findPass<ve::CompositePass>()) {
            ImGui::SliderFloat("Exposure", &m_exposureEV, -3.0f, 3.0f, "%+.2f EV");
            comp->exposureCompensation = m_exposureEV;
        }
    }
    if (DayNightCycle* dn = findDayNight(scene())) {
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Time of day", &dn->timeOfDay, 0.0f, 24.0f, "%.2f h");
        }
    }
    if (auto* dof = g.findPass<DepthOfFieldPass>()) {
        if (ImGui::CollapsingHeader("Depth of field", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Enabled", &dof->enabled);
            ImGui::BeginDisabled(!dof->enabled);
            ImGui::Checkbox("Auto focus (screen centre)", &dof->autoFocus);
            ImGui::BeginDisabled(dof->autoFocus);
            ImGui::SliderFloat("Focus distance", &dof->focusDistance, 0.2f, 500.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
            if (ImGui::Button("Focus on what's in the centre") && m_camera) {
                const glm::vec3 p = m_camera->transform().position();
                const glm::vec3 f = m_camera->transform().rotation() * glm::vec3(0, 0, -1);
                if (auto hit = physics().raycast(p, f, 2000.0f)) dof->focusDistance = std::max(hit->distance, 0.2f);
            }
            ImGui::EndDisabled();
            ImGui::SliderFloat("Aperture", &dof->aperture, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Sharp range", &dof->focusRange, 0.0f, 50.0f, "%.2f m");
            ImGui::SliderFloat("Max blur", &dof->maxBlur, 1.0f, 32.0f, "%.0f px");
            ImGui::SliderFloat("Foreground blur", &dof->nearBlur, 0.0f, 2.0f);
            ImGui::EndDisabled();
        }
    }
    if (ImGui::CollapsingHeader("Capture", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Combo("Resolution", &resolution, kResolutions, IM_ARRAYSIZE(kResolutions));
        const auto s = captureSettings();
        ImGui::TextDisabled("%u x %u", s.width, s.height);
        int sampleIndex = 0;
        for (int i = 0; i < IM_ARRAYSIZE(kSampleChoices); ++i)
            if (kSampleChoices[i] <= samples) sampleIndex = i;
        const char* sampleNames[] = {"16", "32", "64", "128", "256"};
        if (ImGui::Combo("Samples", &sampleIndex, sampleNames, IM_ARRAYSIZE(sampleNames))) samples = kSampleChoices[sampleIndex];
        ImGui::Checkbox("Max quality effects", &maxQuality);
        if (ImGui::Button("Take photo (Enter)", ImVec2(-1, 0))) takePhoto();
        if (!m_status.empty()) ImGui::TextWrapped("%s", m_status.c_str());
    }
    if (ImGui::Button("Leave photo mode (F12)", ImVec2(-1, 0))) exit();
    ImGui::End();
}

} // namespace vfx
