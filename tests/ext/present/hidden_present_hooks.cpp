// [PACE] The hidden present's hooks: struct HiddenHooks, sliced verbatim out of ext/present/FramePacingTrace.cpp by
// run_hidden_present_hooks.ps1 and driven through the real hidden::SeamPresent against a fake swapchain. The switch itself is
// hidden_present_policy's subject and the seam's flags and answers are hidden_present_seam's; this test covers what only the game's
// hooks supply to them:
//   key off  -> the chain sees exactly the caller's flags, and the chain's own answer comes back untouched
//   key on   -> the chain sees callerFlags | DXGI_PRESENT_DO_NOT_WAIT; a busy chain is skipped, reads as S_OK and feeds the trace's
//               skipped counter
//   the key is read on every present, so a change in the menu or the ini takes effect on the next frame and back
//   a chain that rejects the flag is announced once through the hook's warning, and the flag stays off afterwards
//   a run of kEpisodeSkips skips in a row writes the hook's single line, with the count in it
// The two outside dependencies are stand-ins: the key (ext::Cfg()) and the log lines (LOG_INFO / LOG_WARN). No DXGI, no swapchain.
#include "ext/present/HiddenPresentPolicy.h"
#include "ext/present/PresentSeams.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <utility>
#include <vector>

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

namespace
{
// The log lines the hooks write, kept as text so the checks can read them back (the game's logger is spdlog), and the trace's
// skipped counter the hook feeds.
int g_warnLines = 0;
int g_infoLines = 0;
int g_skippedNotes = 0;
std::string g_lastWarn;
std::string g_lastInfo;

template <class... A> void TestWarn(std::format_string<A...> fmt, A&&... args)
{
    ++g_warnLines;
    g_lastWarn = std::format(fmt, std::forward<A>(args)...);
}
template <class... A> void TestInfo(std::format_string<A...> fmt, A&&... args)
{
    ++g_infoLines;
    g_lastInfo = std::format(fmt, std::forward<A>(args)...);
}
} // namespace

#define LOG_WARN(...) TestWarn(__VA_ARGS__)
#define LOG_INFO(...) TestInfo(__VA_ARGS__)

namespace ext
{
// Stand-in for the product config (ext/core/ExtConfig.h): only the key the hook reads, settable from the test. The hook goes
// through ext::Cfg() so that the read is counted; the test sets the value through TestCfg() and does not count itself.
template <class T> struct Setting
{
    T v {};
    Setting() = default;
    Setting(const T& x) : v(x) {}
    Setting& operator=(const T& x)
    {
        v = x;
        return *this;
    }
    T value_or_default() const { return v; }
};
struct StubFramePacingConfig
{
    Setting<bool> hiddenPresentDoNotWait;
};
struct StubConfig
{
    StubFramePacingConfig framePacing;
};
inline unsigned& CfgReads()
{
    static unsigned n = 0;
    return n;
}
inline StubConfig& TestCfg()
{
    static StubConfig c;
    return c;
}
inline StubConfig& Cfg()
{
    ++CfgReads();
    return TestCfg();
}
} // namespace ext

namespace ext::present
{
// The trace's skipped counter the hook feeds (NoteSkipped) and the one policy per process it reads the total from (HiddenPolicy),
// both provided by FramePacingTrace.cpp in the product.
void NoteSkipped(Phase) { ++g_skippedNotes; }
hidden::Policy& HiddenPolicy()
{
    static hidden::Policy policy;
    return policy;
}

// The scope as the game runs it with the trace on: active, so a skipped present is counted.
PacingScope::PacingScope(Phase phase) noexcept : _phase(phase), _startUs(0), _active(true) {}
PacingScope::~PacingScope() = default;

namespace
{
// ---- production text: struct HiddenHooks, sliced by the runner (it fails when the struct moves) ----
#include "hidden_present_hooks_slice.inl"
} // namespace
} // namespace ext::present

using ext::present::HiddenHooks;
using ext::present::HiddenPolicy;
using ext::present::PacingScope;
using ext::present::Phase;
using namespace ext::present::hidden;

namespace
{
// The shape of the hidden chain's call the seam makes: IDXGISwapChain::Present(UINT SyncInterval, UINT Flags). Records every call and
// answers like the chain does (a busy chain answers a DO_NOT_WAIT present with WAS_STILL_DRAWING instead of blocking).
struct FakeChain
{
    struct Call
    {
        UINT sync;
        UINT flags;
    };
    std::vector<Call> calls;
    HRESULT answer = S_OK;        // the answer to every present
    bool busy = false;            // a DO_NOT_WAIT present answers WAS_STILL_DRAWING
    bool rejectDoNotWait = false; // a DO_NOT_WAIT present answers this chain's `rejection`
    HRESULT rejection = E_INVALIDARG;

