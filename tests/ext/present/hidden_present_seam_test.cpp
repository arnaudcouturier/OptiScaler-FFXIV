// [PACE] The body of the hidden-present seam (hidden::SeamPresent in ext/present/HiddenPresentPolicy.h, the code
// ext::present::HiddenPresent in FramePacingTrace.cpp runs on every hidden present) against a fake swapchain. The upstream call site is
// pinned by frame_pacing_upstream_pins and the pure policy by hidden_present_policy; this test covers what sits between them:
//   key off  -> exactly one swapchain->Present(0, callerFlags), and the swapchain's own answer comes back untouched
//   key on   -> exactly one Present(0, callerFlags | DO_NOT_WAIT); a busy chain reads as S_OK and is counted, nothing else changes
// The key is read once per present (a change in the menu or the ini takes effect on the next frame).
#include "ext/present/HiddenPresentPolicy.h"

#include <cstdint>
#include <cstdio>
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

using namespace ext::present::hidden;

namespace
{
// Same shape as IDXGISwapChain::Present(UINT SyncInterval, UINT Flags): records every call, answers like a real chain.
struct FakeChain
{
    struct Call
    {
        UINT sync;
        UINT flags;
    };
    std::vector<Call> calls;
    HRESULT answer = S_OK;         // the answer to every present
    bool busy = false;             // a DO_NOT_WAIT present answers WAS_STILL_DRAWING (a plain one would block)
    bool rejectDoNotWait = false;  // a DO_NOT_WAIT present answers this chain's `rejection`
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

// What HiddenPresent's hooks do in the game (key read, trace counter, log lines), as counters.
struct FakeHooks
{
    bool key = false;
    int keyReads = 0;
    int reports = 0;
    int skipped = 0;
    int latched = 0;
    int episodes = 0;
    HRESULT lastHr = S_OK;

