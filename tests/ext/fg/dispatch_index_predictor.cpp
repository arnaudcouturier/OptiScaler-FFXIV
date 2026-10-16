// [UP] IFGFeature::GetIndexWillBeDispatched (stateless predictor) must name the same slot as the resync branch of
// IFGFeature::GetDispatchIndex (the consumer), for every combination of frame counters and per-index resources.
// docs/HUDLESS.md section 9.4: the S10 hud-less / UI tags are written to the slot the predictor names, and the provider's
// own Dispatch later asks the consumer; a predictor that only looked at Depth tagged one frame off whenever a frame carried
// only hud-less / UI resources.
//
// The two functions included below are the production text (sliced by run_dispatch_index_predictor.ps1 from
// framegen/IFGFeature.cpp), compiled against a small fake of the members they touch.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

#define LOG_DEBUG(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)

enum FG_ResourceType : uint32_t
{
    Depth = 0,
    Velocity,
    HudlessColor,
    UIColor,
    Distortion,
    Color,
    ResourceTypeCOUNT
};

struct State
{
    static State& Instance()
    {
        static State s;
        return s;
    }
    UINT64 fgLastFrame = 0;
};

struct IntOption
{
    int value = 1;
    int value_or_default() const { return value; }
};

struct Config
{
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
    IntOption FGAllowedFrameAhead;
};

#define BUFFER_COUNT 4

class IFGFeature
{
  public:
    UINT64 _frameCount = 0;
    UINT64 _lastDispatchedFrame = 0;
    UINT64 _lastFGFrame = 0;
    std::set<int> resources[BUFFER_COUNT];

    bool HasResource(FG_ResourceType type, int index = -1)
    {
        if (index < 0)
            index = GetIndex();
        return resources[index].count(type) != 0;
    }

    int GetIndex() { return (_frameCount % BUFFER_COUNT); }
    int GetIndexWillBeDispatched();
    int GetDispatchIndex(UINT64& willDispatchFrame);
};

#include "dispatch_index_production.inl"

namespace
{
int failures = 0;

void Check(bool ok, const char* what, UINT64 frame, UINT64 last, int allowed, unsigned mask)
{
    if (ok)
        return;
    ++failures;
    if (failures <= 12)
        std::printf("FAIL: %s (frameCount %llu, lastDispatched %llu, allowed %d, resources %#x)\n", what,
                    (unsigned long long) frame, (unsigned long long) last, allowed, mask);
}

const FG_ResourceType kTypes[4] = { FG_ResourceType::Depth, FG_ResourceType::Velocity, FG_ResourceType::UIColor,
                                    FG_ResourceType::HudlessColor };
} // namespace

int main()
{
    long long cases = 0;
    long long compared = 0;
    long long resyncOnly = 0; // cases in which the resync branch decides on the frame's own slot

    for (UINT64 frame = 1; frame <= 14; ++frame)
        for (int behind = 0; behind <= 9; ++behind)
            for (int allowed : { 1, 2, 3 })
                for (unsigned mask = 0; mask < 16; ++mask)
                    for (bool lastZero : { false, true })
                    {
                        IFGFeature fg;
                        fg._frameCount = frame;
                        fg._lastDispatchedFrame = lastZero ? 0 : (frame >= (UINT64) behind ? frame - behind : 0);
                        Config::Instance()->FGAllowedFrameAhead.value = allowed;
                        for (int t = 0; t < 4; ++t)
                            if (mask & (1u << t))
                                fg.resources[fg.GetIndex()].insert(kTypes[t]);
                        // resources of other indices must never matter
                        fg.resources[(fg.GetIndex() + 1) % BUFFER_COUNT].insert(FG_ResourceType::Depth);

                        ++cases;
                        const UINT64 lastBefore = fg._lastDispatchedFrame;
                        const int predicted = fg.GetIndexWillBeDispatched();
                        Check(fg._lastDispatchedFrame == lastBefore && fg._frameCount == frame,
                              "predictor changed state", frame, lastBefore, allowed, mask);
                        Check(predicted == fg.GetIndexWillBeDispatched(), "predictor not repeatable", frame, lastBefore,
                              allowed, mask);

                        UINT64 will = 0;
                        const int consumed = fg.GetDispatchIndex(will);
                        if (frame == lastBefore)
                        {
                            // the consumer refuses an already dispatched frame; nothing to compare
                            Check(consumed == -1 && fg._lastDispatchedFrame == lastBefore, "same-frame refusal", frame,
                                  lastBefore, allowed, mask);
                            continue;
                        }

                        ++compared;
                        Check(consumed == predicted, "predicted slot != consumed slot", frame, lastBefore, allowed,
                              mask);
                        Check(fg._lastDispatchedFrame == will && (will % BUFFER_COUNT) == (UINT64) consumed,
                              "consumer bookkeeping", frame, lastBefore, allowed, mask);
                        if (will == frame && frame != lastBefore + 1)
                            ++resyncOnly;
                    }

    // The reported bug, spelled out: a frame that carries only a hud-less image, far behind the last dispatch.
    {
        IFGFeature fg;
        fg._frameCount = 10;
        fg._lastDispatchedFrame = 0;
        fg.resources[fg.GetIndex()].insert(FG_ResourceType::HudlessColor);
        UINT64 will = 0;
        Check(fg.GetIndexWillBeDispatched() == 10 % BUFFER_COUNT,
              "hud-less-only frame must resync to the frame's own slot", 10, 0, 1, 1);
        Check(fg.GetDispatchIndex(will) == 10 % BUFFER_COUNT && will == 10, "consumer resync (hud-less only)", 10, 0, 1,
              1);
    }

    if (failures != 0)
    {
        std::printf("FAILED: %d mismatches in %lld cases\n", failures, cases);
        return 1;
    }
    if (compared < 1000 || resyncOnly < 100)
    {
        std::printf("FAILED: the table did not exercise the resync branch (%lld compared, %lld resyncs)\n", compared,
                    resyncOnly);
        return 1;
    }
    std::printf("PASS: predictor == consumer slot in %lld cases (%lld of them resync to the frame's own slot), "
                "predictor stateless\n",
                compared, resyncOnly);
    return 0;
}
