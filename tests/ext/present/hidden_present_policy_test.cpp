// [PACE] Pure policy of the hidden DX11 present (ext/present/HiddenPresentPolicy.h): which flags the present gets, how its result is
// classified, the latch that switches DO_NOT_WAIT off for the session when DXGI rejects it, and the "many skips in a row" episode.
// A fake swapchain stands in for DXGI: a busy chain blocks a plain present and refuses a DO_NOT_WAIT one.
#include "ext/present/HiddenPresentPolicy.h"

#include <cstdint>
#include <cstdio>

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
// A hidden swapchain whose queue is full (busy) or free, with the answer a real DXGI chain gives.
struct FakeChain
{
    bool busy = false;
    HRESULT forced = S_OK;      // when not S_OK: the answer to every present (device removed, invalid argument ...)
    bool rejectDoNotWait = false; // answers E_INVALIDARG to a present that carries DO_NOT_WAIT
    int blockingCalls = 0;       // presents that would have waited for the busy chain
    int calls = 0;
    UINT lastFlags = 0;

    HRESULT Present(UINT flags)
    {
        ++calls;
        lastFlags = flags;
        if (rejectDoNotWait && (flags & DXGI_PRESENT_DO_NOT_WAIT))
            return E_INVALIDARG;
        if (forced != S_OK)
            return forced;
        if (busy)
        {
            if (flags & DXGI_PRESENT_DO_NOT_WAIT)
                return DXGI_ERROR_WAS_STILL_DRAWING;
            ++blockingCalls;
        }
        return S_OK;
    }
};

// What the upstream call site does with the result: one warning per FAILED result.
struct Site
{
    int warnings = 0;
    HRESULT Run(Policy& policy, bool keyOn, FakeChain& chain, UINT callerFlags, Report* report = nullptr)
    {
        const HRESULT hr = Present(policy, keyOn, callerFlags, [&](UINT f) { return chain.Present(f); }, report);
        if (FAILED(hr))
            ++warnings;
        return hr;
    }
};

void FlagsKeepCallerBits()
{
    Policy p;
    const UINT caller = DXGI_PRESENT_RESTART | DXGI_PRESENT_ALLOW_TEARING;
    CHECK(p.NextFlags(caller, false) == caller);
    CHECK(p.NextFlags(caller, true) == (caller | DXGI_PRESENT_DO_NOT_WAIT));
    CHECK(p.NextFlags(0, true) == DXGI_PRESENT_DO_NOT_WAIT);
    CHECK(p.NextFlags(0, false) == 0);
    // a caller that already set the bit keeps it, key on or off
    CHECK(p.NextFlags(DXGI_PRESENT_DO_NOT_WAIT, false) == DXGI_PRESENT_DO_NOT_WAIT);
    CHECK(p.NextFlags(DXGI_PRESENT_DO_NOT_WAIT, true) == DXGI_PRESENT_DO_NOT_WAIT);
}

void ClassifyTable()
{
    CHECK(Classify(S_OK, false) == Outcome::Presented);
    CHECK(Classify(S_OK, true) == Outcome::Presented);
    CHECK(Classify(DXGI_STATUS_OCCLUDED, true) == Outcome::Presented); // a success code
    CHECK(Classify(DXGI_ERROR_WAS_STILL_DRAWING, true) == Outcome::Skipped);
    // without our flag a still-drawing answer is not ours to swallow: it stays a failure, as today
    CHECK(Classify(DXGI_ERROR_WAS_STILL_DRAWING, false) == Outcome::Failed);
    CHECK(Classify(DXGI_ERROR_DEVICE_REMOVED, true) == Outcome::Failed);
    CHECK(Classify(DXGI_ERROR_DEVICE_REMOVED, false) == Outcome::Failed);
    CHECK(Classify(DXGI_ERROR_DEVICE_RESET, true) == Outcome::Failed);
    CHECK(Classify(E_INVALIDARG, true) == Outcome::Failed);
    CHECK(Classify(DXGI_ERROR_INVALID_CALL, true) == Outcome::Failed);
    CHECK(Classify(E_FAIL, false) == Outcome::Failed);
}

