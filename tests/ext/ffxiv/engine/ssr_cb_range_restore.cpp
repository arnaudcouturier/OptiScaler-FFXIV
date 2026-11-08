// Native GPU probe for D3D11.1 constant-buffer range preservation around the real pipeline-only SSR pass.
#define main OriginalSsrWarpMain
#include "ssr_warp.cpp"
#undef main

namespace
{
struct Binding { ComPtr<ID3D11Buffer> buffer; UINT first = 0, count = 0; };

Binding GetPS(ID3D11DeviceContext1* c, UINT slot)
{
    Binding b; ID3D11Buffer* raw = nullptr; c->PSGetConstantBuffers1(slot, 1, &raw, &b.first, &b.count); b.buffer.Attach(raw); return b;
}
Binding GetVS(ID3D11DeviceContext1* c, UINT slot)
{
    Binding b; ID3D11Buffer* raw = nullptr; c->VSGetConstantBuffers1(slot, 1, &raw, &b.first, &b.count); b.buffer.Attach(raw); return b;
}
bool Same(const Binding& a, const Binding& b)
{
    return a.buffer.Get() == b.buffer.Get() && a.first == b.first && a.count == b.count;
}
void Report(const char* stage, const char* name, const Binding& a, const Binding& b)
{
    if (!Same(a,b)) std::printf("%s %s: buffer %p -> %p, firstConstant %u -> %u, numConstants %u -> %u\n", stage,name,
                                 a.buffer.Get(),b.buffer.Get(),a.first,b.first,a.count,b.count);
}
bool MakeTarget(ID3D11Device* d, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv)
{
    D3D11_TEXTURE2D_DESC td{}; td.Width=4; td.Height=4; td.MipLevels=1; td.ArraySize=1; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(d->CreateTexture2D(&td,nullptr,&tex)) && SUCCEEDED(d->CreateRenderTargetView(tex.Get(),nullptr,&rtv));
}
bool DrawAndRead(Harness& h, ID3D11DeviceContext1* c, ID3D11Buffer* cb, ID3D11RenderTargetView* rtv,
                 ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11Texture2D* target, uint8_t pixel[4], bool setup)
{
    if (setup)
    {
    ID3D11Buffer* b=cb; const UINT first=16, count=16; c->PSSetConstantBuffers1(0,1,&b,&first,&count);
    // Seed once before the pass. The post-pass draw must use the restored binding, without repairing it.
    c->OMSetRenderTargets(1,&rtv,nullptr); D3D11_VIEWPORT vp{0,0,4,4,0,1}; c->RSSetViewports(1,&vp);
    c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs,nullptr,0); c->PSSetShader(ps,nullptr,0);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster; if(FAILED(h.device->CreateRasterizerState(&rd,&raster))) return false;
    c->RSSetState(raster.Get());
    }
    // A missing post-pass draw must not pass by reading the previous green result.
    const float sentinel[4] { 0, 0, 1, 1 };
    c->ClearRenderTargetView(rtv, sentinel);
    c->Draw(3,0); c->Flush();
    D3D11_TEXTURE2D_DESC td{}; target->GetDesc(&td); td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; if(FAILED(h.device->CreateTexture2D(&td,nullptr,&staging))) return false;
    c->CopyResource(staging.Get(),target); D3D11_MAPPED_SUBRESOURCE map{}; if(FAILED(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) return false;
    std::memcpy(pixel,static_cast<const uint8_t*>(map.pData)+map.RowPitch*2+8,4); c->Unmap(staging.Get(),0); return true;
}
}