    bool KeyOn()
    {
        ++keyReads;
        return key;
    }
    void OnReport(const Report& r, HRESULT hr)
    {
        ++reports;
        skipped += r.outcome == Outcome::Skipped ? 1 : 0;
        latched += r.latchedNow ? 1 : 0;
        episodes += r.episodeStarted ? 1 : 0;
        lastHr = hr;
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

// Key off: today's call, bit for bit, whatever the flags and whatever the chain says.
void KeyOffIsTheIdenticalCall()
{
    for (UINT caller : kCallerFlags)
        for (HRESULT answer : kAnswers)
        {
            Policy policy;
            FakeChain chain;
            chain.answer = answer;
            FakeHooks hooks; // key off
            const HRESULT hr = SeamPresent(policy, &chain, caller, hooks);
            CHECK(chain.calls.size() == 1);
            CHECK(chain.calls.size() == 1 && chain.calls[0].sync == 0); // sync interval 0, as upstream had it
            CHECK(chain.calls.size() == 1 && chain.calls[0].flags == caller);
            CHECK(hr == answer); // the answer comes back as it came, even WAS_STILL_DRAWING
            CHECK(hooks.keyReads == 1 && hooks.reports == 1);
            CHECK(hooks.skipped == 0 && hooks.latched == 0 && hooks.episodes == 0);
            CHECK(policy.Skipped() == 0 && !policy.Latched());
        }
}

// Key on: only DO_NOT_WAIT is added; a skip reads as S_OK and is counted; every other answer is returned as it came.
void KeyOnAddsOnlyTheFlag()
{
    for (UINT caller : kCallerFlags)
    {
        Policy policy;
        FakeChain chain;
        FakeHooks hooks;
        hooks.key = true;
        const HRESULT hr = SeamPresent(policy, &chain, caller, hooks); // free chain
        CHECK(chain.calls.size() == 1);
        CHECK(chain.calls.size() == 1 && chain.calls[0].sync == 0);
        CHECK(chain.calls.size() == 1 && chain.calls[0].flags == (caller | DXGI_PRESENT_DO_NOT_WAIT));
        CHECK(hr == S_OK && hooks.skipped == 0);
    }

    // busy chain: the present we flagged is skipped, the caller sees S_OK
    {
        Policy policy;
        FakeChain chain;
        chain.busy = true;
        FakeHooks hooks;
        hooks.key = true;
        for (int i = 0; i < 10; ++i)
            CHECK(SeamPresent(policy, &chain, 0, hooks) == S_OK);
        CHECK(hooks.skipped == 10 && policy.Skipped() == 10);
        CHECK(hooks.lastHr == S_OK);
    }
    // a caller that set DO_NOT_WAIT itself keeps its own still-drawing answer (a failure, as before the switch existed)
    {
        Policy policy;
        FakeChain chain;
        chain.busy = true;
        FakeHooks hooks;
        hooks.key = true;
        CHECK(SeamPresent(policy, &chain, DXGI_PRESENT_DO_NOT_WAIT, hooks) == DXGI_ERROR_WAS_STILL_DRAWING);
        CHECK(hooks.skipped == 0 && policy.Skipped() == 0);
    }
    // real failures and success codes come back unchanged
    for (HRESULT answer : { DXGI_STATUS_OCCLUDED, DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, E_FAIL, E_OUTOFMEMORY })
    {
        Policy policy;
        FakeChain chain;
        chain.answer = answer;
        FakeHooks hooks;
        hooks.key = true;
        CHECK(SeamPresent(policy, &chain, 0, hooks) == answer);
        CHECK(hooks.skipped == 0 && hooks.latched == 0);
    }
}

// The key is read on every present, so switching it takes effect on the next frame and back.
void KeyIsReadEveryPresent()
{
    Policy policy;
    FakeChain chain;
    FakeHooks hooks;
    const bool sequence[] = { false, true, true, false, true, false, false };
    for (bool key : sequence)
    {
        hooks.key = key;
        SeamPresent(policy, &chain, DXGI_PRESENT_ALLOW_TEARING, hooks);
    }
    CHECK(hooks.keyReads == 7 && hooks.reports == 7 && chain.calls.size() == 7);
    for (size_t i = 0; i < chain.calls.size(); ++i)
    {
        const UINT expect = DXGI_PRESENT_ALLOW_TEARING | (sequence[i] ? DXGI_PRESENT_DO_NOT_WAIT : 0);
        CHECK(chain.calls[i].flags == expect && chain.calls[i].sync == 0);
    }
}

// A chain that refuses the flag: reported once, the same present is not retried, and the next presents go without the flag.
void RejectionLatchesThroughTheSeam()
{
    for (HRESULT rejection : { E_INVALIDARG, DXGI_ERROR_INVALID_CALL }) // INVALID_CALL also latches: the safe direction (see the policy header)
    {
        Policy policy;
        FakeChain chain;
        chain.rejectDoNotWait = true;
        chain.rejection = rejection;
        FakeHooks hooks;
        hooks.key = true;
        CHECK(SeamPresent(policy, &chain, 0, hooks) == rejection); // the rejected present itself comes back as a failure
        CHECK(chain.calls.size() == 1 && hooks.latched == 1 && policy.Latched());
        for (int i = 0; i < 5; ++i)
            CHECK(SeamPresent(policy, &chain, DXGI_PRESENT_RESTART, hooks) == S_OK);
        CHECK(chain.calls.size() == 6);
        for (size_t i = 1; i < chain.calls.size(); ++i)
            CHECK(chain.calls[i].flags == DXGI_PRESENT_RESTART && chain.calls[i].sync == 0); // no DO_NOT_WAIT any more
        CHECK(hooks.latched == 1); // reported once
    }
}

// 120 skips in a row start one episode; a free present in between re-arms it.
void EpisodeThroughTheSeam()
{
    Policy policy;
    FakeChain chain;
    chain.busy = true;
    FakeHooks hooks;
    hooks.key = true;
    for (int i = 0; i < 119; ++i)
        SeamPresent(policy, &chain, 0, hooks);
    CHECK(hooks.episodes == 0);
    SeamPresent(policy, &chain, 0, hooks);
    CHECK(hooks.episodes == 1);
    for (int i = 0; i < 300; ++i)
        SeamPresent(policy, &chain, 0, hooks);
    CHECK(hooks.episodes == 1); // one line per run of skips
    chain.busy = false;
    SeamPresent(policy, &chain, 0, hooks);
    chain.busy = true;
    for (int i = 0; i < 120; ++i)
        SeamPresent(policy, &chain, 0, hooks);
    CHECK(hooks.episodes == 2);
}
} // namespace

int main()
{
    KeyOffIsTheIdenticalCall();
    KeyOnAddsOnlyTheFlag();
    KeyIsReadEveryPresent();
    RejectionLatchesThroughTheSeam();
    EpisodeThroughTheSeam();
    if (g_failed != 0)
    {
        std::printf("hidden_present_seam: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("hidden_present_seam: all passed\n");
    return 0;
}
