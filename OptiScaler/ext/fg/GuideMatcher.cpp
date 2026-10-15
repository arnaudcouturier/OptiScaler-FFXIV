#include "pch.h"

#include "ext/fg/Dx12Access.h"
#include "ext/fg/GuideState.h"

// B5 (FG side): provider-neutral presentation-guide matcher, see GuideState.h. Moved from IFGFeature_Dx12.cpp.
// Used by DLSSG_Dx12 and XeFG_Dx12 through ext::fg::SetResourcePre; independent of XeFG itself.

namespace ext
{
fg::GuideState& Access::FgDx12::Guides(IFGFeature_Dx12& fg)
{
    auto& guides = Ext(fg).guides;
    if (!guides)
        guides = std::make_unique<fg::GuideState>();
    return *guides;
}

int Access::FgDx12::GuideAge(IFGFeature_Dx12& fg, int index)
{
    const auto* guides = Ext(fg).guides.get();
    return guides ? guides->age[index] : -1;
}

// These copies are recorded on the existing FG UI list/queue. The provider is
// tagged ValidNow, so later consumption uses its own frame-owned copy.
void Access::FgDx12::RetireGuide(IFGFeature_Dx12& fg, ID3D12Resource*& resource)
{
    if (!resource)
        return;
    if (fg._uiFence)
        fg._uiFence->AddRef();
    Guides(fg).retired.push_back({ resource, fg._uiFence, fg._uiFenceValue });
    resource = nullptr;
}

void Access::FgDx12::CollectGuides(IFGFeature_Dx12& fg)
{
    std::erase_if(Guides(fg).retired,
                  [&](const fg::RetiredGuide& item)
                  {
                      if (!item.fence || item.fence->GetCompletedValue() == UINT64_MAX ||
                          item.fence->GetCompletedValue() < item.value)
                          return false;
                      // A higher signaled fence cannot prove that an older, still-open list
                      // has executed. Keep any snapshots those unsubmitted lists may reference.
                      if (item.fence == fg._uiFence)
                          for (unsigned i = 0; i < BUFFER_COUNT; ++i)
                              if (fg._uiCommandListResetted[i] && fg._uiAllocatorFenceValues[i] <= item.value)
                                  return false;
                      item.resource->Release();
                      item.fence->Release();
                      return true;
                  });
}

void Access::FgDx12::SetPresentationGuideDelay(IFGFeature_Dx12& fg, int age)
{
    auto& guides = Guides(fg);
    const int index = fg.GetIndex();
    age = age >= 0 && age <= 2 ? age : -1;
    if (age < 0)
        for (auto& slot : guides.snapshots)
            for (auto& guide : slot)
                guide.valid = false;
    guides.age[index] = age;
    CollectGuides(fg);
}

bool Access::FgDx12::MatchPresentationGuide(IFGFeature_Dx12& fg, Dx12Resource& input, int index)
{
    auto& guides = Guides(fg);
    const auto kind = input.type == FG_ResourceType::Velocity ? 0 : 1;
    auto& current = guides.snapshots[index][kind];
    const int age = guides.age[index];
    if (!input.cmdList || input.cmdList != fg._uiCommandList[index] ||
        input.validity != FG_ResourceValidity::ValidNow || fg._frameCount < UINT64(age))
        return false;
    if (fg._reset[index] && guides.resetFrame != fg._frameCount)
    {
        for (auto& slot : guides.snapshots)
            for (auto& guide : slot)
                guide.valid = false;
        guides.resetFrame = fg._frameCount;
    }
    if (kind == 0)
    {
        auto& m = guides.metadata[index];
        m = { { fg._jitterX[index], fg._jitterY[index], fg._mvScaleX[index], fg._mvScaleY[index],
                fg._cameraNear[index], fg._cameraFar[index], fg._cameraVFov[index], fg._cameraAspectRatio[index],
                fg._meterFactor[index] },
              {},
              fg._ftDelta[index],
              fg._reset[index] };
        const float* vectors[] { fg._cameraPosition[index], fg._cameraUp[index], fg._cameraRight[index],
                                 fg._cameraForward[index] };
        for (unsigned v = 0; v < 4; ++v)
            std::copy_n(vectors[v], 3, m.vectors[v]);
    }
    const auto desc = input.resource->GetDesc();
    auto sameShape = [](const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
    {
        return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height && a.Format == b.Format &&
               a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
               a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
               a.Flags == b.Flags;
    };
    if (current.resource && !sameShape(current.resource->GetDesc(), desc))
        RetireGuide(fg, current.resource);
    current.valid = false;
    if (current.resource)
        fg.ResourceBarrier(input.cmdList, current.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_COPY_DEST);
    if (!fg.CopyResource(input.cmdList, input.resource, &current.resource, input.state))
        return false;
    fg.ResourceBarrier(input.cmdList, current.resource, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    current.view = input;
    current.view.resource = current.resource;
    current.view.copy = nullptr;
    current.view.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    current.frame = fg._frameCount;
    current.valid = true;
    const auto& selected = guides.snapshots[(index + BUFFER_COUNT - age) % BUFFER_COUNT][kind];
    if (!selected.valid || selected.frame != fg._frameCount - age || !sameShape(selected.resource->GetDesc(), desc) ||
        selected.view.width != input.width || selected.view.height != input.height ||
        selected.view.left != input.left || selected.view.top != input.top)
        return false; // Warm-up, gap or resize: do not tag mismatched guides.
    if (kind == 1)
    {
        const auto& m = guides.metadata[(index + BUFFER_COUNT - age) % BUFFER_COUNT];
        fg.SetJitter(m.values[0], m.values[1], index);
        fg.SetMVScale(m.values[2], m.values[3], index);
        fg.SetCameraValues(m.values[4], m.values[5], m.values[6], m.values[7], m.values[8], index);
        float* vectors[] { fg._cameraPosition[index], fg._cameraUp[index], fg._cameraRight[index],
                           fg._cameraForward[index] };
        for (unsigned v = 0; v < 4; ++v)
            std::copy_n(m.vectors[v], 3, vectors[v]);
        fg.SetFrameTimeDelta(m.delta, index);
        fg.SetReset(fg._reset[index] | m.reset, index);
    }
    const auto commands = input.cmdList;
    input = selected.view;
    input.cmdList = commands;
    input.frameIndex = index;
    input.validity = FG_ResourceValidity::ValidNow;
    if (kind == 1 && (fg._frameCount % 240 == 0 || age == 0))
        LOG_INFO("{} matched NR guides: frame={} scene={} age={} render={}x{}", fg.Name(), fg._frameCount,
                 selected.frame, age, input.width, input.height);
    return true;
}

void Access::FgDx12::InvalidateGuides(IFGFeature_Dx12& fg)
{
    auto* guides = Ext(fg).guides.get();
    if (!guides)
        return;
    guides->sceneHistory.Invalidate();
    for (auto& slot : guides->snapshots)
        for (auto& guide : slot)
            guide.valid = false;
}

void Access::FgDx12::ReleaseGuides(IFGFeature_Dx12& fg)
{
    if (!Ext(fg).guides)
        return;
    CollectGuides(fg);
    for (auto& slot : Guides(fg).snapshots)
        for (auto& guide : slot)
        {
            RetireGuide(fg, guide.resource);
            guide.valid = false;
        }
    // Unfinished copies are deliberately retained through shutdown, not freed
    // while an unsubmitted/driver-owned command may still reference them.
}
} // namespace ext
