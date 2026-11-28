#include "ve/ui/UiLayer.hpp"

#include "ve/debug/Debug.hpp"
#include "ve/ui/Gui.hpp"

#include "ve/core/Profiler.hpp"

#include "ve/audio/Audio.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Window.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>

#include <functional>

namespace ve {

namespace {
VkFormat g_uiFormat = VK_FORMAT_UNDEFINED; // must outlive ImGui_ImplVulkan_Init

void checkVk(VkResult r)
{
    if (r != VK_SUCCESS) log::error("ImGui Vulkan error: {}", vk::resultToString(r));
}
} // namespace

void UiLayer::init(Engine& engine)
{
    m_engine = &engine;
    Renderer& r = engine.renderer();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't write imgui.ini next to the executable
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.85f;

    ImGui_ImplSDL2_InitForVulkan(engine.window().handle());

    g_uiFormat = r.swapchain().format();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = r.device().instance();
    info.PhysicalDevice = r.device().physicalDevice();
    info.Device = r.device().handle();
    info.QueueFamily = r.device().graphicsFamily();
    info.Queue = r.device().graphicsQueue();
    info.DescriptorPoolSize = 512; // let the backend create its own pool (game UI images use it too)
    info.MinImageCount = std::max(2u, r.swapchain().minImageCount());
    info.ImageCount = std::max(info.MinImageCount, r.swapchain().imageCount());
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_uiFormat;
    info.CheckVkResultFn = checkVk;
    ImGui_ImplVulkan_Init(&info);

    r.graph().addPass<UiPass>(*this);
    m_initialized = true;
}

void UiLayer::shutdown()
{
    if (!m_initialized) return;
    m_engine->renderer().device().waitIdle();
    m_engine->renderer().graph().removePass("UI");
    ui::System::get().releaseTextures();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void UiLayer::processEvent(const SDL_Event& event)
{
    if (m_initialized) ImGui_ImplSDL2_ProcessEvent(&event);
}

bool UiLayer::wantsKeyboard() const
{
    return m_initialized && ImGui::GetIO().WantCaptureKeyboard;
}

bool UiLayer::wantsMouse() const
{
    return m_initialized && ImGui::GetIO().WantCaptureMouse;
}

void UiLayer::beginFrame()
{
    ui::System::get().applyPendingFonts(); // game UI fonts: the atlas is rebuilt before the frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    m_rendered = false;
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) debugOverlay = !debugOverlay;
}

void UiLayer::endFrameIfNotRendered()
{
    if (!m_rendered) ImGui::EndFrame();
}

void UiLayer::drawDebugOverlay()
{
    const Time& time = m_engine->time();
    m_frameTimes[m_frameTimeIndex] = time.unscaledDeltaTime * 1000.0f;
    m_frameTimeIndex = (m_frameTimeIndex + 1) % 120;
    if (!debugOverlay) return;

    Renderer& r = m_engine->renderer();
    ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({380, 620}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Engine (F1)")) {
        ImGui::End();
        return;
    }

    float avg = 0.0f;
    for (float t : m_frameTimes) avg += t;
    avg /= 120.0f;
    ImGui::Text("%.1f FPS  (%.2f ms)   GPU %.2f ms", avg > 0.0f ? 1000.0f / avg : 0.0f, avg, r.gpuFrameTimeMs());
    ImGui::PlotLines("##frametimes", m_frameTimes, 120, m_frameTimeIndex, nullptr, 0.0f, 50.0f, {-1, 40});

    if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
        const RenderStats& s = r.stats();
        const FrameData& f = r.frameData();
        ImGui::Text("Draw calls: %u   Shadow draws: %u", s.drawCalls, s.shadowDrawCalls);
        ImGui::Text("Triangles: %u", s.triangles);
        ImGui::Text("Visible: %u / %u submeshes", s.visibleObjects, s.totalObjects);
        const InstanceStats& is = r.instances().stats();
        ImGui::Text("Instances: %u (%u camera / %u shadow draws), %u indirect draws", is.instances, is.visibleCamera,
                    is.visibleShadow, is.drawCalls);
        ImGui::Text("Lights: %u dir, %u point, %u shadow views   Decals: %u", s.directionalLights, s.pointLights,
                    s.shadowViews, s.decals);
        ImGui::Text("Objects: %zu   Physics bodies: %u (%.2f ms)", m_engine->scene().objectCount(),
                    m_engine->physics().bodyCount(), m_engine->physics().lastStepMs);
        ImGui::Text("Camera: %.1f %.1f %.1f", f.camera.position.x, f.camera.position.y, f.camera.position.z);
    }

