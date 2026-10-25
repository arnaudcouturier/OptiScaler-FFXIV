// Source pins of the automatic ReShade depth seams (the upstream lines and the glue that cannot run offline): comments out, string
// literals kept, every run of whitespace one space. A pin that stops matching means a seam moved: re-derive it, do not loosen it.
//  - inputs/NVNGX_DLSS_Dx11.cpp: ONE call, right after the successful deviceContext->Evaluate(...) and before anything that takes
//    the feature table lock (IsFGSourceFeature), with the feature pointer and without a lock;
//  - the call is declared once, in ext/reshade/ReShadeSeams.h (forward declarations only), and ReShadeDepth.cpp includes that header, so
//    the compiler enforces the match; ReShadeDepth.h does not declare it again;
//  - the hot path: the first statement of the seam is the one-load early-out;
//  - config load and reload only STORE (SetEnabled constructs nothing: the facade is built by the first evaluate);
//  - the NGX shutdown calls: OnNgxShutdownBegin and BeforeReleaseFeature.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static std::string Read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static std::string Normalize(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '"')
        {
            out += s[i++];
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            out += '"';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (std::isspace(static_cast<unsigned char>(s[i])))
        {
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else
            out += s[i];
    }
    return out;
}

static bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
static size_t Count(const std::string& s, const std::string& part)
{
    size_t n = 0;
    for (size_t p = s.find(part); p != std::string::npos; p = s.find(part, p + 1))
        ++n;
    return n;
}
static std::string Between(const std::string& s, const std::string& from, const std::string& to)
{
    const auto a = s.find(from);
    if (a == std::string::npos)
        return {};
    const auto b = s.find(to, a + from.size());
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string ext = repo + "/OptiScaler/ext/";

    // ---- the call site
    const auto upstream = Normalize(Read(repo + "/OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp"));
    const std::string call = "if (upscaleResult) ext::reshade::depth::OnEvaluated(InDevCtx, InParameters, deviceContext);";
    CHECK(Count(upstream, "ext::reshade::depth::OnEvaluated(") == 1);
    CHECK(Count(upstream, call) == 1);
    {
        const auto evaluate = Between(upstream, "NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext* InDevCtx", "#pragma endregion");
        CHECK(!evaluate.empty() && Has(evaluate, call)); // in the evaluate entry, nowhere else
        const auto atEvaluate = evaluate.find("auto upscaleResult = deviceContext->Evaluate(InDevCtx, InParameters);");
        const auto atSeam = evaluate.find(call);
        const auto atFrame = evaluate.find("activeContext->lastEvalFrame = state.frameCount;");
        const auto atTable = evaluate.find("State::Instance().activeFgInput == FGInput::Upscaler && IsFGSourceFeature(handleId)"); // the first table use after the evaluate
        CHECK(atEvaluate != std::string::npos && atSeam != std::string::npos && atFrame != std::string::npos && atTable != std::string::npos);
        // right after the evaluate, before anything that takes the feature table lock
        CHECK(atEvaluate < atSeam && atSeam < atFrame && atFrame < atTable);
        CHECK(evaluate.substr(atEvaluate + std::string("auto upscaleResult = deviceContext->Evaluate(InDevCtx, InParameters);").size(), 1) == " ");
        CHECK(evaluate.find(call) - atEvaluate == std::string("auto upscaleResult = deviceContext->Evaluate(InDevCtx, InParameters); ").size());
        // no lock and no table access in the seam's own line
        CHECK(!Has(call, "IsFGSourceFeature") && !Has(call, "Dx11Contexts"));
        // the arguments are the evaluate's own: the context NGX gave, the parameter block, the feature
        CHECK(Has(evaluate, "deviceContext = activeContext->feature.get();"));
    }
    CHECK(Has(upstream, "#include \"ext/Seams.h\""));

    // ---- the declaration, once
    const auto seams = Normalize(Read(ext + "reshade/ReShadeSeams.h"));
    const std::string declaration = "void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, IFeature_Dx11* feature) noexcept;";
    CHECK(Count(seams, "OnEvaluated(") == 1 && Has(seams, declaration));
    CHECK(Has(seams, "namespace ext::reshade::depth {"));
    CHECK(Has(seams, "struct ID3D11DeviceContext; struct NVSDK_NGX_Parameter; class IFeature_Dx11;"));
    CHECK(Has(seams, "#include <Windows.h>"));
    CHECK(Count(seams, "#include") == 1); // the header includes Windows.h and nothing else
    CHECK(Has(seams, "void OnNgxShutdown() noexcept;"));
    const auto facadeHeader = Normalize(Read(ext + "reshade/ReShadeDepth.h"));
    CHECK(!Has(facadeHeader, "OnEvaluated(") && !Has(facadeHeader, "void OnNgxShutdown"));
    const auto facade = Normalize(Read(ext + "reshade/ReShadeDepth.cpp"));
    CHECK(Has(facade, "#include \"ext/reshade/ReShadeSeams.h\""));
    CHECK(Has(facade, "void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, IFeature_Dx11* feature) noexcept {"));
    CHECK(Has(facade, "void OnNgxShutdown() noexcept {"));

    // ---- the hot path: the first statement of the seam is the one-load early-out
    {
        const auto body = Between(facade, "void OnEvaluated(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters, IFeature_Dx11* feature) noexcept {", "void OnNgxShutdown() noexcept {");
        CHECK(!body.empty());
        CHECK(body.find("{ if (g_gate.Idle()) return;") == body.find('{'));
        // the facade is built here and nowhere else
        CHECK(Count(facade, "Build()") == 2); // the definition and this one call
        CHECK(Has(body, "f = &Build();"));
    }

    // ---- the facade's wiring (it needs the process, so it is pinned here and compiled by the product build)
    {
        CHECK(Has(facade, "class ProcessHost final : public IEvalHost {"));
        CHECK(Has(facade, "bool ShuttingDown() override { return State::Instance().isShuttingDown; }"));
        CHECK(Has(facade, "bool Registered() override { return ext::reshade::Registered(); }"));
        CHECK(Has(facade, "bool TryLateRegister(bool enabled, int64_t nowMs) override { return ext::reshade::depth::TryLateRegister(enabled, nowMs); }"));
        CHECK(Has(facade, "if (SubscribedEvents() != kAllDepthEvents && FailedEvents() == 0) ext::reshade::depth::EnsureSubscribed();"));
        CHECK(Has(facade, "ConsumerInfo GetConsumer(int64_t nowMs) override { return _binder.GetConsumer(nowMs); }"));
        CHECK(Has(facade, "return ext::ffxiv::lighting::FfxivLightingCapture::installed.load() ? ext::ffxiv::lighting::FfxivLightingCapture::context : nullptr;"));
        CHECK(Has(facade, "DrawTriangleFn RawDraw() override { return &ext::ffxiv::chara::DrawFullscreenTriangle; }"));
        CHECK(Has(facade, "EventDispatcher dispatcher { binder, backend, g_frame };"));
        CHECK(Has(facade, "DepthBinder binder { backend, &LogCounter };"));
        CHECK(Has(facade, "LiveDepthBackend backend { LiveRawApi(), BackendEnv { &NowMsImpl, &FrameImpl, &PrimaryDeviceImpl } };"));
        CHECK(Has(facade, "CaptureEngine engine { g_gate, g_frame, *this, host };"));
        CHECK(Has(facade, "Facade() { binder.SetEnabled(g_gate.Enabled()); SetDispatcher(&dispatcher); }"));
        CHECK(Has(facade, "backend.SetCaptureDevice(capture.valid ? capture.device : 0); binder.PublishCapture(capture);"));
        CHECK(Has(facade, "return glue::Extent { f->DisplayWidth(), f->DisplayHeight() };"));
        CHECK(Has(facade, "f->engine.OnEvaluated(context, parameters, DisplaySource { feature != nullptr ? &DisplayOf : nullptr, feature });"));
        // nothing exists and the option is off: a request has nothing to carry out, it is dropped
        CHECK(Has(facade, "if (!g_gate.Enabled()) { g_gate.TakeRequests(); return; }"));
        CHECK(Has(facade, "f = &Build();"));
        // the status reads the registration the add-on module keeps, and the late planner's give-up
        CHECK(Has(facade, "const AddonStatus addon = ext::reshade::Status(); in.registered = addon.registered; in.reason = addon.reason;"));
        CHECK(Has(facade, "in.lateGaveUp = GetLateStatus().gaveUp;"));
        CHECK(Has(facade, "OnNgxShutdown() noexcept { try { if (Facade* f = g_facade.load(std::memory_order_acquire)) f->engine.OnNgxShutdown();"));
    }

    // ---- the menu card: it reads the facade only, writes the config and applies the switch at once
    {
        const auto menu = Normalize(Read(ext + "reshade/ReShadeDepthMenu.cpp"));
        CHECK(Has(menu, "#include \"ext/reshade/ReShadeDepth.h\""));
        CHECK(!Has(menu, "third_party") && !Has(menu, "reshade.hpp") && !Has(menu, "ReShadeDepthCore.h"));
        CHECK(Has(menu, "bool Visible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }"));
        CHECK(Has(menu, "bool on = cfg.automaticReShadeDepth.value_or_default();"));
        CHECK(Has(menu, "if (ImGui::Checkbox(\"Automatic scene depth\", &on)) { cfg.automaticReShadeDepth = on; ext::reshade::depth::SetEnabled(on); }"));
        CHECK(Has(menu, "const std::string status = ext::reshade::depth::Status(); ImGui::TextWrapped(\"%s\", status.c_str());"));
        CHECK(Has(menu, "{ \"reshade_depth\", \"FFXIV\", \"ReShade depth compatibility\", Draw, \"fg_dlssg\", Visible }"));
    }

    // ---- config load and reload: a pure store
    {
        const auto setEnabled = Between(facade, "void SetEnabled(bool on) noexcept {", "std::string Status() {");
        CHECK(!setEnabled.empty());
        CHECK(!Has(setEnabled, "Build()") && !Has(setEnabled, "new ")); // nothing is constructed
        CHECK(Has(setEnabled, "g_gate.SetEnabled(on);"));
        CHECK(Has(facade, "void OnConfigLoaded(bool on) { SetEnabled(on); }"));
        CHECK(Has(facade, "FfxivConfig::automaticReShadeDepthOnLoaded = &OnConfigLoaded"));
        // ... and a load that ran before the hook existed is made up for with the value the config holds
        CHECK(Has(facade, "bool RegisterLoadHook() { FfxivConfig::automaticReShadeDepthOnLoaded = &OnConfigLoaded; OnConfigLoaded(ext::Cfg().ffxiv.automaticReShadeDepth.value_or_default()); return true; }"));
        CHECK(Has(facade, "const bool kLoadHookRegistered = RegisterLoadHook();"));
    }
    {   // the ini fragment ships the key as `auto` (a value of true here would turn the feature on in every new ini)
        const std::string ini = Read(ext + "ini/FFXIV.ini");
        CHECK(Has(ini, "\nAutomaticReShadeDepth=auto"));
        CHECK(!Has(ini, "\nAutomaticReShadeDepth=true") && !Has(ini, "\nAutomaticReShadeDepth=false"));
        CHECK(Count(ini, "AutomaticReShadeDepth=") == 1);
    }
    const auto cfg = Normalize(Read(ext + "core/ExtConfig.cpp"));
    CHECK(Has(cfg, "b.Add(\"FFXIV\", \"AutomaticReShadeDepth\", automaticReShadeDepth);"));
    CHECK(Has(cfg, "window.PostLoad(); ffxiv.PostLoad(); }"));
    const auto cfgHeader = Normalize(Read(ext + "ffxiv/FfxivConfig.h"));
    CHECK(Has(cfgHeader, "CustomOptional<bool> automaticReShadeDepth { false };"));
    CHECK(Has(cfgHeader, "automaticReShadeDepthOnLoaded(automaticReShadeDepth.value_or_default());"));

    // ---- the NGX shutdown calls
    const auto nr = Normalize(Read(ext + "nr/NrSeams.cpp"));
    CHECK(Has(nr, "void OnNgxShutdownBegin() { ext::reshade::depth::OnNgxShutdown();"));
    const auto lifecycle = Normalize(Read(ext + "dx11bridge/NgxLifecycle.cpp"));
    CHECK(Has(lifecycle, "ext::upscaler::LogReleaseThread(handleId); ext::reshade::depth::OnNgxShutdown(); if (!IsolatesNgx() || !feature.IsWithDx12()) return ReleaseDrain::NotApplicable;"));
    CHECK(Has(nr, "#include \"ext/Seams.h\"") && Has(lifecycle, "#include \"ext/Seams.h\""));

    if (g_failed == 0)
        std::printf("reshade_depth_seam_pins: all checks passed\n");
    return g_failed;
}
