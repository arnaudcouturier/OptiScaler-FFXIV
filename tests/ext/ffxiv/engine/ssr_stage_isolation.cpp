// Exercise the production SSR pass's diagnostic stages against the native WARP fixture.
#define main OriginalSsrWarpMain
#include "ssr_warp.cpp"
#undef main

namespace
{
std::vector<uint8_t> Bytes(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, uint32_t bytesPerTexel)
{
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc=desc; stagingDesc.Usage=D3D11_USAGE_STAGING; stagingDesc.BindFlags=0; stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; CHECK(SUCCEEDED(device->CreateTexture2D(&stagingDesc,nullptr,&staging)));
    context->CopyResource(staging.Get(),source);
    D3D11_MAPPED_SUBRESOURCE mapped{}; const HRESULT hr=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped); CHECK(SUCCEEDED(hr));
    std::vector<uint8_t> result(size_t(desc.Width)*desc.Height*bytesPerTexel);
    if(SUCCEEDED(hr) && mapped.pData) {
        for(UINT y=0;y<desc.Height;++y) std::memcpy(result.data()+size_t(y)*desc.Width*bytesPerTexel,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(desc.Width)*bytesPerTexel);
        context->Unmap(staging.Get(),0);
    }
    return result;
}
bool SameWindow(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,uint32_t edge,uint32_t bytesPerTexel,uint32_t x,uint32_t y)
{
    if(a.size()!=b.size()||a.empty()) return false;
    for(uint32_t row=0;row<64;++row) {
        const size_t offset=(size_t(y+row)*edge+x)*bytesPerTexel;
        if(std::memcmp(a.data()+offset,b.data()+offset,size_t(64)*bytesPerTexel)!=0) return false;
    }
    return true;
}
}

