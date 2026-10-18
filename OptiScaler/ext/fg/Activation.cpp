#include "pch.h"

#include "ext/Seams.h"
#include "ext/fg/FgInternal.h"
#include "ext/fg/xefg/XeFGPacing.h"

#include <framegen/dlssg/DLSSG_Dx12.h>
#include <framegen/xefg/XeFG_Dx12.h>

// D3, E3: the activation hook shared by the two providers (was the DLSSG_Common::OnActivated virtual, overridden by
// DLSSG_Dx12, and an inline call in XeFG_Dx12::Activate). Fullscreen-state logging of the reference build is dropped.

namespace ext::fg
{
void OnActivated(IFGFeature* fg)
{
    // XeFG_Dx12::Activate, before the feature is marked active: the provider's pacing restarts clean.
    if (dynamic_cast<XeFG_Dx12*>(fg) != nullptr)
        XeFGPacing::RequestRecovery();
    // DLSSG_Common::Activate, after the feature is marked active: DLSS-G presentation telemetry restarts (the Vulkan
    // output has none).
    else if (dynamic_cast<DLSSG_Dx12*>(fg) != nullptr)
    {
        ResetDlssgPresentStats();
        ResetAutoMfg(); // dynamic MFG on RTX 40: a new observation window (the kill-switch memory stays)
    }
}
} // namespace ext::fg
