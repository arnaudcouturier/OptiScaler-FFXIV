#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <detours/detours.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include "../../core/Crc32.h"
#include "ext/core/DetourThreads.h"
#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/lighting/FfxivLightingScan.h"
#include "ext/ffxiv/ui/FfxivUiBoundary.h" // ext: S10 A hook consumer
#include "ext/hdr/FfxivHdrPreview.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"

// Native exposure scan hooks. Signatures identify the game's
// shaders, extracted read-only from SqPack on 2026-09-23. No engine RVA is hooked.
// Verified tone response controls history rejection, never an exposure multiply.
namespace ext::ffxiv::lighting::FfxivLightingCapture
{
using Microsoft::WRL::ComPtr;
using IndexedOriginal = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawCall = void (*)(void* state);

// Companion observation is supplied by C7 when its module is selected. Keeping
// these callbacks optional lets the native lighting hooks remain independently
// usable and avoids an allocation or cross-feature include on draw paths.
struct CompanionCallbacks
{
    void (*tagShader)(ID3D11PixelShader*, const void*, SIZE_T) = nullptr;
    void (*tagPixel)(ID3D11PixelShader*, const void*, SIZE_T) = nullptr;
    void (*tagLayout)(ID3D11InputLayout*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T) = nullptr;
    void (*observeDraw)(ID3D11DeviceContext*) = nullptr;
    bool (*interceptVisibilityIndexed)(ID3D11DeviceContext*, UINT, UINT, INT, IndexedOriginal) = nullptr;
    void (*aroundIndexed)(ID3D11DeviceContext*, UINT, UINT, DrawCall, void*) = nullptr;
};
inline CompanionCallbacks companionCallbacks {};
inline void SetCompanionCallbacks(CompanionCallbacks callbacks) noexcept { companionCallbacks = callbacks; }

struct Shader { const char* name; UINT bytes, crc; std::array<unsigned char,16> checksum; };
inline constexpr std::array<Shader,9> shaders {{
    {"ToneMapping", 1512u, 2246539247u, {0x47, 0x3c, 0x73, 0x0f, 0x5e, 0xe5, 0xea, 0xfb, 0x3e, 0x98, 0x65, 0xb7, 0xb3, 0x4a, 0x61, 0x71}},
    {"ToneAdjust", 1292u, 4142406171u, {0x99, 0xaa, 0x14, 0xeb, 0x1f, 0x12, 0xd7, 0xe1, 0x6f, 0xa5, 0xd6, 0x48, 0xe3, 0x28, 0xd2, 0xf7}},
    {"MeasureLumInitial", 1504u, 3549584390u, {0x99, 0xfb, 0xff, 0x98, 0xca, 0xc9, 0xac, 0x5c, 0x53, 0xe0, 0xe4, 0x90, 0x66, 0x1e, 0x6b, 0x2e}},
    {"MeasureLumIterative", 996u, 3365552696u, {0x57, 0xc0, 0x2e, 0x53, 0xea, 0x9c, 0xb8, 0x4d, 0xdb, 0x25, 0xa7, 0x88, 0x62, 0x93, 0x04, 0x8f}},
    {"MeasureLumFinal", 1120u, 3429323786u, {0xf7, 0xc4, 0x0e, 0x83, 0x23, 0x87, 0xd5, 0xe9, 0xb4, 0x76, 0x87, 0x24, 0x4f, 0x07, 0x9b, 0xc3}},
    {"AdaptLum", 1376u, 3046182588u, {0x7b, 0x59, 0x78, 0x4f, 0xed, 0xd5, 0xee, 0x80, 0x96, 0xf3, 0xce, 0xbc, 0x80, 0x2f, 0x1d, 0xb1}},
    {"BrightPassFilter", 1424u, 2198444491u, {0x19, 0xbd, 0x6f, 0x0e, 0x5d, 0xf4, 0xbb, 0x62, 0xd7, 0x83, 0x76, 0xe5, 0xf0, 0xa9, 0x17, 0x34}},
    {"BrightPassFilterUpdate", 1444u, 3541952783u, {0x54, 0x34, 0xf1, 0x09, 0x15, 0x76, 0xae, 0x70, 0x3a, 0x12, 0x64, 0x31, 0x03, 0x05, 0x7c, 0x47}},
    {"ToneMapLut", 1392u, 2137381618u, {0xf0,0x8f,0xb8,0xf9,0x1b,0x65,0xe2,0xe8,0x1a,0xd6,0x7e,0x8c,0x96,0xd4,0x3c,0x58}},
}};
inline constexpr GUID tag {0x032194ea,0x12b6,0x461f,{0x89,0x30,0xa0,0x22,0x93,0xc7,0xbe,0x51}};
inline std::array<std::atomic<UINT>,shaders.size()> created {};
inline std::atomic<bool> installed {false};
inline ID3D11DeviceContext* context = nullptr; // Identity only; no idle COM ownership.
inline thread_local bool inside = false;
// Canonical loop lives in ext::core::Crc32 (ext/core/Crc32.h); kept as a named forwarder since
// this function's signature/name is part of this file's own shader-identification API.
inline UINT Crc(const void* input, size_t size) { return ext::core::Crc32(input, size); }
inline UINT Identify(const void* input, size_t size)
{
    if (!input || size < 32 || std::memcmp(input,"DXBC",4)) return 0;
    for (UINT i=0; i<shaders.size(); ++i)
        if (size == shaders[i].bytes && !std::memcmp(static_cast<const char*>(input)+4,shaders[i].checksum.data(),16)
            && Crc(input,size) == shaders[i].crc) return i+1;
    return 0;
}
using Create = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**);
using CreateLayout = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,ID3D11InputLayout**);
inline CreateLayout createLayout=nullptr;
inline HRESULT STDMETHODCALLTYPE OnCreateLayout(ID3D11Device* d,const D3D11_INPUT_ELEMENT_DESC* desc,UINT count,const void* code,SIZE_T size,ID3D11InputLayout** out)
{
    auto hr=createLayout(d,desc,count,code,size,out);
    if(SUCCEEDED(hr) && out && *out && companionCallbacks.tagLayout)
        companionCallbacks.tagLayout(*out,desc,count,code,size);
    return hr;
}
using Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using Indexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using Instanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
using IndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
using Indirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT);
inline Create create=nullptr; inline Draw draw=nullptr; inline Indexed indexed=nullptr;
inline Instanced instanced=nullptr; inline IndexedInstanced indexedInstanced=nullptr;
inline Indirect indirect=nullptr,indexedIndirect=nullptr;
using Copy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using CopyRegion=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
inline Copy copy=nullptr;inline CopyRegion copyRegion=nullptr;
inline void STDMETHODCALLTYPE OnCopy(ID3D11DeviceContext* c,ID3D11Resource* dest,ID3D11Resource* source)
{
    copy(c,dest,source);
    if(c==context && !inside){FfxivLightingScan::ColorCopy(dest,source,true);ext::hdr::FfxivSceneHdr::Copy(dest,source,true);}
}
inline void STDMETHODCALLTYPE OnCopyRegion(ID3D11DeviceContext* c,ID3D11Resource* dest,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* source,UINT from,const D3D11_BOX* box)
{
    copyRegion(c,dest,sub,x,y,z,source,from,box);
    if(c==context && !inside){const bool full=!sub && !from && !x && !y && !z && !box;FfxivLightingScan::ColorCopy(dest,source,full);ext::hdr::FfxivSceneHdr::Copy(dest,source,full);}
}
inline HRESULT STDMETHODCALLTYPE CreateShader(ID3D11Device* d,const void* code,SIZE_T size,ID3D11ClassLinkage* link,ID3D11PixelShader** out)
{
    auto hr=create(d,code,size,link,out);
    if (SUCCEEDED(hr) && out && *out && companionCallbacks.tagShader)
        companionCallbacks.tagShader(*out, code, size);
    if (SUCCEEDED(hr) && out && *out && companionCallbacks.tagPixel)
        companionCallbacks.tagPixel(*out, code, size);
    if (SUCCEEDED(hr) && out && *out)
        if (auto id=Identify(code,size))
        {
            if (SUCCEEDED((*out)->SetPrivateData(tag,sizeof(id),&id))) ++created[id-1];
        }
    if (SUCCEEDED(hr) && out && *out && code && size>=32 && !std::memcmp(code,"DXBC",4))
        ext::hdr::FfxivSceneHdr::Tag(*out,Crc(code,size));
    if (SUCCEEDED(hr) && out && *out) ext::ffxiv::ui::TagShader(*out,code,size); // ext: S10 A (UI shader registry; one atomic test while disarmed)
    return hr;
}
template<class F> inline void Around(ID3D11DeviceContext* c,const char* kind,F&& original)
{
    if(!inside && ext::ffxiv::ui::Armed()){inside=true;ext::ffxiv::ui::OnDraw(c,c==context);inside=false;} // ext: S10 A boundary detector (before every early-out; `inside` keeps its own copies away from OnCopy)
    if(c==context && !inside && companionCallbacks.observeDraw) companionCallbacks.observeDraw(c);
    const bool wanted=ext::Cfg().nr.whitePointSource.value_or_default()==2;
    if (!inside && c==context && ext::hdr::FfxivSceneHdr::Requested()) {
        struct HdrGuard { HdrGuard(){inside=true;} ~HdrGuard(){inside=false;} } guard;
        UINT id=0,bytes=sizeof(id);ComPtr<ID3D11PixelShader> ps;c->PSGetShader(&ps,nullptr,nullptr);if(ps)ps->GetPrivateData(tag,&bytes,&id);
        if(id==6 || !wanted)FfxivLightingScan::Tick(c,wanted);
        FfxivLightingScan::Before(c,id);
        UINT hdrStage=0,hdrBytes=sizeof(hdrStage);if(ps)ps->GetPrivateData(ext::hdr::FfxivSceneHdr::shaderTag,&hdrBytes,&hdrStage);
        if(hdrStage==6)ext::hdr::FfxivHdrPreview::BeforeDraw(c,original);
        original();FfxivLightingScan::After(c,id);
        ext::hdr::FfxivSceneHdr::AfterDraw(c,original);return;
    }
    if ((!wanted && !FfxivLightingScan::enabled.load()) || c!=context || inside)
    { original(); return; }
    struct Guard { Guard(){inside=true;} ~Guard(){inside=false;} } guard;
    UINT id = 0;
    ComPtr<ID3D11PixelShader> ps; c->PSGetShader(&ps, nullptr, nullptr); UINT bytes = sizeof(id);
    if (ps) ps->GetPrivateData(tag, &bytes, &id);
    // Drive sampling from the native adaptation pass, independent of any
    // DX12 presentation bridge or frame-generation provider.
    if(id==6 || !wanted)FfxivLightingScan::Tick(c,wanted);
    if(!FfxivLightingScan::armed.load()){original();return;}
    FfxivLightingScan::Before(c, id);
    original();
    FfxivLightingScan::After(c, id);
}
inline void STDMETHODCALLTYPE OnDraw(ID3D11DeviceContext* c,UINT n,UINT start) { Around(c,"Draw",[&]{draw(c,n,start);}); }
inline void STDMETHODCALLTYPE OnIndexed(ID3D11DeviceContext* c,UINT n,UINT start,INT b)
{
    if(c==context && !inside && companionCallbacks.interceptVisibilityIndexed &&
       companionCallbacks.interceptVisibilityIndexed(c,n,start,b,indexed))return;
    auto normal = [&]{ Around(c,"DrawIndexed",[&]{indexed(c,n,start,b);}); };
    if (c == context && !inside && companionCallbacks.aroundIndexed)
        companionCallbacks.aroundIndexed(c,n,start,[](void* state){ (*static_cast<decltype(normal)*>(state))(); },&normal);
    else normal();
}
inline void STDMETHODCALLTYPE OnInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,UINT f) { Around(c,"DrawInstanced",[&]{instanced(c,n,i,start,f);}); }
inline void STDMETHODCALLTYPE OnIndexedInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,INT b,UINT f)
{
    auto normal = [&]{ Around(c,"DrawIndexedInstanced",[&]{indexedInstanced(c,n,i,start,b,f);}); };
    if (c == context && !inside && i && companionCallbacks.aroundIndexed)
        companionCallbacks.aroundIndexed(c,n,start,[](void* state){ (*static_cast<decltype(normal)*>(state))(); },&normal);
    else normal();
}
inline void STDMETHODCALLTYPE OnIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { Around(c,"DrawInstancedIndirect",[&]{indirect(c,b,o);}); }
inline void STDMETHODCALLTYPE OnIndexedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { Around(c,"DrawIndexedInstancedIndirect",[&]{indexedIndirect(c,b,o);}); }
inline void Install(ID3D11Device* d)
{
    if (create || !d) return;
    ComPtr<ID3D11DeviceContext> c; d->GetImmediateContext(&c); if (!c) return;
    auto dv=*reinterpret_cast<void***>(d), cv=*reinterpret_cast<void***>(c.Get());
    create=reinterpret_cast<Create>(dv[15]); draw=reinterpret_cast<Draw>(cv[13]); indexed=reinterpret_cast<Indexed>(cv[12]);
    createLayout=reinterpret_cast<CreateLayout>(dv[11]);
    instanced=reinterpret_cast<Instanced>(cv[21]); indexedInstanced=reinterpret_cast<IndexedInstanced>(cv[20]);
    indirect=reinterpret_cast<Indirect>(cv[40]); indexedIndirect=reinterpret_cast<Indirect>(cv[39]);
    copy=reinterpret_cast<Copy>(cv[47]);copyRegion=reinterpret_cast<CopyRegion>(cv[46]);
    // Single-thread enlistment only, deliberately, even though Detach() below enlists every
    // process thread. Install happens once per device, early; Detours itself allocates (per
    // ext::core::DetourThreadScope's own comment, now verified against the real detours.cpp
    // source) while every enlisted thread sits suspended for the whole transaction, so enlisting
    // more threads here would spread a confirmed deadlock hazard to a path that fires on every
    // single session, not just the rare detach path. The crash risk multi-thread enlistment
    // exists to prevent (resuming a thread with its IP inside a patched prologue) is accepted
    // here, matching this codebase's existing convention at 120+ other single-thread Detours call
    // sites -- see the code-review ledger entry for the full trade-off.
    if (DetourTransactionBegin()!=NO_ERROR) { create=nullptr; return; }
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
#define LIGHT_ATTACH(o,h) ok &= DetourAttach(reinterpret_cast<PVOID*>(&o),h)==NO_ERROR
    LIGHT_ATTACH(create,CreateShader); LIGHT_ATTACH(draw,OnDraw); LIGHT_ATTACH(indexed,OnIndexed);
    LIGHT_ATTACH(createLayout,OnCreateLayout);
    LIGHT_ATTACH(instanced,OnInstanced); LIGHT_ATTACH(indexedInstanced,OnIndexedInstanced);
    LIGHT_ATTACH(indirect,OnIndirect); LIGHT_ATTACH(indexedIndirect,OnIndexedIndirect);
    LIGHT_ATTACH(copy,OnCopy);LIGHT_ATTACH(copyRegion,OnCopyRegion);
#undef LIGHT_ATTACH
    LONG result=ERROR_INVALID_FUNCTION;
    if (ok) result=DetourTransactionCommit(); else DetourTransactionAbort();
    if (result==NO_ERROR) { context=c.Get(); installed.store(true); LOG_INFO("FFXIV lighting: native exposure hooks ready"); }
    else { create=nullptr; createLayout=nullptr; LOG_ERROR("FFXIV lighting: hook installation failed {}",result); }
}
inline void Detach()
{
    ext::hdr::FfxivSceneHdr::EndFrame();
    FfxivLightingScan::Stop();
    // installed only flips to false once the detach transaction actually commits (below): if
    // create is null (never attached) or the transaction fails/aborts, installed must stay
    // exactly as it was, since SceneLightingProvider::Status() reads this flag as a diagnostics
    // surface and the hooks remain physically attached until a commit says otherwise.
    if (!create) return; // nothing was ever attached; installed is already false
    // Enlist every other process thread so Detours can relocate any instruction pointer that
    // happens to sit inside a patched prologue. On detach specifically, if another thread's
    // saved instruction pointer sits inside a patched prologue when the trampoline is reverted,
    // resuming that thread unrelocated can crash the game. See ext/core/DetourThreads.h.
    ext::core::DetourThreadScope threads;
    if (!threads.Ok())
    {
        LOG_WARN("FFXIV lighting: unable to enlist game threads; hooks left attached");
        return;
    }
    if (DetourTransactionBegin()!=NO_ERROR) return;
    bool ok=threads.Update()==NO_ERROR;
#define LIGHT_DETACH(o,h) ok &= DetourDetach(reinterpret_cast<PVOID*>(&o),h)==NO_ERROR
    LIGHT_DETACH(create,CreateShader); LIGHT_DETACH(draw,OnDraw); LIGHT_DETACH(indexed,OnIndexed);
    LIGHT_DETACH(createLayout,OnCreateLayout);
    LIGHT_DETACH(instanced,OnInstanced); LIGHT_DETACH(indexedInstanced,OnIndexedInstanced);
    LIGHT_DETACH(indirect,OnIndirect); LIGHT_DETACH(indexedIndirect,OnIndexedIndirect);
    LIGHT_DETACH(copy,OnCopy);LIGHT_DETACH(copyRegion,OnCopyRegion);
#undef LIGHT_DETACH
    if (ok) { if (DetourTransactionCommit()==NO_ERROR) { installed.store(false); create=nullptr; createLayout=nullptr; context=nullptr; } }
    else DetourTransactionAbort();
}
}
