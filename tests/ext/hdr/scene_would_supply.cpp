#include "stubs/pch.h"
#include <d3d11.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "ext/hdr/scene/FfxivSceneHdr.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")

// ISceneHdr::WouldSupplyScene (SceneHooks.cpp forwards it to FfxivSceneHdr::WouldSupply): the side-effect-free twin of
// Open(.., presentation = true). It must agree with Open over every case, and it must leave the request / fallback
// statistics, the cached import and the frame state of the scene provider exactly as they were.
namespace ext::hdr { bool g_active = true; bool Active(){return g_active;} }
using Microsoft::WRL::ComPtr;
namespace S = ext::hdr::FfxivSceneHdr;
void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D failure: "+std::to_string(unsigned(hr)));}
void Expect(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}

struct Counters {
    uint64_t requests,available,missing,aspectMismatch,importFailed;
    bool operator==(const Counters&) const = default;
};
static Counters Count(S::Data& s){return {s.requests,s.available,s.missing,s.aspectMismatch,s.importFailed};}

// Everything WouldSupply must not change
struct Snapshot {
    Counters counters;
    const void* latest;long latestUses;
    const void* openedDevice;const void* openedHdr;const void* openedReference;
    bool reserved,dx12Recorded;ULONGLONG lastUse;
    const void* reconstructed;size_t slots;
};
static Snapshot Take(S::Data& s){
    std::lock_guard lock(s.mutex);
    Snapshot v{};v.counters=Count(s);v.latest=s.latest.get();v.latestUses=s.latest.use_count();
    if(s.latest){v.openedDevice=s.latest->openedDevice.Get();v.openedHdr=s.latest->openedHdr.Get();v.openedReference=s.latest->openedReference.Get();
        v.reserved=s.latest->reserved;v.dx12Recorded=s.latest->dx12Recorded;v.lastUse=s.latest->lastUse;}
    v.reconstructed=s.reconstructed.hdr.Get();v.slots=s.slots.size();return v;
}
static bool Same(const Snapshot& a,const Snapshot& b){
    return a.counters==b.counters && a.latest==b.latest && a.latestUses==b.latestUses && a.openedDevice==b.openedDevice &&
        a.openedHdr==b.openedHdr && a.openedReference==b.openedReference && a.reserved==b.reserved &&
        a.dx12Recorded==b.dx12Recorded && a.lastUse==b.lastUse && a.reconstructed==b.reconstructed && a.slots==b.slots;
}

