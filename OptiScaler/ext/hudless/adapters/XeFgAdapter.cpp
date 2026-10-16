#include "pch.h"

#include "ext/hudless/adapters/Adapters.h"

#include <Config.h>
#include <proxies/XeFG_Proxy.h>

namespace ext::hudless::adapters
{
// XeFG (UNTESTED here, docs/HUDLESS.md 9.2): XEFG_SWAPCHAIN_RES_HUDLESS_COLOR through the provider's SetResource; the UI
// image only when the SDK exports xefgSwapChainSetUiCompositionState or Draw-UI-over-FG is on (the provider drops the tag
// itself otherwise, so building the image would be wasted GPU work). No UI-free base and no paste for this provider.
TagResult XeFgTag(IFGFeature_Dx12& fg, const TagContext& context)
{
    const bool uiPath = XeFGProxy::SetUiCompositionState() != nullptr || Config::Instance()->FGDrawUIOverFG.value_or_default();
    return detail::Tag(fg, context, uiPath);
}
} // namespace ext::hudless::adapters
