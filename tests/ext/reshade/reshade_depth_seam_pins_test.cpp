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
        // no lock and no table access between the evaluate and the seam: the text between them is the evaluate's own line and nothing else
        CHECK(evaluate.substr(atEvaluate, atSeam - atEvaluate) == "auto upscaleResult = deviceContext->Evaluate(InDevCtx, InParameters); ");
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
        // the evaluate thread subscribes only for a registration that neither path has seen: the load path leaves begin and finish to init_effect_runtime
        CHECK(Has(facade, "if (SubscriptionPath() == SubscribedVia::None && FailedEvents() == 0) ext::reshade::depth::EnsureSubscribed();"));
        CHECK(!Has(facade, "kAllDepthEvents"));
        CHECK(Has(facade, "ConsumerInfo GetConsumer(int64_t nowMs) override { return _binder.GetConsumer(nowMs); }"));
        CHECK(Has(facade, "return ext::ffxiv::lighting::FfxivLightingCapture::installed.load() ? ext::ffxiv::lighting::FfxivLightingCapture::context : nullptr;"));
        CHECK(Has(facade, "DrawTriangleFn RawDraw() override { return &ext::ffxiv::chara::DrawFullscreenTriangle; }"));
        CHECK(Has(facade, "EventDispatcher dispatcher { binder, backend };"));
        // the binder reads the counter the engine ticks: one atomic, nothing else moves it
        CHECK(Has(facade, "uint64_t FrameImpl() { return g_frame.load(std::memory_order_relaxed); }"));
        CHECK(Has(facade, "BackendEnv { &NowMsImpl, &FrameImpl, &PrimaryDeviceImpl, &remapEngine, &ScaleMotionImpl }"));
        CHECK(Count(facade, "g_frame") == 4); // the definition, FrameImpl, the engine and GetCaptureStats
        CHECK(Has(facade, "DepthBinder binder { backend, &LogCounter };"));
        CHECK(Has(facade, "LiveDepthBackend backend { LiveRawApi(), BackendEnv { &NowMsImpl, &FrameImpl, &PrimaryDeviceImpl, &remapEngine, &ScaleMotionImpl } };"));
        // the G-buffer engine: one instance, on the raw draw of the lighting hooks only for the context they hooked; the motion scaling reads the ini key where it is used
        CHECK(Has(facade, "LiveRemapEngine remapEngine { &ext::ffxiv::chara::DrawFullscreenTriangle, &HookedContextImpl };"));
        CHECK(Has(facade, "bool ScaleMotionImpl() { return ext::Cfg().ffxiv.automaticReShadeMotionScale.value_or_default(); }"));
        CHECK(Has(facade, "binder.SetSlotLog(&LogSlotCounter);"));
        CHECK(Has(facade, "CaptureEngine engine { g_gate, g_frame, *this, host };"));
        CHECK(Has(facade, "Facade() { binder.SetSlotLog(&LogSlotCounter); binder.SetEnabled(g_gate.Enabled()); SetDispatcher(&dispatcher); }"));
        CHECK(Has(facade, "void Publish(const DepthCapture& capture) noexcept override { binder.PublishCapture(capture); }"));
        // the counter is ticked by the capture side only (the engine), never by an event
        const auto dispatch = Normalize(Read(ext + "reshade/ReShadeDepthDispatch.h"));
        CHECK(!Has(dispatch, "fetch_add") && !Has(dispatch, "_frame") && !Has(dispatch, "IsOurRuntime"));
        const auto engine = Normalize(Read(ext + "reshade/ReShadeDepthEval.cpp"));
        CHECK(Has(engine, "const uint64_t stamp = _frame.fetch_add(1, std::memory_order_relaxed) + 1;"));
        CHECK(Has(engine, "args.frame = stamp;"));
        CHECK(Has(facade, "return glue::Extent { f->DisplayWidth(), f->DisplayHeight() };"));
        CHECK(Has(facade, "f->engine.OnEvaluated(context, parameters, DisplaySource { feature != nullptr ? &DisplayOf : nullptr, feature });"));
        // the game gate: the executable name, read once, after the one-load gate and before anything is built; the status says so
        CHECK(Has(facade, "const std::string& exe = State::Instance().gameExe; if (exe.empty()) return 0; state = glue::IsFfxivExe(exe) ? 1 : 2; g_game.store(state, std::memory_order_relaxed);"));
        CHECK(Has(facade, "if (GameState() != 1) return;"));
        CHECK(Has(facade, "in.gameSupported = GameState() != 2;"));
        {
            const auto body = Between(facade, "void OnEvaluated(ID3D11DeviceContext* context", "void OnNgxShutdown() noexcept {");
            CHECK(body.find("g_gate.Idle()") < body.find("GameState()") && body.find("GameState()") < body.find("f = &Build();"));
        }
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
        // the developer switch of the derived motion vector scaling: ini only, shipped as auto, says so
        CHECK(Has(ini, "\nAutomaticReShadeMotionScale=auto") && Count(ini, "AutomaticReShadeMotionScale=") == 1);
        CHECK(Has(ini, "will be removed once confirmed in game. No menu control."));
        CHECK(!Has(ini, "AutomaticReShadeGBuffers"));
    }
    const auto cfg = Normalize(Read(ext + "core/ExtConfig.cpp"));
    CHECK(Has(cfg, "b.Add(\"FFXIV\", \"AutomaticReShadeDepth\", automaticReShadeDepth);"));
    CHECK(Has(cfg, "b.Add(\"FFXIV\", \"AutomaticReShadeMotionScale\", automaticReShadeMotionScale);") && !Has(cfg, "AutomaticReShadeGBuffers"));
    CHECK(Has(cfg, "window.PostLoad(); ffxiv.PostLoad(); }"));
    const auto cfgHeader = Normalize(Read(ext + "ffxiv/FfxivConfig.h"));
    CHECK(Has(cfgHeader, "CustomOptional<bool> automaticReShadeDepth { false };"));
    CHECK(Has(cfgHeader, "CustomOptional<bool> automaticReShadeMotionScale { true };") && !Has(cfgHeader, "automaticReShadeGBuffers"));
    CHECK(Has(cfgHeader, "automaticReShadeDepthOnLoaded(automaticReShadeDepth.value_or_default());"));

    // ---- five depth events (init_effect_runtime among them) are subscribed AT LOAD, right after the registration and before the boundary tracking, whatever the option says; begin and finish come from the init callback
    {
        const auto addon = Normalize(Read(ext + "reshade/ReShadeAddon.cpp"));
        CHECK(Has(addon, "LOG_INFO(\"ReShade built-in add-on: registered (API {})\", kApiVersion); try { ext::reshade::depth::EnsureSubscribed(ext::reshade::depth::SubscribeWhen::Load); }"));
        CHECK(Count(addon, "EnsureSubscribed(") == 1);
        CHECK(addon.find("EnsureSubscribed(") < addon.find("InstallBoundaryTracking()"));
        CHECK(Has(addon, "#include \"ext/reshade/ReShadeDepthEvents.h\""));
        // the callbacks are not gated by the option and tolerate a facade that does not exist
        const auto events = Normalize(Read(ext + "reshade/ReShadeDepthEvents.cpp"));
        CHECK(Has(events, "EventDispatcher* Live() { if (State::Instance().isShuttingDown) return nullptr; return g_dispatcher.load(std::memory_order_acquire); }"));
        CHECK(Has(events, "if (EventDispatcher* d = Live())") && !Has(events, "automaticReShadeDepth") && !Has(events, "Enabled()"));
        CHECK(Has(events, "void SetDispatcher(EventDispatcher* dispatcher) noexcept { g_dispatcher.store(dispatcher, std::memory_order_release); }"));
        // the init callback: shutdown is its only gate (no dispatcher, no option), it subscribes finish then begin, and begin/finish are registered nowhere else
        {
            const auto init = Between(events, "void DepthOnInit(effect_runtime* runtime) noexcept {", "void DepthOnBegin(");
            CHECK(!init.empty());
            CHECK(Has(init, "if (State::Instance().isShuttingDown) return;") && Has(init, "SubscribeRuntimeEvents(Id(runtime));"));
            CHECK(!Has(init, "Live()") && !Has(init, "g_dispatcher") && !Has(init, "Enabled"));
            CHECK(Has(events, "for (const DepthEvent event : LateRegistrationPlanner::kRuntimeEvents) SubscribeOne(event);"));
            CHECK(Has(events, "for (const DepthEvent event : LateRegistrationPlanner::kLoadEvents) SubscribeOne(event);"));
            CHECK(Count(events, "register_event<addon_event::reshade_begin_effects>") == 1);
        }
    }

    // ---- one writer of the registration state: the load-time attempt notes it the way the late attempt does
    {
        const auto addon = Normalize(Read(ext + "reshade/ReShadeAddon.cpp"));
        CHECK(Has(addon, "NoteRegistration(result.status);"));
        CHECK(!Has(addon, "g.reason.store") && !Has(addon, "g.registered.store"));
        const auto late = Normalize(Read(ext + "reshade/ReShadeDepthLate.cpp"));
        CHECK(Has(late, "NoteRegistration(job.result.status);"));
    }

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
