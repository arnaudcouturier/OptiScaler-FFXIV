#define main ExistingWarpMain
#include "ssr_warp.cpp"
#undef main

int main()
{
    Harness h; CHECK(h.Create());
    Frame frame = BuildScene(true, true, 16, 16);
    CHECK(h.Upload(frame));
    ID3D11RenderTargetView* nativeTarget = h.sceneRtv.Get();
    h.context->OMSetRenderTargets(1, &nativeTarget, nullptr);
    const char* shader = "float4 PS():SV_Target{return float4(.2,.4,.6,1);}";
    ComPtr<ID3DBlob> blob, errors;
    CHECK(SUCCEEDED(D3DCompile(shader, strlen(shader), "binding-groups-reader", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &blob, &errors)));
    if (!blob) return Finish("ssr_binding_groups");
    ComPtr<ID3D11PixelShader> reader;
    CHECK(SUCCEEDED(h.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &reader)));
    ReaderSlots slots; slots.reflection=8; slots.vposition=0; slots.camera=2; slots.water=true;
    CHECK(detail::StampShader(reader.Get(), PackSlots(slots)));
    h.context->PSSetShader(reader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* nativeMap=h.planarView.Get(); h.context->PSSetShaderResources(8,1,&nativeMap);
    ID3D11ShaderResourceView* position=h.positionView.Get(); h.context->PSSetShaderResources(0,1,&position);
    ID3D11Buffer* camera=h.camera.Get(); h.context->PSSetConstantBuffers(2,1,&camera);
    D3D11_VIEWPORT window{frame.facts.window[0],frame.facts.window[1],frame.facts.window[2],frame.facts.window[3],0,1};
    h.context->RSSetViewports(1,&window);
    ext::Cfg().ffxiv.screenSpaceReflections.value=true;
    Publish(true); PublishDiagnostic(true);
    for (const auto item : { std::pair<uint32_t,uint32_t>{14,2}, {15,12} })
    {
        SetDiagnosticModeForTest(item.first);
        ext::present::g_ssrTestFrame=8800+item.first;
        const uint32_t priorRuns=CurrentStatus().runs;
        { BoundReader bound=SwapReader(h.context.Get(),ext::ffxiv::engine::kReflectionReaderBit); CHECK(!bound.Bound()); }
        CHECK(CurrentStatus().runs==priorRuns+1);
        const auto ops=CurrentStatus().diagnosticOperations;
        CHECK(ops.stateSwaps==2 && ops.pipelineBinds==item.second);
        CHECK(ops.copies==0 && ops.clears==0 && ops.maps==0 && ops.sceneOmSets==0);
        CHECK(ops.fullscreenDraws==0 && ops.timingBegins==0 && ops.timingEnds==0 && ops.timingGetData==0);
        ID3D11ShaderResourceView* restoredMap=nullptr; h.context->PSGetShaderResources(8,1,&restoredMap);
        CHECK(restoredMap==nativeMap); if(restoredMap)restoredMap->Release();
        ID3D11RenderTargetView* restoredTarget=nullptr; h.context->OMGetRenderTargets(1,&restoredTarget,nullptr);
        CHECK(restoredTarget==nativeTarget); if(restoredTarget)restoredTarget->Release();
    }
    PublishDiagnostic(false);
    return Finish("ssr_binding_groups");
}