    if (ImGui::CollapsingHeader("CPU zones", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Main-thread VE_PROFILE scopes: smoothed average and worst of the last second.
        if (ImGui::BeginTable("##zones", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("avg ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("max ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();
            for (const Profiler::Zone& z : Profiler::get().zones()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Indent(float(z.depth) * 12.0f + 0.001f);
                ImGui::TextUnformatted(z.name.c_str());
                ImGui::Unindent(float(z.depth) * 12.0f + 0.001f);
                ImGui::TableNextColumn();
                ImGui::Text("%6.2f", z.avgMs);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%6.2f", z.maxMs);
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Settings")) {
        RendererSettings& s = r.settings();
        if (ImGui::Checkbox("VSync", &s.vsync)) r.applySettings();
        ImGui::SliderFloat("Time scale", &m_engine->time().timeScale, 0.0f, 4.0f);
        Environment& env = r.environment();
        ImGui::ColorEdit3("Ambient sky", &env.ambientSky.x);
        ImGui::ColorEdit3("Ambient ground", &env.ambientGround.x);
        ImGui::SliderFloat("Ambient intensity", &env.ambientIntensity, 0.0f, 4.0f);
    }

    if (ImGui::CollapsingHeader("Render passes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& pass : r.graph().passes()) {
            ImGui::PushID(pass.get());
            ImGui::Checkbox("##enabled", &pass->enabled);
            ImGui::SameLine();
            const bool open = ImGui::TreeNode(pass->name().c_str());
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 70.0f);
            ImGui::TextDisabled("%6.2f ms", pass->gpuTimeMs);
            if (open) {
                pass->onGui();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Debug draw")) {
        // ve::debug channels: engine ones (colliders, navmesh, agents) and any the game asked about.
        for (const auto& [name, on] : debug::channels()) {
            bool v = on;
            if (ImGui::Checkbox(name.c_str(), &v)) debug::setChannel(name, v);
        }
        ImGui::TextDisabled("%zu lines this frame", debug::lineCount());
        if (ImGui::Button("Clear lasting lines")) debug::clear();
    }
    if (ImGui::CollapsingHeader("Audio")) {
        if (m_engine->audio().available()) m_engine->audio().onGui();
        else ImGui::TextDisabled("No audio device");
    }
    if (ImGui::CollapsingHeader("Scene hierarchy")) {
        std::function<void(GameObject&)> node = [&](GameObject& go) {
            ImGui::PushID(&go);
            bool active = go.activeSelf();
            if (ImGui::Checkbox("##a", &active)) go.setActive(active);
            ImGui::SameLine();
            const auto& children = go.transform().children();
            const ImGuiTreeNodeFlags flags = children.empty() ? ImGuiTreeNodeFlags_Leaf : 0;
            if (ImGui::TreeNodeEx(go.name().c_str(), flags)) {
                glm::vec3 p = go.transform().localPosition();
                if (ImGui::DragFloat3("Position", &p.x, 0.1f)) go.transform().setLocalPosition(p);
                for (Transform* c : children) node(c->gameObject());
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        int shown = 0;
        for (const auto& o : m_engine->scene().objects()) {
            if (o->transform().parent() || o->isDestroyed()) continue;
            if (++shown > 400) {
                ImGui::TextDisabled("...");
                break;
            }
            node(*o);
        }
    }
    for (const auto& [title, draw] : m_sections)
        if (ImGui::CollapsingHeader(title.c_str())) draw();
    ImGui::End();
}

void UiLayer::addOverlaySection(const std::string& title, std::function<void()> draw)
{
    m_sections.emplace_back(title, std::move(draw));
}

void UiPass::declare(PassBuilder& b)
{
    b.use(renderer().backbuffer(), Access::ColorAttachment);
}

void UiPass::execute(RenderContext& ctx)
{
    ImGui::Render();
    m_layer.m_rendered = true;
    ImDrawData* drawData = ImGui::GetDrawData();
    // Loading-screen frames draw nothing else: the UI starts from a cleared screen.
    const bool loading = renderer().loadingFrames();
    if (!loading && (!drawData || drawData->CmdListsCount == 0)) return;
    ctx.beginRendering({{renderer().backbuffer(), loading, renderer().loadingClearColor()}});
    if (!drawData || drawData->CmdListsCount == 0) {
        ctx.endRendering();
        return;
    }
    ImGui_ImplVulkan_RenderDrawData(drawData, ctx.cmd);
    ctx.endRendering();
}

} // namespace ve
