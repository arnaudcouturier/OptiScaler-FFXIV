#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "ext/nr/spatial/DlssNr_Spatial.h"
#include "ext/nr/DepthAwareWeights.h"
#include <cstring>
#include <limits>
using Microsoft::WRL::ComPtr;
#include "ext/nr/pass/DlssNr_Common.h"
using namespace DlssNr::Spatial;
void check(HRESULT r) { if (FAILED(r)) throw std::runtime_error("D3D failure"); }
void expect(bool v) { if (!v) throw std::runtime_error("Spatial shader reference mismatch"); }
// A mismatch that says where: the case, the pixel, what the shader wrote and what the host reference expects.
void expectAt(bool v, const char* label, unsigned x, unsigned y, unsigned channel, float got, float want) {
    if (v) return;
    std::fprintf(stderr, "MISMATCH %s at (%u,%u) channel %u: shader %.6f, reference %.6f\n", label, x, y, channel, got, want);
    throw std::runtime_error("Spatial shader reference mismatch");
}
struct Texture { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav; unsigned w,h,channels; };
struct Runner {
    ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c; ComPtr<ID3D11ComputeShader> shaders[4]; // resolve and friends, spatial, guide match, and the resolve as it was before the depth-aware enlargement
    ComPtr<ID3D11Buffer> cb; ComPtr<ID3D11SamplerState> sampler;
    Runner(wchar_t** paths) {
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c));
        for (unsigned i=0;i<4;++i) {
            ComPtr<ID3DBlob> code,error;
            auto hr=D3DCompileFromFile(paths[i+1],nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
            if(error) std::printf("%s",(const char*)error->GetBufferPointer());check(hr);
            check(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i]));
        }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(Constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(d->CreateBuffer(&bd,nullptr,&cb));
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;check(d->CreateSamplerState(&sd,&sampler));
    }
    Texture make(unsigned w,unsigned h,unsigned channels,const std::vector<float>& data={}) {
        Texture t{};t.w=w;t.h=h;t.channels=channels;
        D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;
        td.Format=channels==1?DXGI_FORMAT_R32_FLOAT:channels==2?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init{data.data(),w*channels*4,0};check(d->CreateTexture2D(&td,data.empty()?nullptr:&init,&t.tex));
        check(d->CreateShaderResourceView(t.tex.Get(),nullptr,&t.srv));check(d->CreateUnorderedAccessView(t.tex.Get(),nullptr,&t.uav));return t;
    }
    void run(const DlssNrConstants& k,Texture& a,Texture& b,Texture& m,Texture& out,Texture& second,unsigned pipeline=0,Texture* depth=nullptr) {
        ID3D11ShaderResourceView* sv[]={a.srv.Get(),b.srv.Get(),m.srv.Get(),m.srv.Get(),m.srv.Get(),depth?depth->srv.Get():m.srv.Get()};ID3D11UnorderedAccessView* uv[]={out.uav.Get(),second.uav.Get()};
        c->UpdateSubresource(cb.Get(),0,nullptr,&k,0,0);c->CSSetShader(shaders[pipeline].Get(),nullptr,0);
        c->CSSetShaderResources(0,6,sv);c->CSSetUnorderedAccessViews(0,2,uv,nullptr);c->CSSetConstantBuffers(0,1,cb.GetAddressOf());c->CSSetSamplers(0,1,sampler.GetAddressOf());
        c->Dispatch((k.Width+7)/8,(k.Height+7)/8,1);
        ID3D11ShaderResourceView* ns[6]={};ID3D11UnorderedAccessView* nu[2]={};c->CSSetShaderResources(0,6,ns);c->CSSetUnorderedAccessViews(0,2,nu,nullptr);
    }
    std::vector<float> read(Texture& t) {
        D3D11_TEXTURE2D_DESC desc{};t.tex->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(d->CreateTexture2D(&desc,nullptr,&staging));c->CopyResource(staging.Get(),t.tex.Get());
        D3D11_MAPPED_SUBRESOURCE map{};check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<float> result(t.w*t.h*t.channels);
        for(unsigned y=0;y<t.h;++y)memcpy(result.data()+y*t.w*t.channels,(char*)map.pData+y*map.RowPitch,t.w*t.channels*4);
        c->Unmap(staging.Get(),0);return result;
    }
};

