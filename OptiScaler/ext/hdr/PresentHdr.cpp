#include "pch.h"

#include "ext/Seams.h"
#include "ext/hdr/Hdr10.h"

#include <State.h>

namespace ext::present
{
PresenterPlan PlanDx12Presenter(HWND window, bool skipFgSwapchainCreation)
{
    PresenterPlan plan;
    plan.hdr = !skipFgSwapchainCreation && hdr::Request(window);
    plan.createFg = State::Instance().activeFgOutput != FGOutput::NoFG;
    return plan;
}

void PatchPresenterDesc(DXGI_SWAP_CHAIN_DESC& desc, const PresenterPlan& plan)
{
    if (plan.hdr)
        desc.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
}

void PatchPresenterDesc(DXGI_SWAP_CHAIN_DESC1& desc, const PresenterPlan& plan)
{
    if (plan.hdr)
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
}

void LogPlainPresenter(const PresenterPlan& plan, unsigned int fgResult)
{
    if (!plan.createFg)
        LOG_INFO("OptiHDR: creating plain DX12 presenter with FG provider None");
    else
        LOG_WARN("Dx11wDx12 FG swapchain creation failed: {:X}; creating plain DX12 swapchain", fgResult);
}
} // namespace ext::present