int main(int argc, char** argv)
{
    bool hardware = false;
    for (int i = 1; i < argc; ++i)
        hardware = hardware || std::strcmp(argv[i], "--hardware") == 0;
    Harness h;
    if (hardware)
    {
        D3D_FEATURE_LEVEL requested[] { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL actual = D3D_FEATURE_LEVEL_9_1;
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, requested, 1, D3D11_SDK_VERSION,
                                            &h.device, &actual, &h.context);
        if (FAILED(hr) || !h.device || !h.context)
        { std::printf("SKIP: hardware D3D11 FL11_0 device unavailable (0x%08lx); no GPU range-preservation conclusion\n", hr); return Finish("ssr_cb_range_restore"); }
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(h.device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
            std::printf("Hardware adapter: %ls vendor=0x%04x device=0x%04x FL=0x%04x\n", desc.Description, desc.VendorId, desc.DeviceId, actual);
        else std::printf("Hardware adapter: description unavailable FL=0x%04x\n", actual);
        if (desc.VendorId != 0x10de) std::printf("NOTE: adapter is not NVIDIA (vendor=0x%04x)\n", desc.VendorId);
    }
    else { CHECK(h.Create()); if(!h.device) return Finish("ssr_cb_range_restore"); }
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if(FAILED(h.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof options)) || !options.ConstantBufferOffsetting)
    { std::puts("SKIP: D3D11.1 constant-buffer offsetting unsupported; no range-preservation conclusion"); return Finish("ssr_cb_range_restore"); }
    ComPtr<ID3D11DeviceContext1> c1; CHECK(SUCCEEDED(h.context.As(&c1))); if(!c1) return Finish("ssr_cb_range_restore");
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=4096; bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb; CHECK(SUCCEEDED(h.device->CreateBuffer(&bd,nullptr,&cb)));
    float regs[256][4]{}; regs[0][0]=1; regs[0][3]=1; regs[16][1]=1; regs[16][3]=1; h.context->UpdateSubresource(cb.Get(),0,nullptr,regs,0,0);
    ID3D11Buffer* b=cb.Get(); UINT first=16,count=16;
    c1->PSSetConstantBuffers1(0,1,&b,&first,&count); c1->PSSetConstantBuffers1(2,1,&b,&first,&count);
    c1->VSSetConstantBuffers1(0,1,&b,&first,&count); c1->VSSetConstantBuffers1(1,1,&b,&first,&count);
    const auto ps0=GetPS(c1.Get(),0), ps2=GetPS(c1.Get(),2), vs0=GetVS(c1.Get(),0), vs1=GetVS(c1.Get(),1);
    CHECK(ps0.buffer.Get()==cb.Get() && ps0.first==16 && ps0.count==16);
    CHECK(ps2.buffer.Get()==cb.Get() && ps2.first==16 && ps2.count==16);
    CHECK(vs0.buffer.Get()==cb.Get() && vs0.first==16 && vs0.count==16);
    CHECK(vs1.buffer.Get()==cb.Get() && vs1.first==16 && vs1.count==16);
    const char* src="struct V{float4 p:SV_Position;}; V VS(uint i:SV_VertexID){V o;o.p=float4(i==1?3:-1,i==2?3:-1,0,1);return o;} cbuffer C:register(b0){float4 color;} float4 PS():SV_Target{return color;}";
    ComPtr<ID3DBlob> vb, pb, errors; CHECK(SUCCEEDED(D3DCompile(src,strlen(src),"cb-range",nullptr,nullptr,"VS","vs_5_0",0,0,&vb,&errors)));
    CHECK(SUCCEEDED(D3DCompile(src,strlen(src),"cb-range",nullptr,nullptr,"PS","ps_5_0",0,0,&pb,&errors)));
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    CHECK(vb && pb && SUCCEEDED(h.device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs)) &&
          SUCCEEDED(h.device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps)));
    ComPtr<ID3D11Texture2D> target; ComPtr<ID3D11RenderTargetView> rtv; CHECK(MakeTarget(h.device.Get(),target,rtv));
    uint8_t before[4]{},after[4]{};
    Frame frame=BuildScene(true,true,16,16); CHECK(h.Upload(frame));
    PassInputs in; in.planar=h.planarView.Get(); in.planarTexture=h.planarTexture.Get(); in.vposition=h.positionView.Get();
    in.scene=h.sceneTexture.Get(); in.camera=h.camera.Get(); in.facts=frame.facts; in.params=frame.params;
    ID3D11RenderTargetView* colorTarget=rtv.Get(); c1->OMSetRenderTargets(1,&colorTarget,nullptr);
    D3D11_VIEWPORT vp{0,0,4,4,0,1}; c1->RSSetViewports(1,&vp); c1->VSSetShader(vs.Get(),nullptr,0); c1->PSSetShader(ps.Get(),nullptr,0);
    c1->IASetInputLayout(nullptr); c1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // DrawAndRead seeds PS0. Seed the remaining slots after it, retaining a real, observed range on every tested stage.
    c1->PSSetConstantBuffers1(2,1,&b,&first,&count); c1->VSSetConstantBuffers1(0,1,&b,&first,&count); c1->VSSetConstantBuffers1(1,1,&b,&first,&count);
    // The original probe draw binds the pixel range and records the expected GPU result before the pass.
    CHECK(DrawAndRead(h,c1.Get(),cb.Get(),rtv.Get(),vs.Get(),ps.Get(),target.Get(),before,true));
    std::printf("GPU color before pass: %u %u %u %u\n",before[0],before[1],before[2],before[3]);
    const auto expectedPS0=GetPS(c1.Get(),0), expectedPS2=GetPS(c1.Get(),2), expectedVS0=GetVS(c1.Get(),0), expectedVS1=GetVS(c1.Get(),1);
    CHECK(expectedPS0.first==16 && expectedPS0.count==16);
    CHECK(before[0]==0 && before[1]==255 && before[2]==0 && before[3]==255);
    std::printf("PS0 range before pass: firstConstant=%u numConstants=%u; GPU color=%u,%u,%u,%u\n",
                expectedPS0.first, expectedPS0.count, before[0], before[1], before[2], before[3]);
    Pass pass; auto* result=pass.Run(c1.Get(),in,true,false,false,false,DiagnosticStages::PipelineOnly); CHECK(result!=nullptr);
    const auto nowPS0=GetPS(c1.Get(),0), nowPS2=GetPS(c1.Get(),2), nowVS0=GetVS(c1.Get(),0), nowVS1=GetVS(c1.Get(),1);
    Report("PS","slot0",expectedPS0,nowPS0); Report("PS","slot2",expectedPS2,nowPS2); Report("VS","slot0",expectedVS0,nowVS0); Report("VS","slot1",expectedVS1,nowVS1);
    CHECK(Same(expectedPS0,nowPS0)); CHECK(Same(expectedPS2,nowPS2)); CHECK(Same(expectedVS0,nowVS0)); CHECK(Same(expectedVS1,nowVS1));
    CHECK(pass.LastDiagnosticOperations().pipelineBinds==14); CHECK(pass.LastDiagnosticOperations().copies==0); CHECK(pass.LastDiagnosticOperations().maps==0); CHECK(pass.LastDiagnosticOperations().stateSwaps==2); CHECK(pass.LastDiagnosticOperations().timingGetData==0);
    std::printf("PipelineOnly operations: pipelineBinds=%u swaps=%u draws=%u copies=%u maps=%u timingQueries=%u\n",
                pass.LastDiagnosticOperations().pipelineBinds, pass.LastDiagnosticOperations().stateSwaps,
                pass.LastDiagnosticOperations().fullscreenDraws, pass.LastDiagnosticOperations().copies,
                pass.LastDiagnosticOperations().maps, pass.LastDiagnosticOperations().timingGetData);
    CHECK(pass.LastDiagnosticOperations().fullscreenDraws==0); CHECK(pass.LastDiagnosticOperations().timingBegins==0 && pass.LastDiagnosticOperations().timingEnds==0);
    CHECK(DrawAndRead(h,c1.Get(),cb.Get(),rtv.Get(),vs.Get(),ps.Get(),target.Get(),after,false));
    CHECK(after[0]==0 && after[1]==255 && after[2]==0 && after[3]==255);
    CHECK(std::memcmp(before,after,4)==0); // GPU-visible b0 range remains the same across the actual pipeline stage.
    std::printf("PS0 range after pass: firstConstant=%u numConstants=%u; GPU color=%u,%u,%u,%u; buffer and PS2/VS0/VS1 metadata preserved\n",
                nowPS0.first, nowPS0.count, after[0], after[1], after[2], after[3]);
    return Finish("ssr_cb_range_restore");
}
