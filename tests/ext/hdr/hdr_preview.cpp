#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <functional>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#define LOG_INFO(...) ((void)0)
#include "ext/hdr/FfxivHdrPreview.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 failure");}
void expect(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main() try {
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
    const char* hlsl="Texture2D<float4> Source:register(t0);float4 VS(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}float4 PS():SV_Target{return Source.Load(int3(0,0,0));}";
    ComPtr<ID3DBlob> code,error;check(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&code,&error));
    ComPtr<ID3D11VertexShader> vs;check(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    check(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&code,&error));
    ComPtr<ID3D11PixelShader> ps;check(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));
    ext::hdr::FfxivHdrPreview::SetCensusEnabled(true); // the census below runs gated on, like a Diagnostics-on session
    ext::hdr::FfxivHdrPreview::Tag(ps.Get(),3865947726u);
    D3D11_TEXTURE2D_DESC td{};td.Width=64;td.Height=64;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source,target,read;check(d->CreateTexture2D(&td,nullptr,&source));
    ComPtr<ID3D11RenderTargetView> inputRtv;check(d->CreateRenderTargetView(source.Get(),nullptr,&inputRtv));
    ComPtr<ID3D11ShaderResourceView> input;check(d->CreateShaderResourceView(source.Get(),nullptr,&input));
    td.Width=640;td.Height=360;check(d->CreateTexture2D(&td,nullptr,&target));
    ComPtr<ID3D11RenderTargetView> targetRtv;check(d->CreateRenderTargetView(target.Get(),nullptr,&targetRtv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(d->CreateTexture2D(&td,nullptr,&read));
    D3D11_BLEND_DESC bd{};auto& r=bd.RenderTarget[0];r.BlendEnable=true;r.SrcBlend=D3D11_BLEND_SRC_ALPHA;r.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;
    r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlendAlpha=D3D11_BLEND_ZERO;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=15;
    bd.IndependentBlendEnable=false;for(UINT i=1;i<8;++i)bd.RenderTarget[i]=r;
    ComPtr<ID3D11BlendState> blend;check(d->CreateBlendState(&bd,&blend));
    c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),nullptr);c->OMSetBlendState(blend.Get(),nullptr,~0u);
    c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->PSSetShaderResources(0,1,input.GetAddressOf());c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp{0,0,320,360,0,1};c->RSSetViewports(1,&vp);
    auto draw=[&]{c->Draw(3,0);};
    for(float alpha:{1.f,.5f}) {
        ext::hdr::FfxivHdrPreview::EndFrame();const float colour[4]{1,1,1,alpha};c->ClearRenderTargetView(inputRtv.Get(),colour);
        ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);draw();
        auto& state=ext::hdr::FfxivHdrPreview::State();expect(bool(state.current),"Preview draw not captured");
        c->CopyResource(read.Get(),state.current->texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto* bytes=static_cast<unsigned char*>(mapped.pData);
        expect(std::abs(int(bytes[4*100+3])-int((1-alpha)*255))<=1,"Wrong native preview transmittance");
        expect(bytes[4*100]==bytes[4*100+3],"RGB and alpha coverage differ");
        expect(bytes[4*500]==255,"RGB coverage escaped native preview viewport");
        expect(bytes[4*500+3]==255,"Coverage escaped native preview viewport");c->Unmap(read.Get(),0);
        ComPtr<ID3D11RenderTargetView> restored;c->OMGetRenderTargets(1,&restored,nullptr);expect(restored==targetRtv,"Native target not restored");
        ComPtr<ID3D11BlendState> restoredBlend;c->OMGetBlendState(&restoredBlend,nullptr,nullptr);expect(restoredBlend==blend,"Native blend not restored");
    }
    // Depth/stencil-writing UI must be sampled before its one native draw.
    // The replay tests the original buffer but must never change it.
    D3D11_TEXTURE2D_DESC depthDesc=td;depthDesc.Usage=D3D11_USAGE_DEFAULT;depthDesc.CPUAccessFlags=0;
    depthDesc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;check(d->CreateTexture2D(&depthDesc,nullptr,&depth));
    ComPtr<ID3D11DepthStencilView> depthView;check(d->CreateDepthStencilView(depth.Get(),nullptr,&depthView));
    const FLOAT white[4]{1,1,1,1},black[4]{};c->ClearRenderTargetView(inputRtv.Get(),white);
    for(bool stencil:{false,true}) {
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=!stencil;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS;
        ds.StencilEnable=stencil;ds.StencilReadMask=ds.StencilWriteMask=255;
        ds.FrontFace=ds.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_INCR_SAT,D3D11_COMPARISON_EQUAL};
        ComPtr<ID3D11DepthStencilState> nativeDs;check(d->CreateDepthStencilState(&ds,&nativeDs));
        c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),depthView.Get());c->OMSetDepthStencilState(nativeDs.Get(),7);
        c->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,7);c->ClearRenderTargetView(targetRtv.Get(),black);
        ext::hdr::FfxivHdrPreview::EndFrame();ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);draw();
        c->CopyResource(read.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        expect(static_cast<unsigned char*>(mapped.pData)[4*100]==255,"Mask replay changed depth/stencil before the native draw");c->Unmap(read.Get(),0);
        ComPtr<ID3D11DepthStencilState> restored;UINT reference=0;c->OMGetDepthStencilState(&restored,&reference);
        expect(restored==nativeDs && reference==7,"Native clipping state not restored");
        ext::hdr::FfxivHdrPreview::EndFrame();ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);
        c->CopyResource(read.Get(),ext::hdr::FfxivHdrPreview::State().current->texture.Get());check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        expect(static_cast<unsigned char*>(mapped.pData)[4*100+3]==255,"Replay ignored the native depth/stencil test");c->Unmap(read.Get(),0);
    }
    c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),nullptr);c->OMSetDepthStencilState(nullptr,0);
    // A rendered preview can exceed the output extent and need not be at t0.
    // Opaque blending ignores shader alpha, so its mask must do the same.
    ext::hdr::FfxivHdrPreview::EndFrame();D3D11_TEXTURE2D_DESC large{};large.Width=large.Height=1024;large.MipLevels=large.ArraySize=large.SampleDesc.Count=1;
    large.Format=DXGI_FORMAT_R8G8B8A8_UNORM;large.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> largeSource;check(d->CreateTexture2D(&large,nullptr,&largeSource));
    ComPtr<ID3D11ShaderResourceView> largeView;check(d->CreateShaderResourceView(largeSource.Get(),nullptr,&largeView));
    c->PSSetShaderResources(3,1,largeView.GetAddressOf());c->OMSetBlendState(nullptr,nullptr,~0u);
    const FLOAT zeroAlpha[4]{1,1,1,0};c->ClearRenderTargetView(inputRtv.Get(),zeroAlpha);
    ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);expect(bool(ext::hdr::FfxivHdrPreview::State().current),"Large/non-t0 preview rejected");
    c->CopyResource(read.Get(),ext::hdr::FfxivHdrPreview::State().current->texture.Get());D3D11_MAPPED_SUBRESOURCE opaqueMapped{};
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&opaqueMapped));expect(static_cast<unsigned char*>(opaqueMapped.pData)[4*100+3]==0,"Opaque draw inherited zero shader alpha");c->Unmap(read.Get(),0);
    ID3D11ShaderResourceView* nullView=nullptr;c->PSSetShaderResources(3,1,&nullView);
    c->OMSetBlendState(blend.Get(),nullptr,~0u);
    ext::hdr::FfxivHdrPreview::EndFrame();source.Reset(); // Atlas panels also contribute real opacity.
    td.Width=64;td.Height=64;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::vector<unsigned> atlas(64*64,0x80ffffffu);D3D11_SUBRESOURCE_DATA atlasData{atlas.data(),64*4,0};
    check(d->CreateTexture2D(&td,&atlasData,&source));check(d->CreateShaderResourceView(source.Get(),nullptr,&input));c->PSSetShaderResources(0,1,input.GetAddressOf());
    ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);expect(bool(ext::hdr::FfxivHdrPreview::State().current),"Atlas-backed panel opacity omitted");
    c->CopyResource(read.Get(),ext::hdr::FfxivHdrPreview::State().current->texture.Get());D3D11_MAPPED_SUBRESOURCE atlasMapped{};
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&atlasMapped));
    expect(std::abs(int(static_cast<unsigned char*>(atlasMapped.pData)[4*100])-127)<=1,"Transparent atlas panel became opaque");c->Unmap(read.Get(),0);
    ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);
    c->CopyResource(read.Get(),ext::hdr::FfxivHdrPreview::State().current->texture.Get());
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&atlasMapped));
    expect(std::abs(int(static_cast<unsigned char*>(atlasMapped.pData)[4*100])-63)<=1,"Layered UI transmittance did not multiply");c->Unmap(read.Get(),0);
    // Freeze coverage at the bridge boundary; later native UI draws belong to
    // the next frame and must not mutate the mask consumed by DX12.
    ext::hdr::FfxivHdrPreview::SealFrame();
    auto& state=ext::hdr::FfxivHdrPreview::State();const auto before=state.draws;
    ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);
    expect(state.draws==before,"Preview mask changed after the bridge frame boundary");
    c->CopyResource(read.Get(),state.current->texture.Get());
    check(c->Map(read.Get(),0,D3D11_MAP_READ,0,&atlasMapped));
    expect(std::abs(int(static_cast<unsigned char*>(atlasMapped.pData)[4*100])-63)<=1,"Sealed layered coverage changed");
    c->Unmap(read.Get(),0);
    ext::hdr::FfxivHdrPreview::EndFrame();ext::hdr::FfxivHdrPreview::BeforeDraw(c.Get(),draw);
    expect(state.draws==before+1,"Preview coverage did not resume next frame");
    // HDR census: every tagged draw that reaches BeforeDraw is either replayed or counted under exactly ONE reason.
    {
        using namespace ext::hdr::FfxivHdrPreview;
        auto& cs=state.census;
        expect(cs.replayed==state.draws,"replayed counter differs from the number of mask draws");
        auto skippedNow=[&]{std::array<uint64_t,kSkipCount> a{};for(unsigned i=0;i<kSkipCount;++i)a[i]=cs.skipped[i];return a;};
        // runs one BeforeDraw in a fresh frame with the current pipeline state and checks the counters moved as `reason` says
        // (kSkipCount = replayed)
        auto observe=[&](unsigned reason,const char* why){
            const auto drawsBefore=cs.draws.load();const auto replayedBefore=cs.replayed.load();const auto skippedBefore=skippedNow();
            EndFrame();BeforeDraw(c.Get(),draw);
            expect(cs.draws==drawsBefore+1,why);
            expect(cs.replayed==replayedBefore+(reason==kSkipCount?1:0),why);
            const auto after=skippedNow();
            for(unsigned i=0;i<kSkipCount;++i)expect(after[i]==skippedBefore[i]+(i==reason?1:0),why);
        };
        auto makeBlend=[&](D3D11_BLEND src,D3D11_BLEND dest,D3D11_BLEND_OP op,UINT mask,bool a2c){
            D3D11_BLEND_DESC b{};b.AlphaToCoverageEnable=a2c;auto& rt=b.RenderTarget[0];rt.BlendEnable=true;rt.SrcBlend=src;rt.DestBlend=dest;rt.BlendOp=op;
            rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=static_cast<UINT8>(mask);
            ComPtr<ID3D11BlendState> state2;check(d->CreateBlendState(&b,&state2));return state2;
        };
        c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),nullptr);
        observe(kSkipCount,"standard blend must be replayed");
        auto additive=makeBlend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_ONE,D3D11_BLEND_OP_ADD,15,false);
        c->OMSetBlendState(additive.Get(),nullptr,~0u);observe(static_cast<unsigned>(Skip::BlendAdditive),"additive blend not counted as additive");
        auto coverage=makeBlend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_OP_ADD,15,true);
        c->OMSetBlendState(coverage.Get(),nullptr,~0u);observe(static_cast<unsigned>(Skip::BlendAlphaToCoverage),"alpha to coverage not counted");
        auto partial=makeBlend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_OP_ADD,3,false);
        c->OMSetBlendState(partial.Get(),nullptr,~0u);observe(static_cast<unsigned>(Skip::BlendWriteMask),"partial write mask not counted");
        auto subtract=makeBlend(D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_OP_SUBTRACT,15,false);
        c->OMSetBlendState(subtract.Get(),nullptr,~0u);observe(static_cast<unsigned>(Skip::BlendOther),"other blend not counted");
        auto premultiplied=makeBlend(D3D11_BLEND_ONE,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_OP_ADD,15,false);
        c->OMSetBlendState(premultiplied.Get(),nullptr,~0u);observe(kSkipCount,"premultiplied blend must be replayed");
        c->OMSetBlendState(blend.Get(),nullptr,~0u);
        // no texture to read opacity from
        ID3D11ShaderResourceView* none[8]{};c->PSSetShaderResources(0,8,none);observe(static_cast<unsigned>(Skip::NoSource),"missing source not counted");
        c->PSSetShaderResources(0,1,input.GetAddressOf());
        // a pixel shader without the UI tag
        {
            ComPtr<ID3D11PixelShader> plain;ComPtr<ID3DBlob> plainCode,plainError;
            check(D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&plainCode,&plainError));
            check(d->CreatePixelShader(plainCode->GetBufferPointer(),plainCode->GetBufferSize(),nullptr,&plain));
            c->PSSetShader(plain.Get(),nullptr,0);observe(static_cast<unsigned>(Skip::NotMarked),"untagged shader not counted");
            c->PSSetShader(ps.Get(),nullptr,0);
        }
        // a target below 640x360, and two render targets
        c->OMSetRenderTargets(1,inputRtv.GetAddressOf(),nullptr);observe(static_cast<unsigned>(Skip::TargetUnsuitable),"small target not counted");
        {
            D3D11_TEXTURE2D_DESC second{};target->GetDesc(&second);second.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> secondTexture;ComPtr<ID3D11RenderTargetView> secondRtv;check(d->CreateTexture2D(&second,nullptr,&secondTexture));check(d->CreateRenderTargetView(secondTexture.Get(),nullptr,&secondRtv));
            ID3D11RenderTargetView* two[2]{targetRtv.Get(),secondRtv.Get()};c->OMSetRenderTargets(2,two,nullptr);
            observe(static_cast<unsigned>(Skip::MultipleTargets),"two render targets not counted");
        }
        c->OMSetRenderTargets(1,targetRtv.GetAddressOf(),nullptr);
        // sealed frame
        EndFrame();SealFrame();{const auto sealed=cs.skipped[static_cast<unsigned>(Skip::Sealed)].load();BeforeDraw(c.Get(),draw);expect(cs.skipped[static_cast<unsigned>(Skip::Sealed)]==sealed+1,"sealed draw not counted");}
        EndFrame();
        // frames and the 600-frame summary: due exactly when the frame counter crosses a multiple of 600, once, and only with draws seen
        std::string line;
        do {EndFrame();} while(cs.frames%kSummaryFrames!=0);
        expect(cs.framesWithMask>0 && cs.framesWithMask<cs.frames,"frames with a mask not counted separately");
        expect(TakeSummary(line),"summary not offered after 600 frames");
        expect(line.rfind("HDR UI mask: ",0)==0 && line.find("blend_additive 1")!=std::string::npos && line.find("no_source 1")!=std::string::npos &&
               line.find("multiple_targets 1")!=std::string::npos,"summary text lacks the counted reasons");
        expect(!TakeSummary(line),"summary offered twice");
        // Census gate: with Diagnostics off nothing is counted and no summary is offered, while the mask itself still works.
        SetCensusEnabled(true);
        do {EndFrame();} while(cs.frames%kSummaryFrames!=0);
        SetCensusEnabled(false);
        expect(!TakeSummary(line),"summary offered with the gate off: a due summary fired without Diagnostics");
        expect(!TakeSummary(line),"summary offered twice with the gate off");
        {
            const auto drawsBefore=cs.draws.load(),replayedBefore=cs.replayed.load(),framesBefore=cs.frames.load();
            auto skippedBefore=[&]{std::array<uint64_t,kSkipCount> a{};for(unsigned i=0;i<kSkipCount;++i)a[i]=cs.skipped[i];return a;}();
            for(int i=0;i<3;++i){EndFrame();BeforeDraw(c.Get(),draw);}
            expect(cs.draws==drawsBefore && cs.replayed==replayedBefore && cs.frames==framesBefore,"census counted with the gate off");
            const auto skippedAfter=[&]{std::array<uint64_t,kSkipCount> a{};for(unsigned i=0;i<kSkipCount;++i)a[i]=cs.skipped[i];return a;}();
            for(unsigned i=0;i<kSkipCount;++i)expect(skippedAfter[i]==skippedBefore[i],"skip counted with the gate off");
            expect(bool(state.current),"mask capture stopped with the gate off");
        }
        SetCensusEnabled(true);
    }
    puts("PASS: preview alpha/viewport, opaque/transparent coverage, depth/stencil safety, large/non-t0 images, binding restoration, transparent atlas coverage and the Diagnostics-gated census (D3D11 WARP)");return 0;
}catch(const std::exception& e){printf("FAIL: %s\n",e.what());return 1;}
