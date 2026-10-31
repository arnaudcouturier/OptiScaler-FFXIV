// The bridge side of the live output switch (ext/fg/outswitch/BridgeSwap.h) and the per-chain record its replay reads
// (ext/dx11bridge/SwapRecord.h). The real accessors name Direct3D so they cannot run here; the rules they lean on can:
// a seam with no swap composed answers exactly what the bridge answered before the seam existed, a composed one runs the
// switch's own point, and the colour space / resize generation decide the replay.
#include "ext/dx11bridge/SwapRecord.h"
#include "ext/fg/outswitch/BridgeSwap.h"

#include <cstdint>
#include <cstdio>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("  FAIL line %d: %s\n", __LINE__, #cond);                                                      \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
// The switch point a composed swap runs: the test's stand-in for WP5's Ops, recording what the seam passed it.
struct Engine
{
    int detachedCalls = 0;
    int fgCalls = 0;
    bool realChainMissing = false;
    uint32_t presentStartMs = 0;
    uint32_t lastResult = 0;
    uint32_t detachedAnswer = kOk;  // the switch answers S_OK while it owns the gap
    uint32_t fgAnswer = kOk;        // probation masks the failure it is about to roll back
};

Engine* g_engine = nullptr;

uint32_t Detached(BridgeSwap& swap, bool realChainMissing, uint64_t presentStartMs)
{
    (void)swap;
    ++g_engine->detachedCalls;
    g_engine->realChainMissing = realChainMissing;
    g_engine->presentStartMs = presentStartMs;
    return g_engine->detachedAnswer;
}

uint32_t FgResult(BridgeSwap& swap, uint32_t result, uint64_t presentMs)
{
    (void)swap;
    (void)presentMs;
    ++g_engine->fgCalls;
    g_engine->lastResult = result;
    return g_engine->fgAnswer;
}

void Arm(Engine& engine, BridgeSwap& swap)
{
    g_engine = &engine;
    swap.detached = &Detached;
    swap.fgResult = &FgResult;
}
} // namespace

int main()
{
    std::printf("  the two present seams answer the bridge's own value while no switch is composed\n");
    // The early out answered DXGI_ERROR_DEVICE_REMOVED and the present returned the FG chain's result: with no swap, or a
    // swap the composition has not armed, both seams must answer exactly that.
    CHECK(kDeviceRemoved == 0x887A0005u);
    CHECK(DetachedAnswer(nullptr, false, kDeviceRemoved) == kDeviceRemoved);
    CHECK(DetachedAnswer(nullptr, true, kDeviceRemoved) == kDeviceRemoved);
    BridgeSwap idle;
    CHECK(DetachedAnswer(&idle, true, kDeviceRemoved) == kDeviceRemoved); // not armed
    CHECK(FgResultAnswer(&idle, 0x8007000Eu) == 0x8007000Eu);             // not armed: the game sees the failure
    CHECK(DetachedAnswer(nullptr, true, kOk) == kOk); // the fallback is the caller's line, never an invented answer
    std::printf("    [ok] an unattached seam gives the line its own value back\n");

    std::printf("  an armed swap runs the switch's point and reports its answer\n");
    Engine engine;
    BridgeSwap armed;
    armed.presentStartMs = 4321;
    Arm(engine, armed);
    CHECK(DetachedAnswer(&armed, false, kDeviceRemoved) == kOk); // the switch says S_OK inside its own window
    CHECK(engine.detachedCalls == 1);
    CHECK(!engine.realChainMissing);
    CHECK(engine.presentStartMs == 4321); // the present thread's start travels with the call
    CHECK(DetachedAnswer(&armed, true, kDeviceRemoved) == kOk);
    CHECK(engine.detachedCalls == 2);
    CHECK(engine.realChainMissing); // the bridge really had no chain: the switch still answers, it owns the gap
    const uint32_t failed = 0x887A0005u;
    engine.fgAnswer = kOk; // probation masks a failure it is about to roll back
    CHECK(FgResultAnswer(&armed, failed) == kOk);
    CHECK(engine.fgCalls == 1);
    CHECK(engine.lastResult == failed); // the switch saw the real result before masking it
    engine.fgAnswer = failed;           // a device loss is never masked
    CHECK(FgResultAnswer(&armed, failed) == failed);
    CHECK(engine.fgCalls == 2);
    std::printf("    [ok] the armed seams run the switch point, with the present's start and its result\n");

    std::printf("  the colour space is replayed unless it is the default, and only when it was recorded\n");
    CHECK(!ext::dx11bridge::ColorSpaceNeedsReplay(false, 9)); // never recorded: the new chain keeps its own
    CHECK(!ext::dx11bridge::ColorSpaceNeedsReplay(true, 0));  // the default: a fresh chain already has it
    CHECK(ext::dx11bridge::ColorSpaceNeedsReplay(true, 9));   // scRGB
    CHECK(ext::dx11bridge::ColorSpaceNeedsReplay(true, 12));  // scRGB linear
    CHECK(!ext::dx11bridge::ResizeCrossed(4, 4));             // no resize across the switch
    CHECK(ext::dx11bridge::ResizeCrossed(4, 5));              // one ran: the new chain is resized to the window again
    CHECK(ext::dx11bridge::kColorSpaceDefault == 0);
    std::printf("    [ok] the record decides the replay, and a crossed resize is re-issued\n");

    std::printf("  the swap starts idle and has no switch point of its own\n");
    BridgeSwap fresh;
    CHECK(fresh.detached == nullptr && fresh.fgResult == nullptr); // the composition installs both
    CHECK(!fresh.colorSpaceRecorded);
    CHECK(fresh.presentStartMs == 0);
    std::printf("    [ok] the seams are inert until both are installed\n");

    if (g_failed == 0)
        std::printf("ALL BRIDGE SWAP SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
