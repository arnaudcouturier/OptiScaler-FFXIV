#include "pch.h"

#include "ext/hudless/HudlessAccess.h"
#include "ext/hudless/adapters/Adapters.h"

#include <Config.h>
#include <framegen/IFGFeature_Dx12.h>

namespace ext::hudless::adapters
{
namespace detail
{
TagResult Tag(IFGFeature_Dx12& fg, const TagContext& context, bool uiImage)
{
    TagResult result;

    // The slot the provider's own Dispatch will use. The predictor is stateless and mirrors GetDispatchIndex's resync
    // branch; the bridge never calls GetDispatchIndex itself (it would consume the frame and make the provider skip it).
    const int index = fg.GetIndexWillBeDispatched();
    ID3D12GraphicsCommandList* list = fg.GetUICommandList(index);
    if (list == nullptr)
        return result;

    result.listAvailable = true;
    result.readerFence = Access::Hudless::UiFence(fg);
    result.readerValue = Access::Hudless::UiFenceValue(fg, index); // reserved by the Reset inside GetUICommandList

    Dx12Resource hudless {};
    hudless.type = FG_ResourceType::HudlessColor;
    hudless.resource = context.hudless;
    hudless.cmdList = list;
    hudless.state = D3D12_RESOURCE_STATE_COMMON;
    hudless.validity = FG_ResourceValidity::ValidNow;
    hudless.width = context.width;
    hudless.height = context.height;
    hudless.frameIndex = index;
    result.hudlessTagged = fg.SetResource(&hudless);

    if (!result.hudlessTagged || !uiImage || !context.recomposition ||
        Config::Instance()->FGDisableUI.value_or_default())
        return result;

    result.uiWanted = true;
    ID3D12Resource* image = UiImage::Build(context.device, list, index, context.final, context.finalFormat,
                                           context.hudless, context.hudlessFormat, context.mask);
    if (image == nullptr)
        return result;

    Dx12Resource ui {};
    ui.type = FG_ResourceType::UIColor;
    ui.resource = image;
    ui.cmdList = list;
    ui.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // UiImage::Build leaves the image in this state (see log)
    ui.validity = FG_ResourceValidity::ValidNow;
    ui.width = context.width;
    ui.height = context.height;
    ui.frameIndex = index;
    result.uiTagged = fg.SetResource(&ui);
    return result;
}
} // namespace detail

// DLSS-G (tested): HudlessColor and, in recomposition mode, UIColor.
TagResult DlssgTag(IFGFeature_Dx12& fg, const TagContext& context) { return detail::Tag(fg, context, true); }
} // namespace ext::hudless::adapters
