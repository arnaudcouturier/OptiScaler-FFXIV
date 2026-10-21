#pragma once
// Pure policy of the hidden DX11 present (ext::present::HiddenPresent, ext/present/FramePacingTrace.cpp): the auxiliary
// IDXGISwapChain::Present(0, flags) the DX11 bridge makes on its hidden swapchain must not be able to block the thread that drives
// the visible frame-generation output. With [FramePacing] HiddenPresentDoNotWait on, DXGI_PRESENT_DO_NOT_WAIT is added to the flags;
// a chain that is still busy then answers DXGI_ERROR_WAS_STILL_DRAWING and that present is skipped (no retry, no warning).
// Everything else is untouched: other flags are kept, any other failure is returned as it came and still warns at the call site.
// Unit-testable without DXGI (tests/ext/present/hidden_present_policy_test.cpp): the swapchain call is a callable.

#include <dxgi.h> // HRESULT, DXGI_PRESENT_*, DXGI_ERROR_*

#include <atomic>
#include <cstdint>
#include <utility>

namespace ext::present::hidden
{
enum class Outcome
{
    Presented, // a success code (S_OK, DXGI_STATUS_OCCLUDED ...)
    Skipped,   // DXGI_ERROR_WAS_STILL_DRAWING answering the DO_NOT_WAIT flag THIS policy added
    Failed     // anything else
};

// `flagAdded`: the DO_NOT_WAIT bit was added by this policy (the caller did not have it). A still-drawing answer to a flag the caller
// set itself is the caller's own result and stays a failure, exactly as before the switch existed.
inline Outcome Classify(HRESULT hr, bool flagAdded)
{
    if (flagAdded && hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return Outcome::Skipped;
    return SUCCEEDED(hr) ? Outcome::Presented : Outcome::Failed;
}

// The two answers that mean "this chain does not accept the flag".
inline bool RejectsDoNotWait(HRESULT hr) { return hr == E_INVALIDARG || hr == DXGI_ERROR_INVALID_CALL; }

// Skips in a row that make one log line ("the hidden chain has been busy for a long time").
inline constexpr uint64_t kEpisodeSkips = 120;

// What one present told the caller, for logging and for the trace.
struct Report
{
    Outcome outcome = Outcome::Presented;
    bool flagAdded = false;      // the present carried a DO_NOT_WAIT bit this policy added
    bool latchedNow = false;     // DXGI rejected the flag on THIS present: switched off for the session
    bool episodeStarted = false; // this skip is the kEpisodeSkips-th in a row (once per run of skips)
    uint64_t consecutive = 0;    // skips in a row after this present
};

class Policy
{
  public:
    // The flags for the next present: the caller's bits, plus DO_NOT_WAIT when the key is on and DXGI has not rejected it.
    uint32_t NextFlags(uint32_t flags, bool keyOn) const
    {
        return keyOn && !latched_.load(std::memory_order_relaxed) ? (flags | DXGI_PRESENT_DO_NOT_WAIT) : flags;
    }

    // Updates the counters from the answer. `flagAdded` as in Classify.
    Report Observe(HRESULT hr, bool flagAdded)
    {
        Report r;
        r.flagAdded = flagAdded;
        r.outcome = Classify(hr, flagAdded);
        if (r.outcome == Outcome::Skipped)
        {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            r.consecutive = consecutive_.fetch_add(1, std::memory_order_relaxed) + 1;
            r.episodeStarted = r.consecutive == kEpisodeSkips;
            return r;
        }
        consecutive_.store(0, std::memory_order_relaxed);
        if (flagAdded && RejectsDoNotWait(hr) && !latched_.exchange(true, std::memory_order_relaxed))
            r.latchedNow = true;
        return r;
    }

    uint64_t Skipped() const { return skipped_.load(std::memory_order_relaxed); }
    bool Latched() const { return latched_.load(std::memory_order_relaxed); }

  private:
    std::atomic<uint64_t> skipped_ { 0 };
    std::atomic<uint64_t> consecutive_ { 0 };
    std::atomic<bool> latched_ { false };
};

// One hidden present through the policy. `present(flags)` makes the real call and returns its HRESULT. Returns what the call site
// should see: S_OK for a skip (so its failure warning stays quiet), the answer unchanged for everything else.
template <class PresentFn>
HRESULT Present(Policy& policy, bool keyOn, uint32_t callerFlags, PresentFn&& present, Report* report = nullptr)
{
    const uint32_t flags = policy.NextFlags(callerFlags, keyOn);
    const bool flagAdded = (flags & DXGI_PRESENT_DO_NOT_WAIT) != 0 && (callerFlags & DXGI_PRESENT_DO_NOT_WAIT) == 0;
    const HRESULT hr = std::forward<PresentFn>(present)(flags);
    const Report r = policy.Observe(hr, flagAdded);
    if (report != nullptr)
        *report = r;
    return r.outcome == Outcome::Skipped ? S_OK : hr;
}
} // namespace ext::present::hidden
