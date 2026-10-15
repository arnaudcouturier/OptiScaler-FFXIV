#include "pch.h"

#include "ext/Seams.h"
#include "ext/fg/xefg/XeFGPacing.h"

#include <framegen/xefg/XeFG_Dx12.h>

// D3: the activation hook (was an inline call in XeFG_Dx12::Activate; the DLSS-G presentation telemetry reset of E3
// joins it from DLSSG_Common::Activate). Fullscreen-state logging of the reference build is dropped.

namespace ext::fg
{
void OnActivated(IFGFeature* fg)
{
    // XeFG_Dx12::Activate, before the feature is marked active: the provider's pacing restarts clean.
    if (dynamic_cast<XeFG_Dx12*>(fg) != nullptr)
        XeFGPacing::RequestRecovery();
}
} // namespace ext::fg
