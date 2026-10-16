#include "pch.h"

#include "ext/ffxiv/markers/FfxivMarkers.h"
#include "ext/ffxiv/markers/FfxivMarkersEnabled.h"
#include "ext/game/GameInterfaces.h"
#include "ext/nr/feature/DlssNrFeature_Dx12.h"

#include <hooks/Reflex_Hooks.h>
#include <proxies/Streamline_Proxy.h>

#include <mutex>

// D2, C5: the FFXIV implementation of ext::game::IFgMarkers -- the oracle's misc/FfxivDynamicMarkers.h
// (simulation/render interval markers over Streamline PCL, driving its "dynamic MFG" path when the game isn't
// sending its own Reflex markers). The oracle wraps each sl::PCLSetMarker call in its own inline MarkerScope; this
// fork uses its existing ScopedOptiScalerReflex (hooks/Reflex_Hooks.h) instead -- the same primitive
// DLSSG_Common.cpp/DLSSG_Vk.cpp/D3D12_Hooks.cpp/Reflex_Hooks.cpp already use to mark OptiScaler's own Reflex calls
// so upstream's Reflex input (which already recognises calls made under it as "FromOptiScaler") doesn't need a
// second mechanism.
//
// Mutex-guarded simulation/render frame state is a class member here (one singleton instance, `provider` below)
// where the oracle used free functions over namespace-scope globals (FfxivDynamicMarkers::simulationFrame/
// renderFrame/mutex) -- same shape, just moved onto the IFgMarkers object this fork's Game module hands out.

namespace ext::ffxiv::markers
{
namespace
{
class FgMarkersProvider final : public game::IFgMarkers
{
  public:
    // B5: one-line forward to the NR side (ext/nr/feature/DlssNrFeature_Dx12.h), already fully implemented by an
    // earlier wave with zero callers before this one -- was IFGFeature::SetPresentationGuideDelay, now consumed
    // through ext::fg::OnUpscaleStart -> Access::FgDx12::SetPresentationGuideDelay (ext/fg/GuideSeams.cpp).
    int PresentationGuideDelay() override { return DlssNr::ConsumePresentationGuideDelay(); }

    // Oracle: FfxivDynamicMarkers::Upscale(frame), called once per FG upscale boundary
    // (Upscaler_Inputs_Dx11wDx12.cpp there; this fork's ext::fg::OnUpscaleStart, ext/fg/GuideSeams.cpp, already
    // wired).
    void OnUpscale(uint64_t frame) override
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!Enabled())
        {
            _simulationFrame = _renderFrame = UINT64_MAX;
            return;
        }
        if (_simulationFrame != frame)
            return; // drop incomplete or skipped intervals

        _simulationFrame = UINT64_MAX;
        if (Mark(sl::PCLMarker::eSimulationEnd, frame) && Mark(sl::PCLMarker::eRenderSubmitStart, frame))
            _renderFrame = frame;
    }

    // Oracle: FfxivDynamicMarkers::BeforePresent(frame), called from FGPresent before the Streamline frame token
    // is fetched (this fork's ext::fg::MarkersBeforePresent, ext/fg/MarkerSeams.cpp, already wired; the host call
    // site already narrows fg->FrameCount() to uint32_t before calling it, matching the oracle's own Mark() frame
    // token narrowing exactly -- see PORT-LOG S7 IFgMarkers for why the interface keeps this width rather than
    // widening it).
    void BeforePresent(uint32_t frameId) override
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (Enabled() && _renderFrame == frameId)
            Mark(sl::PCLMarker::eRenderSubmitEnd, frameId);
        _renderFrame = UINT64_MAX;
    }

    // Oracle: FfxivDynamicMarkers::NextFrame(frame), called from FGPresent after ReflexSleep with the next frame
    // count (this fork's ext::fg::MarkersNextFrame, ext/fg/MarkerSeams.cpp, already wired).
    void NextFrame(uint64_t frame) override
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _simulationFrame = _renderFrame = UINT64_MAX;
        if (Enabled() && Mark(sl::PCLMarker::eSimulationStart, frame))
            _simulationFrame = frame;
    }

  private:
    std::mutex _mutex;
    uint64_t _simulationFrame = UINT64_MAX;
    uint64_t _renderFrame = UINT64_MAX;

    // Oracle: FfxivDynamicMarkers::Enabled(), minus the gameExe check (ext/ffxiv/markers/FfxivMarkersEnabled.h's
    // doc comment explains why dropping it is correct here, not a shortcut).
    static bool Enabled()
    {
        auto& state = State::Instance();
        return MarkersEnabledValue(
            Config::Instance()->FGEnabled.value_or_default(), Config::Instance()->FGDLSSGForceDMFG.value_or_default(),
            state.activeFgInput == FGInput::Upscaler, state.activeFgOutput == FGOutput::DLSSG, state.isShuttingDown,
            ReflexHooks::gameIsSendingMarkers(), StreamlineProxy::IsD3D12Inited(),
            StreamlineProxy::GetNewFrameToken() != nullptr, StreamlineProxy::PCLSetMarker() != nullptr);
    }

    // Oracle: FfxivDynamicMarkers::Mark(marker, frame) -- fetches a fresh Streamline frame token for `frame` (the
    // oracle narrows it to uint32_t for the token the same way Reflex_Hooks.cpp's own marker sites do) and sends
    // the PCL marker under ScopedOptiScalerReflex (this fork's primitive, see the file-level comment above).
    static bool Mark(sl::PCLMarker marker, uint64_t frame)
    {
        sl::FrameToken* token = nullptr;
        uint32_t id = static_cast<uint32_t>(frame);
        if (StreamlineProxy::GetNewFrameToken()(token, &id) != sl::Result::eOk || token == nullptr)
            return false;

        sl::Result result;
        {
            ScopedOptiScalerReflex optiScalerCall {};
            result = StreamlineProxy::PCLSetMarker()(marker, *token);
        }
        LOG_TRACE("FFXIV dynamic marker: frame={} marker={} result={}", frame, (int) marker, (int) result);
        return result == sl::Result::eOk;
    }
};

FgMarkersProvider provider;
} // namespace

game::IFgMarkers& Provider() { return provider; }
} // namespace ext::ffxiv::markers