int main()
{
    Harness h; CHECK(h.Create());
    Pass pass;
    Frame a=BuildScene(true,true,16,16);
    CHECK(h.Upload(a));
    PassInputs in{h.planarView.Get(),h.planarTexture.Get(),h.positionView.Get(),h.sceneTexture.Get(),h.camera.Get(),a.facts,a.params};
    ID3D11RenderTargetView* sceneRtv=h.sceneRtv.Get(); h.context->OMSetRenderTargets(1,&sceneRtv,nullptr);
    auto sourceA=Bytes(h.device.Get(),h.context.Get(),h.sceneTexture.Get(),8);
    CHECK(!sourceA.empty()); CHECK(pass.Run(h.context.Get(),in,true,false,false,false,DiagnosticStages::PreparationOnly)!=nullptr);
    const auto k=pass.LastDiagnosticOperations();
    CHECK(k.pipelineBinds==0);
    CHECK(k.copies==4 && k.clears==1 && k.maps==1 && k.stateSwaps==0 && k.fullscreenDraws==0);
    CHECK(k.timingBegins==0 && k.timingEnds==0 && k.timingGetData==0 && k.sceneOmSets==0);
    auto sceneK=Bytes(h.device.Get(),h.context.Get(),pass.SceneTexture(),8);
    CHECK(SameWindow(sceneK,sourceA,128,8,16,16));
    auto* diagnostic=pass.DiagnosticEngineCopy(); CHECK(diagnostic!=nullptr);
    if(diagnostic) { ComPtr<ID3D11Resource> r; diagnostic->GetResource(&r); ComPtr<ID3D11Texture2D> t; r.As(&t); auto clone=Bytes(h.device.Get(),h.context.Get(),t.Get(),4); auto engine=Bytes(h.device.Get(),h.context.Get(),h.planarTexture.Get(),4); CHECK(clone.size()==engine.size() && !clone.empty() && std::memcmp(clone.data(),engine.data(),engine.size())==0); }

    // PipelineOnly on the same pass and dimensions must leave the A scene and constants untouched.
    // Seed legal RTV0 + OM UAV1 and verify the seed itself before invoking production code.
    D3D11_TEXTURE2D_DESC ud{}; ud.Width=4;ud.Height=4;ud.MipLevels=1;ud.ArraySize=1;ud.Format=DXGI_FORMAT_R32_UINT;ud.SampleDesc.Count=1;ud.Usage=D3D11_USAGE_DEFAULT;ud.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> ut; CHECK(SUCCEEDED(h.device->CreateTexture2D(&ud,nullptr,&ut))); ComPtr<ID3D11UnorderedAccessView> uav; CHECK(SUCCEEDED(h.device->CreateUnorderedAccessView(ut.Get(),nullptr,&uav)));
    ID3D11UnorderedAccessView* u=uav.Get(); UINT initial=0xffffffffu; h.context->OMSetRenderTargetsAndUnorderedAccessViews(1,&sceneRtv,nullptr,1,1,&u,&initial);
    ID3D11RenderTargetView* gotR=nullptr; ID3D11DepthStencilView* gotD=nullptr; h.context->OMGetRenderTargets(1,&gotR,&gotD); CHECK(gotR==sceneRtv); if(gotR)gotR->Release();if(gotD)gotD->Release();
    ID3D11UnorderedAccessView* gotU=nullptr; h.context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&gotU); CHECK(gotU==uav.Get()); if(gotU)gotU->Release();
    Frame b=BuildScene(false,false,16,16);
    for (auto& channel : b.colour) channel = 0.8125f;
    CHECK(h.Upload(b));
    auto sourceB=Bytes(h.device.Get(),h.context.Get(),h.sceneTexture.Get(),8);
    CHECK(!SameWindow(sourceA,sourceB,128,8,16,16));
    sceneRtv=h.sceneRtv.Get();
    h.context->OMSetRenderTargetsAndUnorderedAccessViews(1,&sceneRtv,nullptr,1,1,&u,&initial);
    gotR=nullptr; h.context->OMGetRenderTargets(1,&gotR,nullptr); CHECK(gotR==sceneRtv); if(gotR)gotR->Release();
    gotU=nullptr; h.context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&gotU); CHECK(gotU==uav.Get()); if(gotU)gotU->Release();
    in={h.planarView.Get(),h.planarTexture.Get(),h.positionView.Get(),h.sceneTexture.Get(),h.camera.Get(),b.facts,b.params};
    CHECK(pass.Run(h.context.Get(),in,true,false,false,false,DiagnosticStages::PipelineOnly)!=nullptr);
    const auto l=pass.LastDiagnosticOperations(); CHECK(l.copies==0 && l.clears==0 && l.maps==0 && l.stateSwaps==2 && l.fullscreenDraws==0);
    CHECK(l.pipelineBinds==14);
    CHECK(l.timingBegins==0 && l.timingEnds==0 && l.timingGetData==0 && l.sceneOmSets==0);
    CHECK(pass.SceneTexture()!=nullptr); auto sceneL=Bytes(h.device.Get(),h.context.Get(),pass.SceneTexture(),8); CHECK(SameWindow(sceneL,sourceA,128,8,16,16));
    gotR=nullptr;gotD=nullptr;h.context->OMGetRenderTargets(1,&gotR,&gotD);CHECK(gotR==sceneRtv && gotD==nullptr);if(gotR)gotR->Release();if(gotD)gotD->Release();
    gotU=nullptr;h.context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&gotU);CHECK(gotU==uav.Get());if(gotU)gotU->Release();

    CHECK(pass.Run(h.context.Get(),in,true,false,false,false,DiagnosticStages::CombinedNoDraw)!=nullptr);
    const auto m=pass.LastDiagnosticOperations(); CHECK(m.copies==4 && m.maps==1 && m.stateSwaps==2 && m.fullscreenDraws==0);
    CHECK(m.pipelineBinds==14);
    CHECK(m.timingBegins==0 && m.timingEnds==0 && m.timingGetData==0 && m.sceneOmSets==0);
    auto sceneM=Bytes(h.device.Get(),h.context.Get(),pass.SceneTexture(),8);
    CHECK(SameWindow(sceneM,sourceB,128,8,16,16));
    gotR=nullptr;gotD=nullptr;h.context->OMGetRenderTargets(1,&gotR,&gotD);CHECK(gotR==sceneRtv && gotD==nullptr);if(gotR)gotR->Release();if(gotD)gotD->Release();
    gotU=nullptr;h.context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,1,1,&gotU);CHECK(gotU==uav.Get());if(gotU)gotU->Release();
    return Finish("ssr_stage_isolation");
}
