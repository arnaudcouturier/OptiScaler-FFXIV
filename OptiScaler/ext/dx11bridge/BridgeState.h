#pragma once
// Fork state of one IFeature_Dx11wDx12 (upscalers/IFeature_Dx11wDx12.h `_ext`), mirrors BridgeExt.h's Dx11wDx12SC
// state but on the feature object instead of the swap chain. C4: the lazily-created forced-quality colour scaler
// (oracle's IFeature_Dx11wDx12::ForcedQualityColorScaler) and its "logged once" flag (oracle's
// ReportedForcedQualityColorScale).
//
// ColorScaler_Dx12 is only forward-declared here, like ext/fg/Dx12Ext.h's GuideState/XefgState: the ctor/dtor are
// declared but defined out-of-line (BridgeState.cpp, which does include ColorScaler_Dx12.h), so every upstream TU
// that reaches this header through ext/Seams.h is not forced to also pull in Shader_Dx12 and the output-scaling
// shader headers.

#include <memory>

namespace ext::dx11bridge
{
class ColorScaler_Dx12; // ext/dx11bridge/ColorScaler_Dx12.h

struct BridgeState
{
    BridgeState();
    ~BridgeState();
    BridgeState(const BridgeState&) = delete;
    BridgeState& operator=(const BridgeState&) = delete;

    // C4: lazily created on first use (ext::dx11bridge::PrepareUpscalerColor), Lanczos3-fixed, sized by the
    // resources passed to Dispatch each call.
    std::unique_ptr<ColorScaler_Dx12> colorScaler;
    // C4: logs the one-time "scaling the complete WxH frame..." INFO line once, not every frame.
    bool reportedForcedQualityColorScale = false;
};
} // namespace ext::dx11bridge
