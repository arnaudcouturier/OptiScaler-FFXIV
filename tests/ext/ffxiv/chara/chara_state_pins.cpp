// Text pins of the character-preview passes that draw from inside a hooked game draw: the spatial AA (FxaaPass::Process) and the DLSS guide
// passes (guides::Passes::Scope). Both used to run inside a private device-context state; in this runtime the swap into it is deferred to the
// outermost "amortized" entry, so a pass entered from a nested draw ran on the game's live state (REPORT of the explicit-state fix). They now save
// and restore the state they touch explicitly (ext/core/D3D11ExplicitState.h), like the engine and relight passes. Pinned here: the guard, its Spec,
// the order of the calls around it, the three defaults the swap used to provide that the guides scope has to set itself, and that every NGX call
// on the game's context sits behind a full snapshot.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// Comments and whitespace removed, so a pin is about the code and not about its formatting.
std::string Comments(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text.compare(i, 2, "/*") == 0)
        {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        out += text[i];
    }
    return out;
}

std::string Code(const std::string& repo, const char* relative)
{
    std::string text = Comments(ReadFile(repo + "/OptiScaler/ext/ffxiv/chara/" + relative));
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Pos(const std::string& text, const std::string& needle) { return text.find(needle); }
size_t Count(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}
// `a` occurs, `b` occurs, and the first `a` is before the first `b`.
bool Before(const std::string& text, const std::string& a, const std::string& b)
{
    const size_t pa = Pos(text, a), pb = Pos(text, b);
    return pa != std::string::npos && pb != std::string::npos && pa < pb;
}
// `a` occurs after position `from` and before the next `b`.
bool Between(const std::string& text, size_t from, const std::string& a, const std::string& b)
{
    if (from == std::string::npos)
        return false;
    const size_t pb = text.find(b, from);
    const size_t pa = text.find(a, from);
    return pa != std::string::npos && pb != std::string::npos && pa < pb;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: chara_state_pins <repository root>\n");
        return 2;
    }
    const std::string repo = argv[1];

    // ---- the spatial AA --------------------------------------------------------------------------------------------------------------------------
    {
        const std::string pass = Code(repo, "CharaEnhancePass.cpp");
        const std::string header = Code(repo, "CharaEnhancePass.h");
        EXPECT(!pass.empty() && !header.empty(), "the AA pass files were read");
        EXPECT(Has(pass, R"x(constexprext::core::explicit_state::SpeckSpec{1,1,0};)x"),
               "the AA declares exactly its own state: one pixel-shader resource (t0), one sampler (s0), no constant buffer");
        EXPECT(Has(pass, R"x(ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);)x") &&
                   Has(pass, R"x(stateGuard.Neutralise();)x"),
               "the game's state is captured and the private state's defaults (no HS, DS, GS, stream-out or predicate) are set explicitly");
        EXPECT(!Has(pass, "SwapDeviceContextState") && !Has(pass, "CreateDeviceContextState") && !Has(pass, "ID3DDeviceContextState") && !Has(header, "ID3DDeviceContextState") &&
                   !Has(pass, "CharaContextState.h") && !Has(pass, "ContextStateFlags") && !Has(pass, "ID3D11Device1"),
               "no device-context state swap or private state remains: the swap does nothing when the draw hook is nested");
        EXPECT(Has(pass, R"x(#include"ext/core/D3D11ExplicitState.h")x"), "the helper is included");
        // order: the state-free preparation, then the guard, then the first setter
        EXPECT(Before(pass, "EnsureOutput(device.Get(),width,height,srgb)", "stateGuard(context,"), "the output texture is made before the guard (no state involved)");
        EXPECT(Before(pass, "stateGuard.Neutralise();", "context->IASetInputLayout(nullptr);"), "the guard captures before the first setter");
        EXPECT(Before(pass, "context->IASetInputLayout(nullptr);", "draw(context);"), "the pass sets its state, then draws");
        // the state window opens after the last way to refuse the run
        const size_t guard = Pos(pass, "stateGuard(context,");
        EXPECT(guard != std::string::npos && pass.find("returnfalse;", guard) == std::string::npos, "no refusal after the guard: a refused run leaves the context untouched");
        // the cleanup the pass already did stays, the guard restores after it
        EXPECT(Between(pass, guard, "context->OMSetRenderTargets(0,nullptr,nullptr);", "*out=_outputSrv.Get();"), "the pass drops its target before it returns the view");
    }

    // ---- the DLSS guide passes --------------------------------------------------------------------------------------------------------------------
    {
        const std::string pass = Code(repo, "CharaGuides.cpp");
        const std::string header = Code(repo, "CharaGuides.h");
        EXPECT(!pass.empty() && !header.empty(), "the guide pass files were read");
        EXPECT(Has(pass, R"x(constexprext::core::explicit_state::SpeckSpec{4,1,2};)x"),
               "the guide passes declare exactly their own state: four pixel-shader resources (t0-t3), one sampler (s0), two constant buffers (b0, b1)");
        EXPECT(Has(pass, R"x(_guard.emplace(context,_context1.Get(),passes._fl111,kSpec);)x") && Has(pass, R"x(_guard->Neutralise();)x"),
               "the guard is built in the scope's constructor where the swap was, and the private state's defaults (no HS, DS, GS, stream-out or predicate) are set explicitly");
        EXPECT(!Has(pass, "SwapDeviceContextState") && !Has(pass, "CreateDeviceContextState") && !Has(pass, "ID3DDeviceContextState") && !Has(header, "ID3DDeviceContextState") &&
                   !Has(pass, "CharaContextState.h") && !Has(pass, "ContextStateFlags") && !Has(pass, "ID3D11Device1") && !Has(pass, "_previous") && !Has(header, "_previous") &&
                   !Has(pass, "passes._state") && !Has(header, "_state;"),
               "no device-context state swap or private state remains");
        // HS, DS and GS go back only when Neutralise() took them off: the explicit nulls of the private state must not come back without it
        EXPECT(!Has(pass, "GSSetShader(nullptr") && !Has(pass, "HSSetShader(nullptr") && !Has(pass, "DSSetShader(nullptr"),
               "no explicit null HS/DS/GS: Neutralise() takes them off and the guard puts them back only after it ran");
        // the three defaults the swap provided and the scope never set
        const size_t emplace = Pos(pass, "_guard.emplace(");
        const size_t dtor = Pos(pass, "Passes::Scope::~Scope()");
        EXPECT(emplace != std::string::npos && dtor != std::string::npos && emplace < dtor, "the guard is built in the constructor, before the destructor");
        const std::string ctor = emplace != std::string::npos && dtor != std::string::npos && emplace < dtor ? pass.substr(emplace, dtor - emplace) : std::string();
        EXPECT(Has(ctor, "_context->RSSetState(nullptr);"), "the default rasterizer state is set (the scope relied on the private state's)");
        EXPECT(Has(ctor, "_context->OMSetBlendState(nullptr,nullptr,0xffffffff);"), "the default blend state and sample mask are set");
        EXPECT(Has(ctor, "_context->OMSetDepthStencilState(nullptr,0);"), "the default depth-stencil state is set");
        EXPECT(Before(ctor, "_guard->Neutralise();", "_context->RSSetState(nullptr);") && Before(ctor, "_context->PSSetConstantBuffers(0,1,&settingsBuffer);", "_ok=true;"),
               "the guard is up before the first setter, and the scope is ready after the last");
        EXPECT(Before(pass, "context->UpdateSubresource(passes._settings.Get(),0,nullptr,&settings,0,0);", "_guard.emplace("),
               "the settings upload stays before the guard, in the order the swap had");
        EXPECT(Has(pass, "Passes::Scope::~Scope(){_guard.reset();}") || Has(pass, "Passes::Scope::~Scope(){_compute.reset();_guard.reset();}"),
               "the destructor puts the game's state back explicitly, on every exit of the scope");
        // class layout: the optional guard is declared after the context interface it points at, so it is destroyed first
        EXPECT(Has(header, "std::optional<ext::core::explicit_state::Guard>_guard;") && Before(header, "ComPtr<ID3D11DeviceContext1>_context1;", "std::optional<ext::core::explicit_state::Guard>_guard;"),
               "the guard member follows the context interface it uses");
        EXPECT(Has(header, "ext::core::explicit_state::FullStateSnapshotKeepState();"), "the scope offers a full snapshot for calls whose state use is unknown");
        EXPECT(Has(header, "boolReady()const{return_ready;}") && Has(pass, "_ready=true;") && Has(pass, "_ready=false;"), "Ready() no longer asks for the private state");
    }

    // ---- NGX on the game's context ----------------------------------------------------------------------------------------------------------------
    {
        const std::string dlss = Code(repo, "CharaDlss.cpp");
        EXPECT(!dlss.empty(), "CharaDlss.cpp was read");
        EXPECT(Count(dlss, "autocreate=NVNGXProxy::D3D11_CreateFeature();") == 1 && Count(dlss, "autoevaluate=NVNGXProxy::D3D11_EvaluateFeature();") == 1,
               "the preview calls NGX CreateFeature and EvaluateFeature from one place each");
        EXPECT(Count(dlss, "scope.KeepState()") == 2, "two snapshots: one around CreateFeature, one around the evaluate");
        EXPECT(Before(dlss, "autongxState=scope.KeepState();", "create(context,NVSDK_NGX_Feature_SuperSampling,params,&handle)"),
               "the snapshot is taken before NGX CreateFeature and lives until the end of its block");
        EXPECT(Before(dlss, "{autongxState=scope.KeepState();result=evaluate(context,g.handle,g.params,nullptr);}", "if(ev.ok)g.lastColour=ev.colour;"),
               "the snapshot is taken immediately before the NGX evaluate and ends right after it (a block of its own)");
        EXPECT(Count(dlss, "result=evaluate(context,g.handle,g.params,nullptr);") == 1, "one NGX evaluate call in RunSlot");
        // the call sites of the scope are unchanged
        EXPECT(Has(dlss, "guides::Passes::Scopescope(g_passes,context,settings,camera::Buffer(),camera::FirstConstant(),camera::ConstantCount());"),
               "RunSlot enters the scope as before");
        // no NGX D3D11 call anywhere else in the preview module
        for (const char* file : { "CharaNr.cpp", "CharaMotion.cpp", "CharaDiag.cpp", "CharaCamera.cpp", "CharaFlood.cpp", "CharaNrTint.cpp", "CharaDispatch.cpp" })
        {
            const std::string text = Code(repo, file);
            EXPECT(!text.empty() && !Has(text, "D3D11_EvaluateFeature") && !Has(text, "D3D11_CreateFeature"), "%s does not call the NGX D3D11 entries", file);
        }
    }

    // ---- the helper: the full snapshot keeps the UAV counters ---------------------------------------------------------------------------------
    {
        const std::string helper = Comments(ReadFile(repo + "/OptiScaler/ext/core/D3D11ExplicitState.h"));
        std::string code;
        for (char c : helper)
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                code += c;
        EXPECT(!code.empty(), "D3D11ExplicitState.h was read");
        // WARP's CopyStructureCount is not reliable (it reads 0 in about half of the runs whatever is bound), so no WARP test sees a restore that resets a counter
        // every time: the initial counts of the restores are pinned in the text. -1 keeps the bound counter.
        EXPECT(Has(code, "UINTkeepCounters[kMaxUavSlots];for(UINT&k:keepCounters)k=static_cast<UINT>(-1);_c->CSSetUnorderedAccessViews(0,_uavSlots,_csUav.p,keepCounters);"),
               "FullStateSnapshot restores the compute-shader UAVs with initial counts -1 (the bound counters are kept)");
        EXPECT(Has(code, "UINTkeepCounters[kMaxUavSlots];for(UINT&k:keepCounters)k=static_cast<UINT>(-1);_c->OMSetRenderTargetsAndUnorderedAccessViews("),
               "OmSnapshot restores the pixel-shader UAVs with initial counts -1");
    }

    if (g_failures == 0)
        std::printf("chara_state_pins: all pins hold\n");
    return g_failures == 0 ? 0 : 1;
}
