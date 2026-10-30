#pragma once
#include "ext/hdr/SceneInput.h"
#include "ext/hdr/UiBlend.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <array>

// Accumulate native UI transmittance, including atlas-backed panels behind
// rendered previews. RGB remains in the native SDR chain; this mask only tells
// HDR composition how much of the original world can pass through the UI.
namespace ext::hdr::FfxivHdrPreview {
using Microsoft::WRL::ComPtr;
// Why a tagged draw did not reach the mask (HDR census, one counter each). The order is the order of the checks in BeforeDraw.
enum class Skip : uint8_t {
    Sealed,               // the frame was already handed to the converter
    NotMarked,            // the pixel shader carries no UI tag
    NoSource,             // no single-sample 2D texture bound at t0..t7 (nothing to read opacity from)
    NoTarget,             // no render target, or it is not a 2D texture
    TargetUnsuitable,     // target below 640x360, multisampled, or the draw reads the target it writes
    BlendAlphaToCoverage, // uiblend::Refusal::AlphaToCoverage
    BlendWriteMask,       // uiblend::Refusal::WriteMask
    BlendAdditive,        // uiblend::Refusal::Additive (glows)
    BlendOther,           // uiblend::Refusal::OtherBlend
    Predicated,
    MultipleTargets,
    StateFailed,          // a blend or depth state could not be created, or the depth state cache is full
    Offscreen,            // a smaller target than the frame's mask (offscreen UI rendered before the screen composition)
    NoSlot,               // all mask textures busy or the texture could not be created
    SizeMismatch,
    Count
};
inline constexpr unsigned kSkipCount=static_cast<unsigned>(Skip::Count);
inline const char* SkipName(Skip s) {
    static constexpr const char* names[kSkipCount]{"sealed","not_marked","no_source","no_target","target_unsuitable","blend_alpha_to_coverage",
        "blend_write_mask","blend_additive","blend_other","predicated","multiple_targets","state_failed","offscreen","no_slot","size_mismatch"};
    return static_cast<unsigned>(s)<kSkipCount?names[static_cast<unsigned>(s)]:"?";
}
inline Skip SkipOf(uiblend::Refusal r) {
    switch(r) {
    case uiblend::Refusal::AlphaToCoverage:return Skip::BlendAlphaToCoverage;
    case uiblend::Refusal::WriteMask:return Skip::BlendWriteMask;
    case uiblend::Refusal::Additive:return Skip::BlendAdditive;
    default:return Skip::BlendOther;
    }
}
// Cumulative counters of the tagged UI draws that reached BeforeDraw (the call site only passes draws of the UI shader set).
// Relaxed atomics: BeforeDraw runs on the render thread, the summary is read where the bridge frame ends.
struct Census {
    std::atomic<uint64_t> draws{0},replayed{0},frames{0},framesWithMask{0};
    std::atomic<uint64_t> skipped[kSkipCount]{};
    std::atomic<bool> summaryDue{false};
};
inline constexpr uint64_t kSummaryFrames=600;
// One log line: frames seen, frames that ended with a mask, tagged draws seen, replayed, refused with the reasons that occurred.
inline std::string SummaryText(uint64_t frames,uint64_t framesWithMask,uint64_t draws,uint64_t replayed,const uint64_t (&skipped)[kSkipCount]) {
    uint64_t refused=0;for(auto n:skipped)refused+=n;
    char head[192];
    std::snprintf(head,sizeof head,"HDR UI mask: %llu frames (%llu with a mask), %llu tagged draws, %llu replayed, %llu refused",
        static_cast<unsigned long long>(frames),static_cast<unsigned long long>(framesWithMask),static_cast<unsigned long long>(draws),
        static_cast<unsigned long long>(replayed),static_cast<unsigned long long>(refused));
    std::string text=head;bool first=true;
    for(unsigned i=0;i<kSkipCount;++i)if(skipped[i]) {
        char item[64];std::snprintf(item,sizeof item,"%s%s %llu",first?" (":", ",SkipName(static_cast<Skip>(i)),static_cast<unsigned long long>(skipped[i]));
        text+=item;first=false;
    }
    if(!first)text+=')';
    return text;
}
inline constexpr GUID tag{0x69ac2fe2,0x65cf,0x4290,{0x87,0x26,0x31,0x0b,0x0c,0x54,0x12,0x92}};
// Census gate: the counters below, the per-frame TakeSummary trigger and the 600-frame log line run only while
// [HudLess] Diagnostics is on. Off by default (a Mode 1 frame then costs what it did before the census); the bridge
// frame end refreshes it from the config, so toggling the key takes effect on the next frame.
inline std::atomic<bool>& CensusGate() { static auto* on = new std::atomic<bool>(false); return *on; }
inline void SetCensusEnabled(bool on) { CensusGate().store(on, std::memory_order_relaxed); }
inline bool CensusEnabled() { return CensusGate().load(std::memory_order_relaxed); }
inline bool Tag(ID3D11PixelShader* shader,UINT crc) {
    switch(crc) {
    case 3865947726u:case 2966694105u:case 3759127293u:case 1898885941u:
    case 4076256744u:case 3915247300u:case 1823062889u: {
        UINT value=1;return SUCCEEDED(shader->SetPrivateData(tag,sizeof(value),&value));
    }
    }
    return false;
}
struct Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D12Resource> opened;
    ComPtr<ID3D12Device> openedDevice;
    std::function<bool()> done;
    UINT width=0,height=0;bool recorded=false;
};
struct Data {
    std::mutex mutex;ComPtr<ID3D11Device> device;ComPtr<ID3D11BlendState> blend,opaqueBlend;
    struct DepthState {ComPtr<ID3D11DepthStencilState> native,readOnly;};
    std::vector<DepthState> depthStates;
    std::vector<std::shared_ptr<Slot>> slots;std::shared_ptr<Slot> current;
    uint64_t draws=0;bool sealed=false;Census census;

};
inline Data& State(){static auto* value=new Data;return *value;}
inline void Note(Data& s,Skip reason){if(CensusEnabled())++s.census.skipped[static_cast<unsigned>(reason)];}
template<class F> inline void BeforeDraw(ID3D11DeviceContext* c,F&& draw) {
    {auto& s=State();if(CensusEnabled())++s.census.draws;std::lock_guard lock(s.mutex);if(s.sealed){Note(s,Skip::Sealed);return;}}
    ComPtr<ID3D11PixelShader> shader;c->PSGetShader(&shader,nullptr,nullptr);
    UINT marked=0,size=sizeof(marked);
    if(!shader || FAILED(shader->GetPrivateData(tag,&size,&marked)) || !marked){Note(State(),Skip::NotMarked);return;}
    std::array<ComPtr<ID3D11ShaderResourceView>,8> inputs;ID3D11ShaderResourceView* rawInputs[8]{};
    c->PSGetShaderResources(0,8,rawInputs);for(UINT i=0;i<8;++i)inputs[i].Attach(rawInputs[i]);
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> source;D3D11_TEXTURE2D_DESC sd{};
    for(UINT i=0;i<8;++i)if(inputs[i]) {
        inputs[i]->GetResource(&resource);ComPtr<ID3D11Texture2D> t;if(FAILED(resource.As(&t)))continue;
        D3D11_TEXTURE2D_DESC desc{};t->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1)continue;
        if(!source || UINT64(desc.Width)*desc.Height>UINT64(sd.Width)*sd.Height){source=t;sd=desc;}
    }
    if(!source){Note(State(),Skip::NoSource);return;}
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;
    c->OMGetRenderTargets(1,&target,&depth);if(!target){Note(State(),Skip::NoTarget);return;}
    target->GetResource(&resource);ComPtr<ID3D11Texture2D> native;if(FAILED(resource.As(&native))){Note(State(),Skip::NoTarget);return;}
    D3D11_TEXTURE2D_DESC td{};native->GetDesc(&td);
    if(td.Width<640 || td.Height<360 || td.SampleDesc.Count!=1 || source==native){Note(State(),Skip::TargetUnsuitable);return;}
    // Replay BEFORE the native draw, with identical clipping/tests but no
    // depth/stencil writes. The actual native draw then runs exactly once.
    ComPtr<ID3D11DepthStencilState> nativeDepth;UINT stencilRef=0;
    D3D11_DEPTH_STENCIL_DESC dd{};
    if(depth) {
        c->OMGetDepthStencilState(&nativeDepth,&stencilRef);
        if(nativeDepth)nativeDepth->GetDesc(&dd);
        else {dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;dd.StencilReadMask=dd.StencilWriteMask=255;dd.FrontFace=dd.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};}
    }
    ComPtr<ID3D11BlendState> original;FLOAT factor[4]{};UINT sampleMask=0;
    c->OMGetBlendState(&original,factor,&sampleMask);
    D3D11_BLEND_DESC bd{};if(original)original->GetDesc(&bd);
    else bd.RenderTarget[0].RenderTargetWriteMask=15;
    // The refusal rule lives in UiBlend.h (shared with the UI draw census); with no blend state bound the D3D11 default applies.
    const auto verdict=uiblend::Classify(original?&bd:nullptr);
    const bool opaque=verdict.Opaque();
    auto& s=State();
    if(!verdict.Replayable()){Note(s,SkipOf(verdict.refusal));return;}
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;c->GetPredication(&predicate,&predicateValue);if(predicate){Note(s,Skip::Predicated);return;}
    std::array<ComPtr<ID3D11RenderTargetView>,8> targets;ID3D11RenderTargetView* raw[8]{};
    c->OMGetRenderTargets(8,raw,nullptr);for(UINT i=0;i<8;++i)targets[i].Attach(raw[i]);
    for(UINT i=1;i<8;++i)if(raw[i]){Note(s,Skip::MultipleTargets);return;}
    ComPtr<ID3D11Device> device;c->GetDevice(&device);std::lock_guard lock(s.mutex);
    if(s.device!=device){s.device=device;s.blend.Reset();s.opaqueBlend.Reset();s.depthStates.clear();s.current.reset();s.slots.clear();}
    auto& maskBlend=opaque?s.opaqueBlend:s.blend;
    if(!maskBlend) {
        D3D11_BLEND_DESC b{};auto& r=b.RenderTarget[0];r.BlendEnable=true;
        r.SrcBlend=D3D11_BLEND_ZERO;r.DestBlend=opaque?D3D11_BLEND_ZERO:D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha=D3D11_BLEND_ZERO;r.DestBlendAlpha=opaque?D3D11_BLEND_ZERO:D3D11_BLEND_INV_SRC_ALPHA;
        r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(device->CreateBlendState(&b,&maskBlend))){Note(s,Skip::StateFailed);return;}
    }
    ComPtr<ID3D11DepthStencilState> readOnly;
    if(depth) {
        for(auto& entry:s.depthStates)if(entry.native==nativeDepth){readOnly=entry.readOnly;break;}
        if(!readOnly) {
            if(s.depthStates.size()>=32){Note(s,Skip::StateFailed);return;}
            dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.StencilWriteMask=0;
            if(FAILED(device->CreateDepthStencilState(&dd,&readOnly))){Note(s,Skip::StateFailed);return;}
            s.depthStates.push_back({nativeDepth,readOnly});
        }
    }
    if(s.current && (s.current->width!=td.Width || s.current->height!=td.Height)) {
        // Offscreen UI can render before the full-screen composition. It must
        // not occupy the frame's mask and block later screen-size previews.
        if(UINT64(td.Width)*td.Height<=UINT64(s.current->width)*s.current->height){Note(s,Skip::Offscreen);return;}
        s.current.reset();
    }
    if(!s.current) {
        for(auto& p:s.slots)if(p.use_count()==1 && ReusableRecordedWork(p->recorded,p->done)){s.current=p;break;}
        if(!s.current){if(s.slots.size()>=6){Note(s,Skip::NoSlot);return;}s.current=std::make_shared<Slot>();s.slots.push_back(s.current);}
        auto& p=*s.current;
        p.done={};p.recorded=false;
        if(!p.texture || p.width!=td.Width || p.height!=td.Height) {
            p=Slot{};D3D11_TEXTURE2D_DESC d{};d.Width=td.Width;d.Height=td.Height;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if(FAILED(device->CreateTexture2D(&d,nullptr,&p.texture)) ||
               FAILED(device->CreateRenderTargetView(p.texture.Get(),nullptr,&p.rtv))){s.current.reset();Note(s,Skip::NoSlot);return;}
            p.width=d.Width;p.height=d.Height;
        }
        const FLOAT clear[4]{1,1,1,1};c->ClearRenderTargetView(p.rtv.Get(),clear);
    }
    auto& p=*s.current;if(p.width!=td.Width || p.height!=td.Height){Note(s,Skip::SizeMismatch);return;}
    c->OMSetRenderTargets(1,p.rtv.GetAddressOf(),depth.Get());c->OMSetBlendState(maskBlend.Get(),factor,sampleMask);
    if(depth)c->OMSetDepthStencilState(readOnly.Get(),stencilRef);
    draw();
    c->OMSetRenderTargets(8,raw,depth.Get());c->OMSetBlendState(original.Get(),factor,sampleMask);
    if(depth)c->OMSetDepthStencilState(nativeDepth.Get(),stencilRef);
    ++s.draws;if(CensusEnabled())++s.census.replayed;
}
// Freeze the mask at the same frame boundary as the SDR backbuffer, before
// the bridge signals its DX11 fence. Importing never records native GPU work.
inline void SealFrame() {
    auto& s=State();std::lock_guard lock(s.mutex);s.sealed=true;
}
inline SceneInput Attach(SceneInput image,ID3D12Device* device,UINT w,UINT h) {
    auto& s=State();std::lock_guard lock(s.mutex);auto p=s.current;
    if(!image.hdr || !p || p->width!=w || p->height!=h)return image;
    if(p->openedDevice.Get()!=device) {
        ComPtr<IDXGIResource1> resource;if(FAILED(p->texture.As(&resource)))return image;
        HANDLE handle=nullptr;if(FAILED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return image;
        ComPtr<ID3D12Resource> opened;auto hr=device->OpenSharedHandle(handle,IID_PPV_ARGS(&opened));CloseHandle(handle);
        if(FAILED(hr))return image;p->opened=opened;p->openedDevice=device;
    }
    struct Owners {std::shared_ptr<void> scene;std::shared_ptr<Slot> preview;};
    image.owner=std::make_shared<Owners>(Owners{image.owner,p});image.previewMask=p->opened;
    auto previous=image.retire;image.retire=[previous,p](std::function<bool()> done) {
        if(previous)previous(done);auto& state=State();std::lock_guard lock(state.mutex);
        // Multiple conversions can read this frame; all must finish before reuse.
        auto older=p->done;p->done=[older,done]{return (!older || older()) && done && done();};p->recorded=true;
    };
    return image;
}
// Called when the bridge frame ends. Counts the frame (and whether it carried a mask) and flags the 600-frame summary.
inline void EndFrame(){
    auto& s=State();std::lock_guard lock(s.mutex);
    if(CensusEnabled()){++s.census.frames;if(s.current)++s.census.framesWithMask;if(s.census.frames%kSummaryFrames==0)s.census.summaryDue=true;}
    s.current.reset();s.sealed=false;
}
// The summary line when 600 more frames have passed and tagged draws were seen (never in an SDR session: BeforeDraw is only
// reached with HDR scene mode on). Call it outside any lock and log the result.
inline bool TakeSummary(std::string& line) {
    if(!CensusEnabled())return false;
    auto& c=State().census;
    if(!c.summaryDue.exchange(false) || c.draws.load()==0)return false;
    uint64_t skipped[kSkipCount];for(unsigned i=0;i<kSkipCount;++i)skipped[i]=c.skipped[i].load();
    line=SummaryText(c.frames.load(),c.framesWithMask.load(),c.draws.load(),c.replayed.load(),skipped);return true;
}
}
