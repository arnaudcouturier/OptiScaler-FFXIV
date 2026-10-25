// [REL] Source pins. The pure tests cannot see an inverted condition in the one upstream file the release path lives in, so
// this test reads inputs/NVNGX_DLSS_Dx11.cpp and ext/dx11bridge/NgxLifecycle.cpp (comments stripped, whitespace collapsed)
// and pins the exact lines. A pin that stops matching means the release path changed: re-derive it, do not loosen it.
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

// Comments out, string literals kept, every run of whitespace one space.
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
            if (out.empty() || out.back() != ' ') out += ' ';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            ++i;
            if (out.empty() || out.back() != ' ') out += ' ';
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

static bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

// text between the start of `from` and the next occurrence of `to` after it ("" when missing)
static std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const auto a = text.find(from);
    if (a == std::string::npos)
        return {};
    const auto b = text.find(to, a + from.size());
    return b == std::string::npos ? std::string {} : text.substr(a, b - a);
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const auto upstream = Normalize(Read(repo + "/OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp"));
    const auto glue = Normalize(Read(repo + "/OptiScaler/ext/dx11bridge/NgxLifecycle.cpp"));
    CHECK(upstream.size() > 1000 && glue.size() > 1000); // the files were read

    // the release function body
    const auto release = Between(upstream, "NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle* InHandle)", "NVSDK_NGX_D3D11_GetFeatureRequirements");
    CHECK(!release.empty());

    // the result of the seam is kept and Failed (only Failed) refuses the release
    CHECK(Has(release, "const auto drain = ext::dx11bridge::BeforeReleaseFeature(*deviceContext, handleId, shutdown);"));
    CHECK(Has(release, "if (drain == ext::dx11bridge::ReleaseDrain::Failed) return NVSDK_NGX_Result_FAIL_PlatformError;"));

    // the sleep BEFORE the reset: skipped only through the seam, never on shutdown
    CHECK(Has(release, "if (!shutdown && !ext::dx11bridge::ShouldSkipReleaseSleep(drain)) { LOG_TRACE(\"sleeping for 500ms before reset()!\"); std::this_thread::sleep_for(std::chrono::milliseconds(500)); }"));
    // the call comes after the seam and before the reset
    {
        const auto atSeam = release.find("BeforeReleaseFeature(");
        const auto atSleep = release.find("ShouldSkipReleaseSleep(drain)");
        const auto atReset = release.find("Dx11Contexts.Extract(handleId)");
        CHECK(atSeam != std::string::npos && atSleep != std::string::npos && atReset != std::string::npos);
        CHECK(atSeam < atSleep && atSleep < atReset);
    }
    // the sleep AFTER the reset is still the upstream one: Dx11DelayedInit, no skip
    CHECK(Has(release, "if (!shutdown && Config::Instance()->Dx11DelayedInit.value_or_default()) { LOG_TRACE(\"sleeping for 500ms after reset()!\"); std::this_thread::sleep_for(std::chrono::milliseconds(500)); }"));
    // the entry leaves the table through Extract (detached under the lock, destroyed after it); no reset/erase of the table is left
    CHECK(Has(release, "ext::dx11bridge::ClearCurrentFeatureIf(deviceContext);"));
    CHECK(Has(release, "{ auto detached = Dx11Contexts.Extract(handleId); }"));
    CHECK(!Has(release, ".feature.reset()"));
    CHECK(!Has(release, "Dx11Contexts.erase("));
    CHECK(!Has(release, "State::Instance().currentFeature = nullptr;"));
    // the table: declared as the locked, reference-stable ContextTable; the one iteration is under the lock
    CHECK(Has(upstream, "static ext::dx11bridge::ContextTable<ContextData<IFeature_Dx11>> Dx11Contexts;"));
    CHECK(Has(upstream, "for (const auto& [id, context] : Dx11Contexts.Locked())"));
    CHECK(!Has(upstream, "ankerl::unordered_dense::map<unsigned int, ContextData<IFeature_Dx11>>"));
    // both writers of currentFeature in create/evaluate go through the locked setter
    {
        const auto create = Between(upstream, "NVSDK_NGX_D3D11_CreateFeature(ID3D11DeviceContext* InDevCtx", "NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle* InHandle)");
        const auto evaluate = Between(upstream, "NVSDK_NGX_D3D11_EvaluateFeature(ID3D11DeviceContext* InDevCtx", "#pragma endregion");
        CHECK(Has(create, "ext::dx11bridge::SetCurrentFeature(deviceContext);"));
        CHECK(!Has(create, "currentFeature = "));
        CHECK(Has(evaluate, "ext::dx11bridge::SetCurrentFeature(deviceContext);"));
        CHECK(!Has(evaluate, "currentFeature = "));
        // the evaluate pin: after the pass-through branch, before anything touches the table, refuses a closed handle
        const auto atPin = evaluate.find("ext::dx11bridge::EvaluatePin evaluatePin(handleId);");
        const auto atRefuse = evaluate.find("if (!evaluatePin) return NVSDK_NGX_Result_Success;");
        const auto atTable = evaluate.find("&Dx11Contexts[handleId]");
        const auto atProxy = evaluate.find("handleId < DLSS_MOD_ID_OFFSET");
        CHECK(atPin != std::string::npos && atRefuse != std::string::npos && atTable != std::string::npos && atProxy != std::string::npos);
        CHECK(atProxy < atPin && atPin < atRefuse && atRefuse < atTable);
    }

    // exactly two 500 ms sleeps in the function, one skip call
    {
        size_t sleeps = 0, skips = 0;
        for (auto p = release.find("sleep_for"); p != std::string::npos; p = release.find("sleep_for", p + 1))
            ++sleeps;
        for (auto p = release.find("ShouldSkipReleaseSleep"); p != std::string::npos; p = release.find("ShouldSkipReleaseSleep", p + 1))
            ++skips;
        CHECK(sleeps == 2);
        CHECK(skips == 1);
    }

    // the seam: key on + Drained + not shutting down, read from the [NgxRelease] key
    CHECK(Has(glue, "bool ShouldSkipReleaseSleep(ReleaseDrain drain) {"));
    CHECK(Has(glue, "return SkipReleaseSleep(drain, ext::Cfg().ngxRelease.skipReleaseSleep.value_or_default(), false);"));
    // BeforeReleaseFeature: the gate (closing the handle and waiting) needs the key and a non-shutdown release; the order is
    // PrepareRelease's; the early return for other games / non-bridge features is the first statement
    {
        const auto body = Between(glue, "ReleaseDrain BeforeReleaseFeature(IFeature_Dx11& feature, unsigned handleId, bool shuttingDown) {", "return result; }");
        CHECK(!body.empty());
        CHECK(Has(body, "plan.gate = !shuttingDown && !xefgRebuild && ext::Cfg().ngxRelease.skipReleaseSleep.value_or_default();"));
        // the XeFG rebuild decides once, before the drain, and the block that rebuilds uses the same flag
        CHECK(Has(body, "const bool xefgRebuild = !shuttingDown && State::Instance().activeFgOutput == FGOutput::XeFG && ext::Cfg().fg.xefgResetOnUpscalerChange.value_or_default() && &feature == State::Instance().currentFeature && fg != nullptr && State::Instance().activeFgInput == FGInput::Upscaler;"));
        CHECK(Has(glue, "if (xefgRebuild) { OwnedLockGuard guard(fg->Mutex, 5);"));
        CHECK(body.find("const bool xefgRebuild") < body.find("PrepareRelease(Registry(), plan, ops)"));
        CHECK(Has(body, "PrepareRelease(Registry(), plan, ops)"));
        CHECK(Has(body, "bool shuttingDown) { ext::upscaler::LogReleaseThread(handleId); ext::reshade::depth::OnNgxShutdown(); if (!IsolatesNgx() || !feature.IsWithDx12()) return ReleaseDrain::NotApplicable;")); // the thread line comes first, then the depth pass gives its device reference back (every release, whatever the game), both before the early return
        CHECK(Has(body, "if (result == ReleaseDrain::Failed) {"));
    }
    // the key binding
    const auto cfg = Normalize(Read(repo + "/OptiScaler/ext/core/ExtConfig.cpp"));
    CHECK(Has(cfg, "b.Add(\"NgxRelease\", \"SkipReleaseSleep\", skipReleaseSleep);"));
    const auto cfgHeader = Normalize(Read(repo + "/OptiScaler/ext/dx11bridge/NgxReleaseConfig.h"));
    CHECK(Has(cfgHeader, "CustomOptional<bool> skipReleaseSleep { true };")); // ON by default

    if (g_failed == 0)
        std::printf("release_upstream_pins: all checks passed\n");
    return g_failed;
}
