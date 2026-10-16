// IFgMarkers: pure decision math behind the FFXIV dynamic-marker provider's Enabled() gate
// (docs/SEAMS-catalog.md "game", IFgMarkers; the oracle's FfxivDynamicMarkers::Enabled(), misc/FfxivDynamicMarkers.h
// at the pinned ref-nightly-merge SHA, minus its `gameExe == "ffxiv_dx11.exe"` check -- see
// ext/ffxiv/markers/FfxivMarkersEnabled.h's doc comment for why dropping it is correct here). No upstream types,
// so this is a standalone unit test: no NVNGX, no Config, no link against the product -- same split as
// ext/upscaler/ForcedQuality.h / RenderGeometry.h.
#include "ext/ffxiv/markers/FfxivMarkersEnabled.h"

#include <cassert>
#include <cstdio>

using namespace ext::ffxiv::markers;

namespace
{
// Every condition in its "feature is live" state: isShuttingDown and gameIsSendingMarkers are the two the oracle
// inverts (!isShuttingDown, !gameIsSendingMarkers), so their live value is false; every other argument's live
// value is true. Named arguments (rather than positional bools) so each flip below is unambiguous.
struct Inputs
{
    bool fgEnabled = true;
    bool forceDmfg = true;
    bool isUpscalerInput = true;
    bool isDlssgOutput = true;
    bool isShuttingDown = false;
    bool gameIsSendingMarkers = false;
    bool isD3D12Inited = true;
    bool hasFrameTokenFn = true;
    bool hasSetMarkerFn = true;
};

bool Eval(const Inputs& in)
{
    return MarkersEnabledValue(in.fgEnabled, in.forceDmfg, in.isUpscalerInput, in.isDlssgOutput, in.isShuttingDown,
                               in.gameIsSendingMarkers, in.isD3D12Inited, in.hasFrameTokenFn, in.hasSetMarkerFn);
}
} // namespace

int main()
{
    // Every condition in its live state: enabled.
    assert(Eval(Inputs {}));

    // Each of the seven plain AND conditions flipped to false, one at a time, must disable the feature.
    {
        Inputs in;
        in.fgEnabled = false;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.forceDmfg = false;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.isUpscalerInput = false;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.isDlssgOutput = false;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.isD3D12Inited = false;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.hasFrameTokenFn = false;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.hasSetMarkerFn = false;
        assert(!Eval(in));
    }

    // The two inverted conditions: becoming true must disable the feature.
    {
        Inputs in;
        in.isShuttingDown = true;
        assert(!Eval(in));
    }
    {
        Inputs in;
        in.gameIsSendingMarkers = true;
        assert(!Eval(in));
    }

    // Two conditions false together must not accidentally re-enable the feature (AND, not XOR).
    {
        Inputs in;
        in.fgEnabled = false;
        in.forceDmfg = false;
        assert(!Eval(in));
    }

    std::puts("FfxivMarkers: Enabled() decision math passed");
    return 0;
}
