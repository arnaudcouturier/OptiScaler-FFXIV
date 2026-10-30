// [PACE] Source pins. The pure tests cannot see a trace site that moved or a hidden present that bypasses the policy, so this test
// reads the sites (comments stripped, whitespace collapsed) and pins the exact statements: the seam call at the hidden present, the
// scope as the first statement of LocalPresent, the Reflex scope before the sleep, and the ext-side sites of the other phases.
// A pin that stops matching means a site moved: re-derive it, do not loosen it. Source text only, no git inside a test.
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

static bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

static size_t Count(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (auto p = text.find(what); p != std::string::npos; p = text.find(what, p + 1))
        ++n;
    return n;
}

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
    const auto bridge = Normalize(Read(repo + "/OptiScaler/with_dx12/dx11_with_dx12_sc.cpp"));
    const auto wrapped = Normalize(Read(repo + "/OptiScaler/wrapped/wrapped_swapchain.cpp"));
    const auto fgHooks = Normalize(Read(repo + "/OptiScaler/hooks/FG_Hooks.cpp"));
    const auto bridgeHdr = Normalize(Read(repo + "/OptiScaler/ext/hdr/BridgeHdr.cpp"));
    const auto nrPass = Normalize(Read(repo + "/OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp"));
    const auto chara = Normalize(Read(repo + "/OptiScaler/ext/ffxiv/chara/CharaNr.cpp"));
    const auto preset = Normalize(Read(repo + "/OptiScaler/ext/fg/dlssg/PresetSeams.cpp"));
    const auto guide = Normalize(Read(repo + "/OptiScaler/ext/fg/GuideSeams.cpp"));
    const auto trace = Normalize(Read(repo + "/OptiScaler/ext/present/FramePacingTrace.cpp"));
    CHECK(bridge.size() > 1000 && wrapped.size() > 1000 && fgHooks.size() > 1000 && bridgeHdr.size() > 500 && nrPass.size() > 1000 &&
          chara.size() > 1000 && preset.size() > 500 && guide.size() > 500 && trace.size() > 1000); // the files were read

    // ---- trace-off cost: the "did the decision change" check of every scope is a load; the locked exchange only runs on a change -------
    CHECK(Has(trace, "if (!AnnounceChanged(g_announced, AnnounceKey(d.reason, logToFile, level))) return;"));
    CHECK(!Has(trace, "g_announced.exchange"));

    // ---- hidden-present seam (the body is hidden::SeamPresent, behaviour-tested by hidden_present_seam): the .cpp only supplies the key
    // read and the report hook, opens the scope first, and makes no swapchain call of its own --------------------------------------------
    CHECK(Has(trace, "bool KeyOn() const { return ext::Cfg().framePacing.hiddenPresentDoNotWait.value_or_default(); }"));
    CHECK(Count(trace, "hiddenPresentDoNotWait.value_or_default()") == 2); // KeyOn and the "skipped k" column of the hidden-present line
    // ONE contiguous statement sequence: the scope opens, the hooks are built, and the recorder arms and times around the single seam call (a changed flags or swapchain
    // argument computed in between would defeat "key off = exactly Present(0, callerFlags)", which the seam test checks only for the
    // template; key off still takes the same call with the same arguments, only without the timing). Whitespace and
    // comments are normalised away, nothing else may sit between the pieces.
    CHECK(Has(trace, "HRESULT HiddenPresent(IDXGISwapChain* swapChain, UINT flags) { PacingScope scope(Phase::HiddenPresent); HiddenHooks hooks { scope }; LARGE_INTEGER begin {}, end {}; const bool rec = ext::fgrec::RecArmed(); if (rec) QueryPerformanceCounter(&begin); const HRESULT hr = hidden::SeamPresent(HiddenPolicy(), swapChain, flags, hooks); if (rec) { QueryPerformanceCounter(&end); ext::fgrec::OnHiddenPresent(hr, end.QuadPart - begin.QuadPart); } return hr; }"));
    CHECK(Count(trace, "hidden::SeamPresent(") == 1); // still exactly one call on the hidden chain
    CHECK(!Has(trace, "->Present(")); // the only swapchain call lives in the (tested) template
    CHECK(!Has(trace, ".Present(")); // nor through a dereference spelled another way

    // ---- hidden present: the seam call, the flags variable and the failure warning stay as upstream had them ----------------------------
    {
        const auto block = Between(bridge, "UINT realFlags = Flags;", "auto result = _fgSwapChain->Present(SyncInterval, Flags);");
        CHECK(!block.empty());
        CHECK(Has(block, "auto realPresentResult = ext::present::HiddenPresent(_real, realFlags);"));
        CHECK(Has(block, "if (FAILED(realPresentResult)) LOG_WARN(\"hidden real DX11 Present failed: {:X}\", (UINT) realPresentResult);"));
        // the guard of the block is upstream's (a real swapchain, not a Linux run)
        CHECK(Has(bridge, "if (_real != nullptr && !State::Instance().isRunningOnLinux) { UINT realFlags = Flags;"));
    }
    // no present on the hidden chain bypasses the seam: the only Present(0, ...) call on _real is inside it
    CHECK(Count(bridge, "ext::present::HiddenPresent(") == 1);
    CHECK(!Has(bridge, "_real->Present(0"));

    // ---- output present: the scope is the first statement of LocalPresent, before the shutdown early return --------------------------
    {
        const auto head = Between(wrapped, "static HRESULT LocalPresent(IDXGISwapChain* pSwapChain", "if (State::Instance().isShuttingDown)");
        CHECK(!head.empty());
        CHECK(Has(head, "bool isUWP) { ext::present::PacingScope outputPresent(ext::present::Phase::OutputPresent); "));
        CHECK(Count(wrapped, "ext::present::PacingScope") == 1);
    }

    // ---- Reflex sleep: the scope sits directly before the sleep, in the same block, before MarkersNextFrame ---------------------------
    {
        CHECK(Count(fgHooks, "ext::present::PacingScope") == 1);
        CHECK(Has(fgHooks, "ext::present::PacingScope reflexSleep(ext::present::Phase::ReflexSleep); StreamlineProxy::ReflexSleep()(*localToken); ext::fg::MarkersNextFrame("));
    }

    // ---- FG event recorder: one begin/end pair per present path, beside the seams that already time them ------------
    // FgPresentBegin sits after the willPresent timing block, before the feature lookup; FgPresentEnd reports the
    // HRESULT at the tail return. OutPresentBegin sits before the original-present call, OutPresentEnd at the tail.
    CHECK(Count(fgHooks, "ext::fgrec::FgPresentBegin(willPresent, SyncInterval, Flags);") == 1);
    CHECK(Count(wrapped, "ext::fgrec::OutPresentBegin(willPresent, SyncInterval, Flags);") == 1);
    CHECK(Count(fgHooks, "ext::fgrec::FgPresentEnd(willPresent, result);") == 1);
    CHECK(Count(wrapped, "ext::fgrec::OutPresentEnd(willPresent, presentResult);") == 1);
    CHECK(Has(fgHooks, "ext::fgrec::FgPresentBegin(willPresent, SyncInterval, Flags); IFGFeature* fg = state.currentFG;"));
    CHECK(Has(fgHooks, "LOG_DEBUG(\"Present finished\"); ext::fgrec::FgPresentEnd(willPresent, result); return result; }"));
    CHECK(Has(wrapped, "ext::fgrec::OutPresentBegin(willPresent, SyncInterval, Flags); LOG_DEBUG(\"Calling original present\");"));
    CHECK(Has(wrapped, "ext::fgrec::OutPresentEnd(willPresent, presentResult); return presentResult; }"));

    // ---- native present: the bridge's PresentScope opens and closes the span --------------------------------------------------------------
    {
        const auto ctor = Between(bridgeHdr, "PresentScope::PresentScope() {", "PresentScope::~PresentScope()");
        CHECK(!ctor.empty());
        CHECK(Has(ctor, "PresentScope::PresentScope() { ext::present::NativeBegin(); "));
        const auto dtor = Between(bridgeHdr, "PresentScope::~PresentScope() {", "void BeforeSharedCopy(");
        CHECK(!dtor.empty());
        CHECK(Has(dtor, "window::OnPresentEnd(); ext::present::NativeEnd(); }")); // last, so the span covers every close-out
    }

    // ---- scene NR: the scope is the first statement of Dispatch, before the lock ---------------------------------------------------------
    CHECK(Has(nrPass, "const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue) { ext::present::PacingScope sceneNr(ext::present::Phase::SceneNr); std::lock_guard<std::recursive_mutex> nrLock(g_nrMutex);"));
    CHECK(Count(nrPass, "ext::present::PacingScope") == 1);

    // ---- preview NR: the recording time that is already measured is fed to the trace -----------------------------------------------------
    CHECK(Has(chara, "m.cpuCost.Add(ops.cpuMs); ext::present::RecordSample(ext::present::Phase::PreviewNr, static_cast<uint64_t>(ops.cpuMs * 1000.0));"));
    CHECK(Count(chara, "ext::present::RecordSample(") == 1);

    // ---- FG dispatch: begins at the first statement of PresetSwitchTick, ends at the first statement of PostDispatch -------------------
    CHECK(Has(preset, "bool PresetSwitchTick(IFGFeature* fg, const sl::ViewportHandle& viewport) { ext::present::FgDispatchBegin(); "));
    CHECK(Count(preset, "ext::present::FgDispatchBegin()") == 1);
    CHECK(Has(guide, "Provider provider) { ext::present::FgDispatchEnd(); if (!SceneBridge()) return;"));
    CHECK(Count(guide, "ext::present::FgDispatchEnd()") == 1);

    if (g_failed != 0)
    {
        std::printf("frame_pacing_upstream_pins: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("frame_pacing_upstream_pins: all passed\n");
    return 0;
}