int wmain(int argc,wchar_t** argv) try {
    expect(argc==5);Runner r(argv);
    static_assert(sizeof(DlssNrConstants)==256);
    static_assert(offsetof(DlssNrConstants,SpatialWarp)==144);
    // Actual typed scalar/RG UAVs and independent offset/padded source regions.
    std::vector<float> dep(16*10), mv(20*12*2);
    for(unsigned y=0;y<10;++y)for(unsigned x=0;x<16;++x)dep[y*16+x]=float(x+100*y);
    for(unsigned y=0;y<12;++y)for(unsigned x=0;x<20;++x){mv[(y*20+x)*2]=float(x);mv[(y*20+x)*2+1]=-float(y);}
    auto d=r.make(16,10,1,dep),m=r.make(20,12,2,mv),od=r.make(5,3,1),om=r.make(5,3,2);
    DlssNrConstants k{};k.Mode=DlssNrMode_ResizePrivateGuides;k.Width=5;k.Height=3;
    k.GuideWidth=10;k.GuideHeight=6;k.DebugView=3;k.CompareMode=2;
    k.TransferStrength=12;k.ColourStrength=8;k.CompareSwap=4;k.Transfer=1;
    r.run(k,d,m,d,od,om,2);
    auto dd=r.read(od),mm=r.read(om);
    for(unsigned y=0;y<3;++y)for(unsigned x=0;x<5;++x){
        unsigned dx=3+unsigned((x+.5f)/5*10),dy=2+unsigned((y+.5f)/3*6);
        unsigned mx=4+unsigned((x+.5f)/5*12),my=1+unsigned((y+.5f)/3*8);
        expect(dd[y*5+x]==dep[dy*16+dx]);expect(mm[(y*5+x)*2]==float(mx));expect(mm[(y*5+x)*2+1]==-float(my));
    }
    std::puts("PASS: typed NR guide resize, padded independent regions, point depth and unchanged vector units");
    for(unsigned profile : {0u,1u}) for(float scale : {1.0f,.75f}) { // shader maths validated independently of the production native-only gate
        Settings settings{};settings.enabled=true;settings.offsetX=7;settings.offsetY=-4;
        if(profile==1){ // the foveated profile through the same fused path: the profile bit rides in the selector word
            settings.profile=Profile::Foveated;settings.foveaWidth=200;settings.foveaHeight=100;settings.foveaFalloff=30;
            settings.foveaEdgeX=.5f;settings.foveaEdgeY=.35f;
        }
        auto layout=Build(settings,320,160,scale,320,160);expect(layout.active&&layout.profile==(profile?Profile::Foveated:Profile::Peripheral));
        std::vector<float> proxy(layout.modelW*layout.modelH*4),answer(proxy.size()),base(320*160*4);
        for(unsigned y=0;y<layout.modelH;++y)for(unsigned x=0;x<layout.modelW;++x){auto i=(y*layout.modelW+x)*4;
            proxy[i]=.1f+.6f*x/layout.modelW;proxy[i+1]=.15f+.5f*y/layout.modelH;proxy[i+2]=.3f;proxy[i+3]=1;
            answer[i]=proxy[i]+.03f*std::sin(x*.13f);answer[i+1]=proxy[i+1]+.02f*std::cos(y*.19f);answer[i+2]=.32f;answer[i+3]=1;
        }
        for(unsigned y=0;y<160;++y)for(unsigned x=0;x<320;++x){auto i=(y*320+x)*4;base[i]=.1f+2.f*x/320;base[i+1]=.2f+1.5f*y/160;base[i+2]=.3f;base[i+3]=.8f;}
        auto p=r.make(layout.modelW,layout.modelH,4,proxy),a=r.make(layout.modelW,layout.modelH,4,answer),o=r.make(320,160,4,base);
        auto up=r.make(320,160,4),ua=r.make(320,160,4),out=r.make(320,160,4),keep=r.make(320,160,4);
        // Explicit legacy unpack onto the native uniform grid, then resolve.
        layout.ordinaryW=320;layout.ordinaryH=160;
        auto sc=MakeConstants(layout,102,{{0,0,320,160},{0,0,320,160}},1,1,320,160);
        DlssNrConstants bytes{};std::memcpy(&bytes,&sc,sizeof(sc));r.run(bytes,p,a,o,up,ua,1);
        for(unsigned hdr : {0u,1u})for(unsigned mode : {0u,1u,2u,3u,4u})for(float strength : {0.f,1.f,2.f}) {
            k={};k.Mode=DlssNrMode_Resolve;k.Width=320;k.Height=160;k.WhitePoint=1;
            k.Passthrough=hdr?0:1;k.ApplyModel=1;k.Transfer=1;k.SpatialResidual=1;k.ReversibleMode=mode;
            k.TransferStrength=strength;k.ColourStrength=1;k.MaxRatio=3;
            k.SkinProtection=1;k.SkinDetail=.6f;k.SkinColour=.7f;k.EnvironmentDetail=1;k.EnvironmentColour=1;
            r.run(k,up,ua,o,out,keep);auto legacy=r.read(out);
            const auto resolveWarp=MakeResolveWarp(layout,true);
            expect(resolveWarp.residual==(profile?(2u|DlssNr::Foveated::kResolveFoveatedBit):2u));
            k.SpatialResidual=resolveWarp.residual;
            k.SpatialWorkSize[0]=resolveWarp.workSize[0];k.SpatialWorkSize[1]=resolveWarp.workSize[1];
            std::memcpy(k.SpatialWarp,resolveWarp.fields,sizeof(resolveWarp.fields));
            r.run(k,p,a,o,out,keep);auto fused=r.read(out);
            for(size_t i=0;i<fused.size();++i)expect(std::isfinite(fused[i]) && std::abs(fused[i]-legacy[i])<.0002f);
        }
    }
    std::puts("PASS: fused native spatial resolve matches legacy unpack/resolve, peripheral and foveated profiles (SDR/HDR, all reversible curves, strengths, skin protection, shifted periphery)");
    // ---- depth-aware enlargement of a reduced model: the ordinary resolve, a 32x16 model enlarged onto a 64x32 frame ----
    {
        namespace DA = DlssNr::DepthAware;
        constexpr unsigned FW = 64, FH = 32, MW = 32, MH = 16;
        // proxy and answer with a step at model texel 16 (frame pixel 32); the red channel carries it
        std::vector<float> proxyData(MW * MH * 4), answerData(MW * MH * 4), frameData(FW * FH * 4);
        for (unsigned y = 0; y < MH; ++y)
            for (unsigned x = 0; x < MW; ++x)
            {
                const auto i = (y * MW + x) * 4;
                proxyData[i] = x < 16 ? .1f : .6f; proxyData[i + 1] = .3f; proxyData[i + 2] = .3f; proxyData[i + 3] = 1;
                answerData[i] = x < 16 ? .2f : .8f; answerData[i + 1] = .35f; answerData[i + 2] = .32f; answerData[i + 3] = 1;
            }
        for (unsigned y = 0; y < FH; ++y)
            for (unsigned x = 0; x < FW; ++x)
            {
                const auto i = (y * FW + x) * 4;
                frameData[i] = .1f + .9f * x / FW; frameData[i + 1] = .2f + .5f * y / FH; frameData[i + 2] = .3f; frameData[i + 3] = .8f;
            }
        auto proxyTex = r.make(MW, MH, 4, proxyData), answerTex = r.make(MW, MH, 4, answerData), frameTex = r.make(FW, FH, 4, frameData);
        auto out = r.make(FW, FH, 4), keep = r.make(FW, FH, 4);

        // depth: a 20x12 texture whose valid 16x8 rectangle sits at (2,1), smaller than the frame; a step (near left, far right) at the middle
        // of the rectangle; the margin outside the rectangle holds a value that would give the wrong answer if the rectangle were ignored
        const float rect[4] = { 2, 1, 16, 8 };
        auto depthTexture = [&](bool inverted)
        {
            const float nearD = inverted ? .8f : .2f, farD = inverted ? .1f : .9f;
            std::vector<float> d(20 * 12, inverted ? 1.0f : 0.0f);
            for (unsigned y = 1; y < 9; ++y)
                for (unsigned x = 2; x < 18; ++x)
                    d[y * 20 + x] = (x - 2) < 8 ? nearD : farD;
            return r.make(20, 12, 1, d);
        };
        auto resolve = [&](unsigned shader, unsigned debugView, unsigned transfer, bool passthrough, unsigned reversible, unsigned residualWord,
                           Texture* depthTex, bool inverted, Texture* proxyIn = nullptr, Texture* answerIn = nullptr, const float* rectOverride = nullptr)
        {
            DlssNrConstants k {};
            k.Mode = DlssNrMode_Resolve; k.Width = FW; k.Height = FH; k.WhitePoint = 1;
            k.Passthrough = passthrough ? 1 : 0; k.ApplyModel = 1; k.Transfer = transfer; k.ReversibleMode = reversible;
            k.DebugView = debugView; k.DebugScale = 1; k.TransferStrength = 1; k.ColourStrength = 1; k.MaxRatio = 3;
            k.SpatialResidual = residualWord;
            if (residualWord & DA::kResolveBit)
                DA::PackFields(k.SpatialWarp, rectOverride ? rectOverride : rect, inverted);
            r.run(k, proxyIn ? *proxyIn : proxyTex, answerIn ? *answerIn : answerTex, frameTex, out, keep, shader, depthTex);
            return r.read(out);
        };
        const auto at = [&](const std::vector<float>& v, unsigned x, unsigned y) { return v[(y * FW + x) * 4]; };

        // 1. The feature off: bit for bit the resolve shader as it was before (every curve, both enlargement modes, SDR and HDR, every view)
        for (unsigned view : { 0u, 1u, 2u, 3u })
            for (unsigned transfer : { 0u, 1u })
                for (bool passthrough : { true, false })
                    for (unsigned reversible : { 0u, 1u, 5u })
                    {
                        const auto oldShader = resolve(3, view, transfer, passthrough, reversible, 0, nullptr, false);
                        const auto newShader = resolve(0, view, transfer, passthrough, reversible, 0, nullptr, false);
                        expect(oldShader.size() == newShader.size() &&
                               std::memcmp(oldShader.data(), newShader.data(), newShader.size() * sizeof(float)) == 0);
                    }
        // ... also when the model ran at the frame's size (the default configuration: nothing to enlarge)
        {
            std::vector<float> fullProxy(FW * FH * 4), fullAnswer(FW * FH * 4);
            for (unsigned y = 0; y < FH; ++y)
                for (unsigned x = 0; x < FW; ++x)
                {
                    const auto i = (y * FW + x) * 4;
                    fullProxy[i] = .1f + .5f * x / FW; fullProxy[i + 1] = .3f; fullProxy[i + 2] = .3f; fullProxy[i + 3] = 1;
                    fullAnswer[i] = fullProxy[i] + .04f * std::sin(x * .4f); fullAnswer[i + 1] = .33f; fullAnswer[i + 2] = .31f; fullAnswer[i + 3] = 1;
                }
            auto fullP = r.make(FW, FH, 4, fullProxy), fullA = r.make(FW, FH, 4, fullAnswer);
            auto depthTex = depthTexture(true);
            for (unsigned view : { 0u, 2u })
                for (unsigned transfer : { 0u, 1u })
                {
                    const auto oldShader = resolve(3, view, transfer, false, 0, 0, nullptr, false, &fullP, &fullA);
                    const auto newShader = resolve(0, view, transfer, false, 0, 0, nullptr, false, &fullP, &fullA);
                    expect(std::memcmp(oldShader.data(), newShader.data(), newShader.size() * sizeof(float)) == 0);
                    // and with the key on nothing changes there either: there is no enlargement to weight
                    const auto keyOn = resolve(0, view, transfer, false, 0, DA::kResolveBit, &depthTex, true, &fullP, &fullA);
                    expect(std::memcmp(oldShader.data(), keyOn.data(), keyOn.size() * sizeof(float)) == 0);
                }
        }
        std::puts("PASS: depth-aware enlargement off: the resolve is bit-identical to the shader before the feature");

        // 2. The step: pixels beside the edge keep the answer of their own side, with the depth read through the smaller offset rectangle
        for (bool inverted : { false, true })
        {
            auto depthTex = depthTexture(inverted);
            const auto off = resolve(0, 2, 1, true, 0, 0, nullptr, inverted);
            const auto on = resolve(0, 2, 1, true, 0, DA::kResolveBit, &depthTex, inverted);
            // the plain filter blends across the edge: pixel 31 is three quarters left and a quarter right, pixel 32 the reverse
            expect(std::abs(at(off, 31, 16) - (.75f * .2f + .25f * .8f)) < .002f && std::abs(at(off, 32, 16) - (.25f * .2f + .75f * .8f)) < .002f);
            // depth-aware: the near side stays at its own answer, the far side at its own
            expect(std::abs(at(on, 31, 16) - .2f) < .002f && std::abs(at(on, 32, 16) - .8f) < .002f);
            expect(std::abs(at(on, 30, 8) - .2f) < .002f && std::abs(at(on, 33, 24) - .8f) < .002f);
            // away from the edge nothing changes
            for (unsigned x : { 3u, 10u, 20u, 27u, 38u, 50u, 61u })
                expect(std::abs(at(on, x, 16) - at(off, x, 16)) < .002f);
            // the proxy is enlarged with the same weights (view 1), so the residual between them stays on its own side too
            const auto proxyOff = resolve(0, 1, 1, true, 0, 0, nullptr, inverted);
            const auto proxyOn = resolve(0, 1, 1, true, 0, DA::kResolveBit, &depthTex, inverted);
            expect(std::abs(at(proxyOff, 31, 16) - (.75f * .1f + .25f * .6f)) < .002f);
            expect(std::abs(at(proxyOn, 31, 16) - .1f) < .002f && std::abs(at(proxyOn, 32, 16) - .6f) < .002f);
            // both enlargement modes go through the weighted samples (the edit of a real resolve differs between the two sides only)
            for (unsigned transfer : { 0u, 1u })
            {
                const auto plain = resolve(0, 0, transfer, true, 0, 0, nullptr, inverted);
                const auto aware = resolve(0, 0, transfer, true, 0, DA::kResolveBit, &depthTex, inverted);
                expect(plain.size() == aware.size());
                bool differs = false;
                for (size_t i = 0; i < plain.size(); ++i)
                {
                    expect(std::isfinite(aware[i]));
                    differs = differs || std::abs(plain[i] - aware[i]) > .0005f;
                }
                expect(differs); // the weighting changed the picture beside the edge
            }
        }
        std::puts("PASS: depth-aware enlargement: no bleeding across a depth step, depth read through a smaller offset rectangle, both depth conventions, proxy and answer, both modes");

        // 3. A flat depth or an unusable one gives the plain bilinear result
        {
            std::vector<float> flat(20 * 12, .5f), bad(20 * 12, std::numeric_limits<float>::quiet_NaN());
            auto flatTex = r.make(20, 12, 1, flat), badTex = r.make(20, 12, 1, bad);
            const auto off = resolve(0, 2, 1, true, 0, 0, nullptr, true);
            const auto flatOn = resolve(0, 2, 1, true, 0, DA::kResolveBit, &flatTex, true);
            const auto badOn = resolve(0, 2, 1, true, 0, DA::kResolveBit, &badTex, true);
            for (size_t i = 0; i < off.size(); ++i)
                expect(std::abs(flatOn[i] - off[i]) < .002f && std::abs(badOn[i] - off[i]) < .002f);
        }
        std::puts("PASS: depth-aware enlargement: constant depth and invalid depth give the plain filter");

        // 4. With the spatial warp the feature is ignored: the selector bit changes nothing
        {
            Settings settings {};
            settings.enabled = true;
            settings.centerX = settings.centerY = 30; settings.workX = settings.workY = 50;
            auto layout = Build(settings, FW, FH, 1.0f);
            expect(layout.active);
            const auto warp = MakeResolveWarp(layout, true);
            auto depthTex = depthTexture(true);
            DlssNrConstants k {};
            k.Mode = DlssNrMode_Resolve; k.Width = FW; k.Height = FH; k.WhitePoint = 1; k.Passthrough = 1; k.ApplyModel = 1;
            k.Transfer = 1; k.TransferStrength = 1; k.ColourStrength = 1; k.MaxRatio = 3;
            k.SpatialResidual = warp.residual;
            k.SpatialWorkSize[0] = warp.workSize[0]; k.SpatialWorkSize[1] = warp.workSize[1];
            std::memcpy(k.SpatialWarp, warp.fields, sizeof(warp.fields));
            auto packedProxy = r.make(layout.modelW, layout.modelH, 4, std::vector<float>(layout.modelW * layout.modelH * 4, .4f));
            auto packedAnswer = r.make(layout.modelW, layout.modelH, 4, std::vector<float>(layout.modelW * layout.modelH * 4, .5f));
            r.run(k, packedProxy, packedAnswer, frameTex, out, keep, 0, &depthTex);
            const auto spatialOff = r.read(out);
            // the fused spatial resolve, key off: bit for bit what the shader did before the feature
            r.run(k, packedProxy, packedAnswer, frameTex, out, keep, 3, nullptr);
            const auto spatialBefore = r.read(out);
            expect(std::memcmp(spatialOff.data(), spatialBefore.data(), spatialBefore.size() * sizeof(float)) == 0);
            k.SpatialResidual = warp.residual | DA::kResolveBit;
            r.run(k, packedProxy, packedAnswer, frameTex, out, keep, 0, &depthTex);
            const auto spatialOn = r.read(out);
            expect(std::memcmp(spatialOff.data(), spatialOn.data(), spatialOn.size() * sizeof(float)) == 0);
        }
        std::puts("PASS: depth-aware enlargement is ignored while the spatial warp is active");

        // 5. Every pixel against the host reference (DepthAwareWeights.h: taps, depth texel mapping, weights) on scenes that discriminate:
        // a graded ramp, a horizontal and a vertical edge, the last column and row of an odd-sized depth rectangle, depth values that are
        // exactly 0 and 1, out-of-range and unusable texels next to usable ones, a thin feature whose four taps are all far in depth. The model
        // textures hold a different value in every texel and channel, so a wrong tap, weight or mapping shows in the pixel.
        {
            struct Scene
            {
                unsigned dw, dh;
                float rect[4];
                std::vector<float> depth; // raw depth (what the game's buffer holds), dw x dh
            };
            const auto texel = [](unsigned x, unsigned y, unsigned salt)
            { return .05f + .9f * float((x * 37u + y * 101u + salt * 53u) % 97u) / 96.0f; };
            // the depth of a disparity (large = near, in both conventions)
            const auto raw = [](float disparity, bool inverted) { return inverted ? disparity : 1.0f - disparity; };
            const float kNaN = std::numeric_limits<float>::quiet_NaN();
            const float noRect[4] = { 0, 0, FW, FH };

            struct Shape { unsigned mw, mh; };
            const Shape kShapes[] = { { 32, 16 }, { 32, 32 }, { 64, 16 } }; // reduced on both axes, on the width only, on the height only

            // a scene: disparity of each depth texel, inside the rectangle (outside it, the margin, a disparity of .5 that must never be read)
            const auto makeScene = [&](unsigned dw, unsigned dh, const float rect[4], bool inverted, auto&& disparityOf)
            {
                Scene s { dw, dh, { rect[0], rect[1], rect[2], rect[3] }, std::vector<float>(dw * dh, raw(.5f, inverted)) };
                for (unsigned y = unsigned(rect[1]); y < unsigned(rect[1] + rect[3]); ++y)
                    for (unsigned x = unsigned(rect[0]); x < unsigned(rect[0] + rect[2]); ++x)
                        s.depth[y * dw + x] = disparityOf(x - unsigned(rect[0]), y - unsigned(rect[1]), x, y, inverted);
                return s;
            };

            // Runs the shader on one scene and compares every pixel of the proxy (view 1) and of the model's answer (view 2) with the host. Returns
            // the number of pixels where the depth-aware result differs from the plain bilinear one (so a scene cannot pass by doing nothing).
            const auto compare = [&](const Scene& s, Shape shape, bool inverted, const char* label)
            {
                std::vector<float> p(shape.mw * shape.mh * 4), a(p.size());
                for (unsigned y = 0; y < shape.mh; ++y)
                    for (unsigned x = 0; x < shape.mw; ++x)
                        for (unsigned c = 0; c < 3; ++c)
                        {
                            p[(y * shape.mw + x) * 4 + c] = texel(x, y, c);
                            a[(y * shape.mw + x) * 4 + c] = texel(x, y, c + 3);
                            p[(y * shape.mw + x) * 4 + 3] = a[(y * shape.mw + x) * 4 + 3] = 1.0f;
                        }
                auto proxyTexture = r.make(shape.mw, shape.mh, 4, p), answerTexture = r.make(shape.mw, shape.mh, 4, a);
                auto depthTexture2 = r.make(s.dw, s.dh, 1, s.depth);
                const auto proxyOn = resolve(0, 1, 1, true, 0, DA::kResolveBit, &depthTexture2, inverted, &proxyTexture, &answerTexture, s.rect);
                const auto answerOn = resolve(0, 2, 1, true, 0, DA::kResolveBit, &depthTexture2, inverted, &proxyTexture, &answerTexture, s.rect);
                const auto depthAt = [&](float u, float v)
                {
                    int px = 0, py = 0;
                    DA::DepthPixel(u, v, s.rect, px, py);
                    return s.depth[unsigned(py) * s.dw + unsigned(px)];
                };
                unsigned differs = 0;
                for (unsigned y = 0; y < FH; ++y)
                    for (unsigned x = 0; x < FW; ++x)
                    {
                        const float u = (float(x) + .5f) / FW, v = (float(y) + .5f) / FH;
                        const DA::Taps taps = DA::BilinearTaps(u, v, shape.mw, shape.mh);
                        std::array<float, 4> tapDepth {};
                        for (size_t k = 0; k < 4; ++k)
                            tapDepth[k] = depthAt(DA::TapCentre(taps.x[k], shape.mw), DA::TapCentre(taps.y[k], shape.mh));
                        const auto w = DA::Weights(taps.bilinear, tapDepth, depthAt(u, v), inverted);
                        bool pixelDiffers = false;
                        for (unsigned c = 0; c < 3; ++c)
                            for (unsigned view = 0; view < 2; ++view)
                            {
                                float want = 0, plain = 0;
                                for (size_t k = 0; k < 4; ++k)
                                {
                                    const float value = texel(unsigned(taps.x[k]), unsigned(taps.y[k]), c + 3 * view);
                                    want += w[k] * value;
                                    plain += taps.bilinear[k] * value;
                                }
                                const float got = (view == 0 ? proxyOn : answerOn)[(y * FW + x) * 4 + c];
                                expectAt(std::isfinite(got) && std::abs(got - want) < 4e-4f, label, x, y, c + 3 * view, got, want);
                                pixelDiffers = pixelDiffers || std::abs(want - plain) > 5e-3f;
                            }
                        differs += pixelDiffers ? 1 : 0;
                    }
                return differs;
            };
            const auto must = [&](unsigned differs, unsigned atLeast, const char* label)
            {
                if (differs < atLeast)
                {
                    std::fprintf(stderr, "VACUOUS %s: %u pixels differ from the plain filter, expected at least %u\n", label, differs, atLeast);
                    throw std::runtime_error("Spatial shader reference mismatch");
                }
            };

            for (bool inverted : { false, true })
            {
                const char* convention = inverted ? "reversed depth" : "depth";
                // (a) a graded ramp of disparity along both axes, with a band at the far plane (disparity below the epsilon) on the left
                for (const Shape shape : kShapes)
                {
                    const auto ramp = makeScene(64, 32, noRect, inverted, [&](unsigned lx, unsigned ly, unsigned, unsigned, bool inv)
                                                { return raw(lx < 4 ? .0002f * float(lx + 1) : .12f + .7f * float(lx) / 63.0f + .12f * float(ly) / 31.0f, inv); });
                    must(compare(ramp, shape, inverted, "graded ramp"), 20, "graded ramp");
                    // (b) the last column and the last row of a depth rectangle whose size does not divide the frame (15 texels over 64 pixels, 8
                    // over 32), set apart from their neighbours: a pixel or a tap that reads one texel short of the rectangle's end is wrong
                    const float oddRect[4] = { 2, 1, 15, 8 };
                    const auto edgeScene = makeScene(20, 12, oddRect, inverted, [&](unsigned lx, unsigned ly, unsigned, unsigned, bool inv)
                                                     { return raw((lx == 14 || ly == 7) ? .9f : .1f + .01f * float(lx) + .005f * float(ly), inv); });
                    must(compare(edgeScene, shape, inverted, "last column and row of an odd-sized depth rectangle"), 1, "odd rectangle");
                    // (c) a vertical edge and a horizontal edge, through the offset rectangle {2,1,16,8} of a 20x12 depth texture. The horizontal
                    // one is constant along x, so the y mapping of the depth read is the only thing that places it.
                    const float sixteen[4] = { 2, 1, 16, 8 };
                    const auto vertical = makeScene(20, 12, sixteen, inverted, [&](unsigned lx, unsigned, unsigned, unsigned, bool inv) { return raw(lx < 8 ? .8f : .1f, inv); });
                    const auto horizontal = makeScene(20, 12, sixteen, inverted, [&](unsigned, unsigned ly, unsigned, unsigned, bool inv) { return raw(ly < 4 ? .8f : .1f, inv); });
                    must(compare(vertical, shape, inverted, "vertical edge"), shape.mw < FW ? 16u : 0u, "vertical edge");
                    must(compare(horizontal, shape, inverted, "horizontal edge"), shape.mh < FH ? 16u : 0u, "horizontal edge");
                }
                const Shape both { 32, 16 };
                // (d) depth values that are exactly 0 and exactly 1 on the two sides of an edge (both are usable); a tap or pixel with an
                // out-of-range depth (above 1, below 0) beside usable ones; NaN. Plain bilinear wherever a depth is unusable. The edge is at x = 32
                // and the pixel beside it reads the taps on both sides (model texel centres at frame x 31 and 33, rows 15 and 17 for y = 16).
                {
                    const auto special = makeScene(64, 32, noRect, inverted, [&](unsigned lx, unsigned ly, unsigned, unsigned, bool inv)
                    {
                        if (lx == 33 && ly == 17) return inv ? -0.5f : 1.5f;          // a tap above/below the usable range, far from the near side in disparity
                        if (lx == 33 && ly == 13) return inv ? 1.5f : -0.5f;          // ... and the other way round
                        if (lx == 31 && ly == 24) return inv ? 1.2f : -0.2f;          // the pixel's own depth unusable (disparity 1.2), taps on both sides of the edge
                        if (lx == 33 && ly == 21) return kNaN;                        // an unusable tap
                        if (lx == 31 && ly == 28) return kNaN;                        // an unusable pixel depth
                        return raw(lx < 32 ? 1.0f : 0.0f, inv);                       // exactly the two ends of the range
                    });
                    must(compare(special, both, inverted, "exact 0/1 depths, out-of-range and NaN texels"), 8, "special texels");
                }
                // (e) a thin near object: the pixel (30,16) sits on a one-texel feature and its four taps (29/31 x 15/17) are all far from it in depth, at
                // different depths, so the weights add up to less than the threshold and the plain filter is used. The pixel (50,16) has its four taps
                // at moderate distances: the weights add up to a few thousandths, above the threshold, and are used normalised.
                {
                    const auto thin = makeScene(64, 32, noRect, inverted, [&](unsigned lx, unsigned ly, unsigned, unsigned, bool inv)
                    {
                        float d = .3f;
                        if (lx == 30 && ly == 16) d = .9f;
                        if (lx == 29 && ly == 15) d = .0f;
                        if (lx == 31 && ly == 15) d = .05f;
                        if (lx == 29 && ly == 17) d = .02f;
                        if (lx == 31 && ly == 17) d = .0f;
                        if (lx == 50 && ly == 16) d = .9f;
                        if (lx == 49 && ly == 15) d = .40f;
                        if (lx == 51 && ly == 15) d = .45f;
                        if (lx == 49 && ly == 17) d = .35f;
                        if (lx == 51 && ly == 17) d = .42f;
                        return raw(d, inv);
                    });
                    must(compare(thin, both, inverted, "thin feature"), 1, "thin feature");
                    // the host side of the two pixels: under the threshold (plain bilinear back), and between the threshold and 1e-2 (normalised)
                    const auto weightsAt = [&](unsigned x, unsigned y)
                    {
                        const float u = (float(x) + .5f) / FW, v = (float(y) + .5f) / FH;
                        const DA::Taps taps = DA::BilinearTaps(u, v, both.mw, both.mh);
                        std::array<float, 4> tapDepth {};
                        for (size_t k = 0; k < 4; ++k)
                        {
                            int px = 0, py = 0;
                            DA::DepthPixel(DA::TapCentre(taps.x[k], both.mw), DA::TapCentre(taps.y[k], both.mh), thin.rect, px, py);
                            tapDepth[k] = thin.depth[unsigned(py) * thin.dw + unsigned(px)];
                        }
                        int qx = 0, qy = 0;
                        DA::DepthPixel(u, v, thin.rect, qx, qy);
                        const float pixelDepth = thin.depth[unsigned(qy) * thin.dw + unsigned(qx)];
                        float sum = 0;
                        for (size_t k = 0; k < 4; ++k)
                            sum += taps.bilinear[k] * DA::Similarity(pixelDepth, tapDepth[k], inverted);
                        return std::pair { DA::Weights(taps.bilinear, tapDepth, pixelDepth, inverted) == taps.bilinear, sum };
                    };
                    const auto under = weightsAt(30, 16), between = weightsAt(50, 16);
                    expect(under.first && under.second > 1e-6f && under.second < DA::kMinWeightSum);
                    expect(!between.first && between.second > DA::kMinWeightSum && between.second < 1e-2f);
                }
                std::printf("PASS: depth-aware enlargement matches the host reference pixel by pixel (%s): ramp, odd-sized rectangle, edges, exact and unusable depths, thin feature\n", convention);
            }
        }
    }
    return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