int main() try {
    ext::Cfg().hdr.ffxivHdr = true; ext::Cfg().hdr.mode = 1;
    ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> c;
    Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c));
    ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> dx12;Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dx12)));
    auto& state=S::State();

    // A capture slot as AfterDraw leaves it (the same Allocate), without needing the replay shaders.
    auto makeSlot=[&](UINT w,UINT h,UINT viewportW,UINT viewportH){
        auto slot=std::make_shared<S::Slot>();
        D3D11_TEXTURE2D_DESC input{};input.Width=w;input.Height=h;input.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        Expect(S::Allocate(*slot,d11.Get(),input),"slot allocation");
        slot->viewport={0,0,float(viewportW),float(viewportH),0,1};
        slot->crop={0,0,float(viewportW),float(viewportH)};slot->stage=1;slot->lastUse=GetTickCount64();
        return slot;};
    auto setLatest=[&](std::shared_ptr<S::Slot> slot){std::lock_guard lock(state.mutex);state.slots={slot};state.latest=slot;};
    auto clearFrame=[&]{std::lock_guard lock(state.mutex);state.latest.reset();state.slots.clear();state.reconstructed={};};
    auto reconstructed=[&](UINT w,UINT h){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=w;rd.Height=h;rd.DepthOrArraySize=1;rd.MipLevels=1;
        rd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ext::hdr::SceneInput image;
        Check(dx12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&image.hdr)));
        return image;};

    // The one comparison every case makes: ask WouldSupply first, check it changed nothing, then ask Open (the real thing,
    // which does report) and require the same answer. Returns the answer.
    int cases=0;
    auto agree=[&](const char* name,UINT w,UINT h,bool expected){
        ++cases;std::printf("  case %s\n",name);std::fflush(stdout);
        const auto before=Take(state);
        bool would=false;
        for(int i=0;i<3;++i)would=S::WouldSupply(w,h); // repeated calls are as inert as one
        const auto after=Take(state);
        Expect(Same(before,after),std::string(name)+": WouldSupply changed counters or state");
        Expect(would==expected,std::string(name)+": WouldSupply answered "+(would?"true":"false"));
        const auto opened=S::Open(dx12.Get(),w,h,true);
        Expect((opened.hdr!=nullptr)==would,std::string(name)+": Open disagrees with WouldSupply ("+(opened.hdr?"scene":"empty")+")");
        return Take(state);
    };
    auto delta=[&](const Snapshot& a,const Snapshot& b){ // which statistic Open moved
        return std::array<uint64_t,5>{b.counters.requests-a.counters.requests,b.counters.available-a.counters.available,
            b.counters.missing-a.counters.missing,b.counters.aspectMismatch-a.counters.aspectMismatch,b.counters.importFailed-a.counters.importFailed};};
    using D=std::array<uint64_t,5>;

    // 1. scene HDR not requested (mode 0, then the game module inactive): empty / false and Open reports nothing either
    {
        ext::Cfg().hdr.mode=0;auto base=Take(state);
        auto after=agree("mode 0",3840,2160,false);Expect(delta(base,after)==D{0,0,0,0,0},"mode 0 reported a request");
        ext::Cfg().hdr.mode=1;ext::hdr::g_active=false;
        after=agree("inactive",3840,2160,false);Expect(delta(base,after)==D{0,0,0,0,0},"inactive reported a request");
        ext::hdr::g_active=true;
        ext::Cfg().hdr.ffxivHdr=false;
        after=agree("ffxivHdr off",3840,2160,false);Expect(delta(base,after)==D{0,0,0,0,0},"ffxivHdr off reported a request");
        ext::Cfg().hdr.ffxivHdr=true;
    }
    // 2. zero size
    {
        auto base=Take(state);auto after=agree("zero width",0,2160,false);Expect(delta(base,after)==D{0,0,0,0,0},"zero width reported");
        after=agree("zero height",3840,0,false);Expect(delta(base,after)==D{0,0,0,0,0},"zero height reported");
    }
    // 3. requested, no capture this frame: Open counts one "missing", WouldSupply none
    {
        clearFrame();auto base=Take(state);auto after=agree("no capture",3840,2160,false);
        Expect(delta(base,after)==D{1,0,1,0,0},"no capture: Open should count one missing request");
    }
    // 4. a same-frame capture with a matching aspect, not imported yet: WouldSupply true without importing, Open imports
    {
        auto slot=makeSlot(1280,720,1280,720);setLatest(slot);
        Expect(!slot->openedDevice,"fresh slot is not imported");
        auto base=Take(state);auto after=agree("capture, not imported",3840,2160,true);
        Expect(delta(base,after)==D{1,1,0,0,0},"capture: Open should count one available scene");
        Expect(after.openedDevice!=nullptr,"Open imports the slot (WouldSupply did not do it before)");
        // 5. the same slot again: already imported, still supplied
        base=Take(state);after=agree("capture, imported",1920,1080,true);
        Expect(delta(base,after)==D{1,1,0,0,0},"imported capture: one more available scene");
        // 6. aspect mismatch against the capture's viewport
        base=Take(state);after=agree("aspect mismatch",640,640,false);
        Expect(delta(base,after)==D{1,0,0,1,0},"aspect mismatch: Open should count one mismatch");
        after=agree("aspect mismatch (ultrawide)",3440,1440,false);
        // the capture's viewport aspect tolerance (0.01) is the same in both
        base=Take(state);after=agree("aspect within tolerance",1920,1081,true);
        Expect(delta(base,after)==D{1,1,0,0,0},"aspect within tolerance is a scene");
    }
    // 7. a viewport smaller than its texture (padded target): the viewport aspect decides, as in Open
    {
        auto slot=makeSlot(1024,512,640,360);setLatest(slot);
        agree("padded capture, matching viewport aspect",3840,2160,true);
        agree("padded capture, texture aspect",1024,512,false);
    }
    // 8. import precondition: the reference image is not NT-handle shareable -> Open counts an import failure, no scene
    {
        auto slot=makeSlot(1280,720,1280,720);
        D3D11_TEXTURE2D_DESC td{};slot->reference->GetDesc(&td);td.MiscFlags=0;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        slot->reference.Reset();Check(d11->CreateTexture2D(&td,nullptr,&slot->reference));
        setLatest(slot);auto base=Take(state);auto after=agree("unshareable reference",3840,2160,false);
        Expect(delta(base,after)==D{1,0,0,0,1},"unshareable reference: Open should count one import failure");
        // the same for the HDR image
        auto slot2=makeSlot(1280,720,1280,720);
        slot2->hdr->GetDesc(&td);td.MiscFlags=0;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        slot2->hdr.Reset();Check(d11->CreateTexture2D(&td,nullptr,&slot2->hdr));
        setLatest(slot2);base=Take(state);after=agree("unshareable hdr",3840,2160,false);
        Expect(delta(base,after)==D{1,0,0,0,1},"unshareable hdr: Open should count one import failure");
    }
    // 9. a reconstructed display image published this frame wins, whatever the capture says (and Open does not report)
    {
        clearFrame();
        { std::lock_guard lock(state.mutex);state.reconstructed=reconstructed(3840,2160); }
        auto base=Take(state);auto after=agree("reconstructed, no capture",3840,2160,true);
        Expect(delta(base,after)==D{0,0,0,0,0},"a reconstructed image is not a counted request");
        // size mismatch: not the reconstructed image; with no capture it is a missing scene
        base=Take(state);after=agree("reconstructed of another size, no capture",1920,1080,false);
        Expect(delta(base,after)==D{1,0,1,0,0},"reconstructed of another size: missing");
        // size mismatch but a matching capture exists: the capture supplies it
        setLatest(makeSlot(1280,720,1280,720));
        base=Take(state);after=agree("reconstructed of another size, capture",1920,1080,true);
        Expect(delta(base,after)==D{1,1,0,0,0},"capture supplies when the reconstructed size does not match");
        // matching reconstructed + a capture of another aspect: the reconstructed image still wins
        base=Take(state);after=agree("reconstructed wins over a mismatching capture",3840,2160,true);
        Expect(delta(base,after)==D{0,0,0,0,0},"reconstructed wins without a request");
        setLatest(makeSlot(1280,720,1280,720));
        { std::lock_guard lock(state.mutex);state.slots.front()->viewport={0,0,640,640,0,1}; }
        base=Take(state);after=agree("reconstructed wins over an aspect-mismatching capture",3840,2160,true);
        Expect(delta(base,after)==D{0,0,0,0,0},"reconstructed wins without a request (aspect case)");
    }
    // 10. EndFrame forgets the frame: nothing is supplied from a previous frame
    {
        S::EndFrame();auto base=Take(state);auto after=agree("after EndFrame",3840,2160,false);
        Expect(delta(base,after)==D{1,0,1,0,0},"after EndFrame: missing");
    }
    // 11. many calls leave a long statistics run exactly as Open alone would (no request is added by the twin)
    {
        setLatest(makeSlot(1280,720,1280,720));
        const auto base=Take(state);
        for(int i=0;i<2000;++i)Expect(S::WouldSupply(3840,2160),"steady answer");
        Expect(Same(base,Take(state)),"2000 WouldSupply calls changed the provider state");
    }
    std::printf("PASS: ISceneHdr::WouldSupplyScene agrees with Open(presentation) over %d cases (not requested, zero size, no capture, aspect, import precondition, reconstructed) and touches no counter or state\n",cases);
    return 0;
}catch(const std::exception& e){std::puts(e.what());return 1;}
