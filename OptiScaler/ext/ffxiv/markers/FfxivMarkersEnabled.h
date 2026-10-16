#pragma once
// Pure decision logic behind the oracle's FfxivDynamicMarkers::Enabled() (misc/FfxivDynamicMarkers.h at the pinned
// ref-nightly-merge SHA), with one condition dropped: `state.gameExe == "ffxiv_dx11.exe"`. That check is replaced
// structurally in this fork by ext::game::Current().fgMarkers being non-null in the first place -- this provider
// is only ever reached for a matched FFXIV build, never for any other game, so the string compare would always be
// true here and is redundant to re-test. Every other condition of the oracle's Enabled() is kept as-is, just
// passed in as plain bools so this header carries no upstream types (State/Config/ReflexHooks/StreamlineProxy) and
// is cheaply unit-testable on its own (tests/ext/ffxiv), the same split ext/upscaler/ForcedQuality.h and
// ext/upscaler/RenderGeometry.h use for their own decision math.
//
// `fgEnabled` = Config::Instance()->FGEnabled.value_or_default()
// `forceDmfg` = Config::Instance()->FGDLSSGForceDMFG.value_or_default()
// `isUpscalerInput` = State::Instance().activeFgInput == FGInput::Upscaler
// `isDlssgOutput` = State::Instance().activeFgOutput == FGOutput::DLSSG
// `isShuttingDown` = State::Instance().isShuttingDown
// `gameIsSendingMarkers` = ReflexHooks::gameIsSendingMarkers()
// `isD3D12Inited` = StreamlineProxy::IsD3D12Inited()
// `hasFrameTokenFn` = StreamlineProxy::GetNewFrameToken() != nullptr
// `hasSetMarkerFn` = StreamlineProxy::PCLSetMarker() != nullptr

namespace ext::ffxiv::markers
{
inline bool MarkersEnabledValue(bool fgEnabled, bool forceDmfg, bool isUpscalerInput, bool isDlssgOutput,
                                bool isShuttingDown, bool gameIsSendingMarkers, bool isD3D12Inited,
                                bool hasFrameTokenFn, bool hasSetMarkerFn)
{
    return !isShuttingDown && isUpscalerInput && isDlssgOutput && fgEnabled && forceDmfg && !gameIsSendingMarkers &&
           isD3D12Inited && hasFrameTokenFn && hasSetMarkerFn;
}
} // namespace ext::ffxiv::markers
