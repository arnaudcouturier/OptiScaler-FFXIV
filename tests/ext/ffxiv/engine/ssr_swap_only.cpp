#define main ExistingWarpMain
#include "ssr_warp.cpp"
#undef main

int main()
{
    Harness h; CHECK(h.Create());
    Pass pass;
    Frame frame = BuildScene(true, true, 16, 16);
    CHECK(h.Upload(frame));
    PassInputs in { h.planarView.Get(), h.planarTexture.Get(), h.positionView.Get(), h.sceneTexture.Get(), h.camera.Get(), frame.facts, frame.params };
    const auto* engineView = in.planar;
    ID3D11RenderTargetView* seededTarget = h.sceneRtv.Get();
    h.context->OMSetRenderTargets(1, &seededTarget, nullptr);
    CHECK(pass.Run(h.context.Get(), in, true, false, false, false, DiagnosticStages::SwapOnly) != nullptr);
    ID3D11RenderTargetView* restoredTarget = nullptr;
    h.context->OMGetRenderTargets(1, &restoredTarget, nullptr);
    CHECK(restoredTarget == seededTarget);
    if (restoredTarget) restoredTarget->Release();
    const auto n = pass.LastDiagnosticOperations();
    CHECK(pass.TargetsCreated() == 1);
    CHECK(n.copies == 0 && n.clears == 0 && n.maps == 0 && n.stateSwaps == 2);
    CHECK(n.fullscreenDraws == 0 && n.timingBegins == 0 && n.timingEnds == 0 && n.timingGetData == 0);
    CHECK(n.sceneOmSets == 0 && n.pipelineBinds == 0);
    CHECK(pass.OutputView() != engineView); // caller selects the original view in mode N; the pass itself still returns its initialized output.
    // Exercise the production reader too: retaining the same SRV must not hide a redundant setter/restorer.
    const char* shader = "float4 PS():SV_Target{return float4(.2,.4,.6,1);}";
    ComPtr<ID3DBlob> blob, errors;
    CHECK(SUCCEEDED(D3DCompile(shader, strlen(shader), "swap-only-reader", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &blob, &errors)));
    if (!blob) return Finish("ssr_swap_only");
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
    Publish(true); PublishDiagnostic(true); SetDiagnosticModeForTest(13);
    ext::present::g_ssrTestFrame=8801;
    const uint32_t runsBefore=CurrentStatus().runs;
    { BoundReader bound=SwapReader(h.context.Get(),ext::ffxiv::engine::kReflectionReaderBit); CHECK(!bound.Bound()); }
    ID3D11ShaderResourceView* boundMap=nullptr; h.context->PSGetShaderResources(8,1,&boundMap);
    CHECK(boundMap==nativeMap); if(boundMap)boundMap->Release();
    CHECK(CurrentStatus().runs==runsBefore+1);
    CHECK(CurrentStatus().diagnosticOperations.stateSwaps==2 && CurrentStatus().diagnosticOperations.pipelineBinds==0);
    PublishDiagnostic(false);
    return Finish("ssr_swap_only");
}