void BusyChainWithFlagNeverBlocks()
{
    Policy p;
    FakeChain chain;
    chain.busy = true;
    Site site;
    for (int i = 0; i < 1000; ++i)
    {
        const HRESULT hr = site.Run(p, true, chain, 0);
        CHECK(hr == S_OK); // the skip is invisible to the caller: no warning
    }
    CHECK(chain.calls == 1000);
    CHECK(chain.blockingCalls == 0);
    CHECK(site.warnings == 0);
    CHECK(p.Skipped() == 1000);
    CHECK(!p.Latched());
}

void BusyChainWithKeyOffKeepsTodaysBehaviour()
{
    Policy p;
    FakeChain chain;
    chain.busy = true;
    Site site;
    for (int i = 0; i < 100; ++i)
        CHECK(site.Run(p, false, chain, 0) == S_OK);
    CHECK(chain.blockingCalls == 100);
    CHECK(chain.lastFlags == 0);
    CHECK(site.warnings == 0);
    CHECK(p.Skipped() == 0);
}

void KeyOffWithCallersFlagStillWarns()
{
    // the caller asked for DO_NOT_WAIT itself: the old code warned on WAS_STILL_DRAWING, and so it must now
    Policy p;
    FakeChain chain;
    chain.busy = true;
    Site site;
    const HRESULT hr = site.Run(p, false, chain, DXGI_PRESENT_DO_NOT_WAIT);
    CHECK(hr == DXGI_ERROR_WAS_STILL_DRAWING);
    CHECK(site.warnings == 1);
    CHECK(p.Skipped() == 0);
    // and with the key on the answer is still the caller's own, not ours
    const HRESULT hr2 = site.Run(p, true, chain, DXGI_PRESENT_DO_NOT_WAIT);
    CHECK(hr2 == DXGI_ERROR_WAS_STILL_DRAWING);
    CHECK(p.Skipped() == 0);
}

void FreeChainPresentsNormally()
{
    Policy p;
    FakeChain chain;
    Site site;
    CHECK(site.Run(p, true, chain, DXGI_PRESENT_ALLOW_TEARING) == S_OK);
    CHECK(chain.lastFlags == (DXGI_PRESENT_ALLOW_TEARING | DXGI_PRESENT_DO_NOT_WAIT));
    CHECK(p.Skipped() == 0);
    CHECK(site.warnings == 0);
}

void RealFailuresStillWarn()
{
    for (HRESULT failure : { DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, E_FAIL, E_OUTOFMEMORY })
    {
        Policy p;
        FakeChain chain;
        chain.forced = failure;
        Site site;
        CHECK(site.Run(p, true, chain, 0) == failure); // returned unchanged
        CHECK(site.warnings == 1);
        CHECK(p.Skipped() == 0);
        CHECK(!p.Latched()); // only an argument error latches
    }
}

void InvalidArgumentLatchesOff()
{
    {
        Policy p;
        FakeChain chain;
        chain.rejectDoNotWait = true;
        Site site;
        Report report;
        // the rejected present itself fails (reported as before) and latches the flag off
        const HRESULT first = site.Run(p, true, chain, 0, &report);
        CHECK(first == E_INVALIDARG);
        CHECK(report.latchedNow);
        CHECK(p.Latched());
        CHECK(site.warnings == 1);
        // from then on the flag is never sent again, key on or off, and the present goes through
        for (int i = 0; i < 5; ++i)
        {
            Report next;
            CHECK(site.Run(p, true, chain, DXGI_PRESENT_RESTART, &next) == S_OK);
            CHECK(chain.lastFlags == DXGI_PRESENT_RESTART);
            CHECK(!next.latchedNow); // reported once
        }
        CHECK(site.warnings == 1);
    }

    // DXGI_ERROR_INVALID_CALL as the rejection
    {
        Policy p;
        const HRESULT hr = DXGI_ERROR_INVALID_CALL;
        Report r;
        const HRESULT out = Present(p, true, 0, [&](UINT f) { return (f & DXGI_PRESENT_DO_NOT_WAIT) ? hr : S_OK; }, &r);
        CHECK(out == hr && r.latchedNow && p.Latched());
    }

    // an invalid-argument answer to a present WITHOUT our flag does not latch (it is not about DO_NOT_WAIT)
    {
        Policy p;
        Report r;
        const HRESULT out = Present(p, false, 0, [&](UINT) { return E_INVALIDARG; }, &r);
        CHECK(out == E_INVALIDARG && !r.latchedNow && !p.Latched());
        // nor when the caller already carried the bit and the key is on (the bit was not ours)
        const HRESULT out2 = Present(p, true, DXGI_PRESENT_DO_NOT_WAIT, [&](UINT) { return E_INVALIDARG; }, &r);
        CHECK(out2 == E_INVALIDARG && !p.Latched());
    }
}

