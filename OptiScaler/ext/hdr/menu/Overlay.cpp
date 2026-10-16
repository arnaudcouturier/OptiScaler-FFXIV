#include "pch.h"

#include "ext/Seams.h"
#include "ext/hdr/menu/HdrMenuLayer.h"
#include "ext/hdr/screenshot/HdrScreenshot.h"

#include <State.h>

#include <memory>

namespace ext::hdr
{
namespace
{
struct OverlayState
{
    std::unique_ptr<menu::MenuLayer> layer;
    D3D12_RECT bounds {};
    bool enabled = false;
};

OverlayState& Overlay()
{
    static OverlayState state;
    return state;
}
} // namespace

DXGI_FORMAT OverlayRtvFormat(DXGI_FORMAT format)
{
    auto& overlay = Overlay();
    overlay.enabled = Active() && format == DXGI_FORMAT_R10G10B10A2_UNORM;
    return overlay.enabled ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
}

bool OverlayPrepare(ID3D12Device* device, ID3D12Resource* output, ImDrawData* drawData, bool (*wait)(UINT64),
                    UINT64 retireValue)
{
    auto& overlay = Overlay();
    if (!overlay.enabled)
        return true;
    if (!device || !output || !drawData)
        return false;

    const auto desc = output->GetDesc();
    const auto bounds = menu::MenuBounds(*drawData, static_cast<UINT>(desc.Width), desc.Height);
    if (bounds.left >= bounds.right || bounds.top >= bounds.bottom)
        return false;

    if (overlay.layer && (overlay.layer->Width() != desc.Width || overlay.layer->Height() != desc.Height))
    {
        if (!wait || !wait(retireValue))
            return false;
    }

    if (!overlay.layer)
        overlay.layer = std::make_unique<menu::MenuLayer>();
    ScopedSkipHeapCapture skipHeapCapture {};
    const auto hr = overlay.layer->Ensure(device, desc);
    if (FAILED(hr))
    {
        LOG_ERROR("OptiHDR menu layer preparation failed: {:X}", static_cast<UINT>(hr));
        return false; // Never draw SDR colours directly into the HDR target.
    }
    overlay.bounds = bounds;
    return true;
}

OverlayScope::OverlayScope(ID3D12GraphicsCommandList* commands, ID3D12Resource* output,
                           D3D12_CPU_DESCRIPTOR_HANDLE outputRtv)
    : _commands(commands), _output(output), _outputRtv(outputRtv)
{
    auto& overlay = Overlay();
    if (overlay.enabled && overlay.layer && overlay.layer->Ready() && commands && output)
    {
        overlay.layer->Begin(commands, output, overlay.bounds);
        _active = true;
    }
}

OverlayScope::~OverlayScope()
{
    if (_active)
    {
        Overlay().layer->End(_commands, _outputRtv, Overlay().bounds);
        // F4: the menu now sits in the PQ output (still a render target). An HDR screenshot that should include the
        // menu is recorded here, before the caller transitions the image to present.
        screenshot::PrepareMenu(_commands, _output);
    }
}

void OverlayRelease()
{
    auto& overlay = Overlay();
    overlay.layer.reset();
    overlay.enabled = false;
    overlay.bounds = {};
}
} // namespace ext::hdr

namespace ext::menu
{
void AdjustTheme(ImGuiStyle& style)
{
    if (hdr::Active())
    {
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        style.Colors[ImGuiCol_ChildBg].w = 1.0f;
        style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    }
}
} // namespace ext::menu
