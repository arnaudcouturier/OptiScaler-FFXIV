#pragma once
// Frame generation provider adapters of the HUD-less engine (docs/HUDLESS.md section 9). An adapter decides which images
// a provider takes and tags them on the frame generator's UI command list of the frame that will be dispatched
// (IFGFeature::GetIndexWillBeDispatched, the stateless predictor that mirrors the provider's own GetDispatchIndex).
// The tags themselves are the provider's SetResource (DLSS-G: kBufferTypeHUDLessColor / kBufferTypeUIColorAndAlpha,
// DLSSG_Dx12.cpp), whose gates (FGDisableHudless, FGDisableUI, ...) stay.
//
//   DLSS-G   HudlessColor, plus the UI image in recomposition mode. Tested here.
//   FSR-FG   HudlessColor only; its UI resource is never registered. UNTESTED here (docs/HUDLESS.md 9.2).
//   XeFG     HudlessColor, plus the UI image only when the SDK exports xefgSwapChainSetUiCompositionState or Draw-UI-over-FG
//            is on. UNTESTED here. (XeFG with OptiHDR rejects UIColor; the engine does not tag at all over HDR10 output.)

#include "ext/hudless/HudlessGpu.h"

#include <cstdint>
#include <d3d12.h>

class IFGFeature_Dx12;

namespace ext::hudless::adapters
{
struct TagContext
{
    ID3D12Device* device = nullptr;
    ID3D12Resource* hudless = nullptr; // the slot's shared texture, resting in COMMON
    ID3D12Resource* final = nullptr;   // the bridge's copy of the final frame, resting in COMMON
    DXGI_FORMAT hudlessFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT finalFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    bool recomposition = false; // Mode == recomposition: also build and tag the UI image
    MaskParams mask;
};

struct TagResult
{
    bool listAvailable = false; // the frame generator handed out its UI command list
    bool hudlessTagged = false;
    bool uiWanted = false;      // recomposition on and the provider takes a UI image
    bool uiTagged = false;
    // The fence the frame generator signals once it submitted that UI list, and the value reserved for it: the tags and the
    // UI image read the slot's textures on that list, so the slot is busy until the fence reaches the value.
    ID3D12Fence* readerFence = nullptr;
    uint64_t readerValue = 0;
};

TagResult DlssgTag(IFGFeature_Dx12& fg, const TagContext& context);
TagResult FsrFgTag(IFGFeature_Dx12& fg, const TagContext& context);
TagResult XeFgTag(IFGFeature_Dx12& fg, const TagContext& context);

namespace detail
{
// Shared body: tags the hud-less image and, when `uiImage` and the mode allow, builds and tags the UI image.
TagResult Tag(IFGFeature_Dx12& fg, const TagContext& context, bool uiImage);
} // namespace detail
} // namespace ext::hudless::adapters