void EpisodeLogFiresOnceAtThreshold()
{
    Policy p;
    FakeChain chain;
    chain.busy = true;
    Site site;
    int episodes = 0;
    for (int i = 1; i <= 119; ++i)
    {
        Report r;
        site.Run(p, true, chain, 0, &r);
        episodes += r.episodeStarted ? 1 : 0;
    }
    CHECK(episodes == 0);
    {
        Report r;
        site.Run(p, true, chain, 0, &r); // the 120th in a row
        CHECK(r.episodeStarted);
        CHECK(r.consecutive == 120);
        episodes += 1;
    }
    for (int i = 0; i < 500; ++i)
    {
        Report r;
        site.Run(p, true, chain, 0, &r);
        episodes += r.episodeStarted ? 1 : 0; // the same episode: no second line
    }
    CHECK(episodes == 1);

    // an accepted present ends the episode and re-arms it
    chain.busy = false;
    {
        Report r;
        CHECK(site.Run(p, true, chain, 0, &r) == S_OK);
        CHECK(!r.episodeStarted);
        CHECK(r.consecutive == 0);
    }
    chain.busy = true;
    for (int i = 1; i <= 120; ++i)
    {
        Report r;
        site.Run(p, true, chain, 0, &r);
        if (i < 120)
            CHECK(!r.episodeStarted);
        else
            CHECK(r.episodeStarted);
    }
    CHECK(p.Skipped() == 120 + 500 + 120);
}

void ShortRunsNeverLog()
{
    // 119 skips, one present, 119 skips, one present ...: the run never reaches the threshold
    Policy p;
    FakeChain chain;
    Site site;
    int episodes = 0;
    for (int round = 0; round < 10; ++round)
    {
        chain.busy = true;
        for (int i = 0; i < 119; ++i)
        {
            Report r;
            site.Run(p, true, chain, 0, &r);
            episodes += r.episodeStarted ? 1 : 0;
        }
        chain.busy = false;
        site.Run(p, true, chain, 0);
    }
    CHECK(episodes == 0);
    CHECK(p.Skipped() == 1190);
}

void FailureEndsTheEpisodeToo()
{
    // a failed present is not a skip: it interrupts the run
    Policy p;
    FakeChain chain;
    chain.busy = true;
    Site site;
    for (int i = 0; i < 100; ++i)
        site.Run(p, true, chain, 0);
    chain.forced = DXGI_ERROR_DEVICE_REMOVED;
    site.Run(p, true, chain, 0);
    chain.forced = S_OK;
    int episodes = 0;
    for (int i = 0; i < 119; ++i)
    {
        Report r;
        site.Run(p, true, chain, 0, &r);
        episodes += r.episodeStarted ? 1 : 0;
    }
    CHECK(episodes == 0);
}
} // namespace

int main()
{
    FlagsKeepCallerBits();
    ClassifyTable();
    BusyChainWithFlagNeverBlocks();
    BusyChainWithKeyOffKeepsTodaysBehaviour();
    KeyOffWithCallersFlagStillWarns();
    FreeChainPresentsNormally();
    RealFailuresStillWarn();
    InvalidArgumentLatchesOff();
    EpisodeLogFiresOnceAtThreshold();
    ShortRunsNeverLog();
    FailureEndsTheEpisodeToo();
    if (g_failed != 0)
    {
        std::printf("hidden_present_policy: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("hidden_present_policy: all passed\n");
    return 0;
}