    HRESULT Present(UINT sync, UINT flags)
    {
        calls.push_back({ sync, flags });
        if (rejectDoNotWait && (flags & DXGI_PRESENT_DO_NOT_WAIT))
            return rejection;
        if (busy && (flags & DXGI_PRESENT_DO_NOT_WAIT))
            return DXGI_ERROR_WAS_STILL_DRAWING;
        return answer;
    }
};

const UINT kCallerFlags[] = { 0,
                              DXGI_PRESENT_RESTART,
                              DXGI_PRESENT_ALLOW_TEARING,
                              DXGI_PRESENT_RESTART | DXGI_PRESENT_ALLOW_TEARING,
                              DXGI_PRESENT_DO_NOT_WAIT, // a caller that already asks for it
                              DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_ALLOW_TEARING };
const HRESULT kAnswers[] = { S_OK,
                             DXGI_STATUS_OCCLUDED,
                             DXGI_ERROR_WAS_STILL_DRAWING,
                             DXGI_ERROR_DEVICE_REMOVED,
                             DXGI_ERROR_DEVICE_RESET,
                             E_INVALIDARG,
                             DXGI_ERROR_INVALID_CALL,
                             E_FAIL };

// Key off: the chain sees the caller's flags and nothing else, once, with sync interval 0, and its answer comes back as it came.
void KeyOffIsTheCallersCall()
{
    ext::TestCfg().framePacing.hiddenPresentDoNotWait = false;
    for (UINT caller : kCallerFlags)
        for (HRESULT answer : kAnswers)
        {
            Policy policy;
            FakeChain chain;
            chain.answer = answer;
            const int notes = g_skippedNotes;
            const int warnings = g_warnLines;
            const unsigned reads = ext::CfgReads();
            PacingScope scope(Phase::HiddenPresent);
            HiddenHooks hooks { scope };
            const HRESULT hr = SeamPresent(policy, &chain, caller, hooks);
            CHECK(chain.calls.size() == 1);
            CHECK(chain.calls.size() == 1 && chain.calls[0].sync == 0);
            CHECK(chain.calls.size() == 1 && chain.calls[0].flags == caller);
            CHECK(hr == answer);
            CHECK(ext::CfgReads() == reads + 1); // the key is read once, whatever the answer
            CHECK(g_skippedNotes == notes && g_warnLines == warnings);
            CHECK(policy.Skipped() == 0 && !policy.Latched());
        }
}

// Key on: only DO_NOT_WAIT is added; a skip reads as S_OK, is counted by the trace, and nothing else changes.
void KeyOnAddsTheFlag()
{
    ext::TestCfg().framePacing.hiddenPresentDoNotWait = true;
    for (UINT caller : kCallerFlags)
    {
        Policy policy;
        FakeChain chain;
        PacingScope scope(Phase::HiddenPresent);
        HiddenHooks hooks { scope };
        const HRESULT hr = SeamPresent(policy, &chain, caller, hooks); // free chain
        CHECK(chain.calls.size() == 1);
        CHECK(chain.calls.size() == 1 && chain.calls[0].sync == 0);
        CHECK(chain.calls.size() == 1 && chain.calls[0].flags == (caller | DXGI_PRESENT_DO_NOT_WAIT));
        CHECK(hr == S_OK);
    }

    // busy chain: every flagged present is skipped, each one feeds the trace's counter, the caller sees S_OK
    {
        Policy policy;
        FakeChain chain;
        chain.busy = true;
        const int notes = g_skippedNotes;
        for (int i = 0; i < 5; ++i)
        {
            PacingScope scope(Phase::HiddenPresent);
            HiddenHooks hooks { scope };
            CHECK(SeamPresent(policy, &chain, 0, hooks) == S_OK);
        }
        CHECK(g_skippedNotes == notes + 5 && policy.Skipped() == 5);
    }
    // a caller that set DO_NOT_WAIT itself keeps its own still-drawing answer (a failure, as before the switch existed) and the
    // hook does not count it as its own skip
    {
        Policy policy;
        FakeChain chain;
        chain.busy = true;
        const int notes = g_skippedNotes;
        PacingScope scope(Phase::HiddenPresent);
        HiddenHooks hooks { scope };
        CHECK(SeamPresent(policy, &chain, DXGI_PRESENT_DO_NOT_WAIT, hooks) == DXGI_ERROR_WAS_STILL_DRAWING);
        CHECK(g_skippedNotes == notes && policy.Skipped() == 0);
    }
}

// The key is read on every present: switching it on and off again takes effect on the next present each time.
void TheKeyIsReadOnEveryPresent()
{
    Policy policy;
    FakeChain chain;
    const bool sequence[] = { false, true, true, false, true, false, false, true };
    const int count = int(sizeof(sequence) / sizeof(sequence[0]));
    const unsigned reads = ext::CfgReads();
    for (bool key : sequence)
    {
        ext::TestCfg().framePacing.hiddenPresentDoNotWait = key;
        PacingScope scope(Phase::HiddenPresent);
        HiddenHooks hooks { scope };
        SeamPresent(policy, &chain, DXGI_PRESENT_ALLOW_TEARING, hooks);
    }
    CHECK(chain.calls.size() == size_t(count));
    CHECK(ext::CfgReads() == reads + unsigned(count));
    for (int i = 0; i < count && i < int(chain.calls.size()); ++i)
    {
        const UINT expect = DXGI_PRESENT_ALLOW_TEARING | (sequence[i] ? DXGI_PRESENT_DO_NOT_WAIT : 0);
        CHECK(chain.calls[size_t(i)].flags == expect && chain.calls[size_t(i)].sync == 0);
    }
}

// A chain that refuses the flag: the rejected present comes back as a failure, the hook announces it once with the code, and the
// next presents go without the flag (the policy latches it off for the session).
void ARejectedFlagIsAnnouncedOnce()
{
    ext::TestCfg().framePacing.hiddenPresentDoNotWait = true;
    for (HRESULT rejection : { E_INVALIDARG, DXGI_ERROR_INVALID_CALL })
    {
        Policy policy;
        FakeChain chain;
        chain.rejectDoNotWait = true;
        chain.rejection = rejection;
        const int warnings = g_warnLines;
        {
            PacingScope scope(Phase::HiddenPresent);
            HiddenHooks hooks { scope };
            CHECK(SeamPresent(policy, &chain, 0, hooks) == rejection);
        }
        CHECK(policy.Latched());
        CHECK(g_warnLines == warnings + 1);
        CHECK(g_lastWarn.find("rejected DO_NOT_WAIT") != std::string::npos);
        CHECK(g_lastWarn.find(std::format("{:X}", static_cast<unsigned>(rejection))) != std::string::npos);
        for (int i = 0; i < 4; ++i)
        {
            PacingScope scope(Phase::HiddenPresent);
            HiddenHooks hooks { scope };
            CHECK(SeamPresent(policy, &chain, DXGI_PRESENT_RESTART, hooks) == S_OK);
        }
        CHECK(chain.calls.size() == 5);
        for (size_t i = 1; i < chain.calls.size(); ++i)
            CHECK(chain.calls[i].flags == DXGI_PRESENT_RESTART && chain.calls[i].sync == 0); // no DO_NOT_WAIT any more
        CHECK(g_warnLines == warnings + 1);                                                  // announced once
    }
}

// A long run of skips writes the hook's one line, with the run's length and the process-wide total in it, and only once per run.
void ASkipRunIsAnnouncedOnce()
{
    ext::TestCfg().framePacing.hiddenPresentDoNotWait = true;
    Policy& policy = HiddenPolicy(); // the one policy the entry hands the seam, so the total is the run's
    FakeChain chain;
    chain.busy = true;
    const int infos = g_infoLines;
    for (uint64_t i = 0; i < kEpisodeSkips; ++i)
    {
        PacingScope scope(Phase::HiddenPresent);
        HiddenHooks hooks { scope };
        SeamPresent(policy, &chain, 0, hooks);
    }
    CHECK(g_infoLines == infos + 1);
    CHECK(policy.Skipped() == kEpisodeSkips);
    CHECK(g_lastInfo.find(std::format("skipped {} presents in a row", kEpisodeSkips)) != std::string::npos);
    CHECK(g_lastInfo.find(std::format("{} skipped in total", policy.Skipped())) != std::string::npos);
    for (int i = 0; i < 300; ++i)
    {
        PacingScope scope(Phase::HiddenPresent);
        HiddenHooks hooks { scope };
        SeamPresent(policy, &chain, 0, hooks);
    }
    CHECK(g_infoLines == infos + 1); // one line per run of skips
}
} // namespace

int main()
{
    KeyOffIsTheCallersCall();
    KeyOnAddsTheFlag();
    TheKeyIsReadOnEveryPresent();
    ARejectedFlagIsAnnouncedOnce();
    ASkipRunIsAnnouncedOnce();
    if (g_failed != 0)
    {
        std::printf("hidden_present_hooks: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("hidden_present_hooks: all passed\n");
    return 0;
}
