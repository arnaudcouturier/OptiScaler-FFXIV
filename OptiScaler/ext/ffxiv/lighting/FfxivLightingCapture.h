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
#include "ext/ffxiv/chara/CharaDispatch.h" // ext: S11 character preview (CharaView)
#include "ext/ffxiv/lighting/FfxivLightingScan.h"
#include "ext/ffxiv/lighting/FfxivResolveGate.h"
#include "ext/ffxiv/catalog/FfxivCatalog.h" // ext: catalog health accounting
#include "ext/ffxiv/diag/DrawDiag.h" // ext: developer draw diagnostic (log only, one atomic load per draw)
#include "ext/ffxiv/relight/Inject.h" // ext: contact shadows before the sun light pass (one atomic load per draw while off)
#include "ext/ffxiv/ui/FfxivUiBoundary.h" // ext: S10 A hook consumer
#include "ext/ffxiv/ui/PlateDraws.h" // ext: developer probe of the UI draws (log only)
#include "ext/ffxiv/ui/PlateLayoutTag.h" // ext: input-layout tags for the nameplate vertex shaders (catalog group and the plate classifier)
#include "ext/ffxiv/plates/PlateNative.h" // ext: native nameplate draw classifier and redirect
#include "ext/ffxiv/plates/PlateShaderKind.h" // ext: native nameplate pixel-shader stamp
#include "ext/hdr/FfxivHdrPreview.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"

// Native exposure scan hooks. Signatures identify the game's
// shaders, extracted read-only from SqPack on 2026-09-23. No engine RVA is hooked.
// Verified tone response controls history rejection, never an exposure multiply.
namespace ext::ffxiv::lighting::FfxivLightingCapture
{
using Microsoft::WRL::ComPtr;
// Input-layout tags for the nameplate vertex shaders live with the catalog consumers
// (ext/ffxiv/ui/PlateLayoutTag.h) and are written directly, with no callback.

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
    if(SUCCEEDED(hr) && out && *out)
        ext::ffxiv::ui::plate::TagLayout(*out,desc,count,code,size);
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
// ext: contact shadows tracks the bound pixel shader here, so the draw hook never asks the context for it (a PSGetShader per draw was
// 0.65 ms a frame at the game's ~9,250 draws, 32x the design's 0.02 ms budget). The seam sits behind the game's own PSSetShader call, which
// it makes about 0.7 times per draw: with the key off the seam adds one out-of-line call and one relaxed load per bind, measured at 1.0 ns
// on this machine, i.e. 0.007 ms a frame at 6,300 binds; with it on, one pointer compare and (on a hit) one stamp
// check per bind, and one integer load per draw.
using SetPs=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11PixelShader*,ID3D11ClassInstance* const*,UINT);
inline SetPs setPs=nullptr;
inline void STDMETHODCALLTYPE OnSetPs(ID3D11DeviceContext* c,ID3D11PixelShader* ps,ID3D11ClassInstance* const* instances,UINT count)
{
    setPs(c,ps,instances,count);
    if(c==context) ext::ffxiv::relight::OnPixelShaderSet(ps,count); // ext: contact shadows (one atomic load inside while the key is off)
}
using Copy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using CopyRegion=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
inline Copy copy=nullptr;inline CopyRegion copyRegion=nullptr;
inline void STDMETHODCALLTYPE OnCopy(ID3D11DeviceContext* c,ID3D11Resource* dest,ID3D11Resource* source)
{
    copy(c,dest,source);
    if(c==context && !inside){FfxivLightingScan::ColorCopy(dest,source,true);ext::hdr::FfxivSceneHdr::Copy(dest,source,true);}
    ext::ffxiv::chara::OnCopy(c,dest,source,true); // ext: S11 (after the native copy; not under `inside`, the feature has its own flags)
}
inline void STDMETHODCALLTYPE OnCopyRegion(ID3D11DeviceContext* c,ID3D11Resource* dest,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource* source,UINT from,const D3D11_BOX* box)
{
    copyRegion(c,dest,sub,x,y,z,source,from,box);
    const bool full=!sub && !from && !x && !y && !z && !box;
    if(c==context && !inside){FfxivLightingScan::ColorCopy(dest,source,full);ext::hdr::FfxivSceneHdr::Copy(dest,source,full);}
    ext::ffxiv::chara::OnCopyRegion(c,dest,sub,x,y,z,source,from,box); // ext: S11 (the preview layer works out `full` itself)
}
// ext: S11 ResolveSubresource (vtable index 57) is only observed by the preview probe.
using ResolveCall=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,ID3D11Resource*,UINT,DXGI_FORMAT);
inline ResolveCall resolve=nullptr;
inline void STDMETHODCALLTYPE OnResolve(ID3D11DeviceContext* c,ID3D11Resource* dest,UINT destSub,ID3D11Resource* source,UINT sourceSub,DXGI_FORMAT format)
{
    resolve(c,dest,destSub,source,sourceSub,format);
    ext::ffxiv::chara::OnResolve(c,dest,destSub,source,sourceSub,format);
}
// The detour is NOT part of the main hook transaction (Install): a debug aid must not be able to take the shipped features down. The preview
// probe asks for it through ext::ffxiv::chara::probe::ResolveHookControl when the probe or the flood test is first switched on, from the
// draw thread, and again to remove it. Every transaction of this layer (Install, the main Detach, these two) runs under one gate
// (FfxivResolveGate.h): Detours rejects a second transaction that starts while one is open, and the unhook thread's main detach can meet the
// draw thread's run-time removal after a deferred preview teardown. A failure leaves every other hook as it was, and a failed attach is
// reported by the probe.
// These two transactions run while the game's other threads are live, and the code of ResolveSubresource is shared with deferred contexts
// that they may be executing while the patch is written: so they enlist every game thread (DetourThreadScope), like the main Detach does.
// The risk the Install comment below describes (Detours allocates while enlisted threads are suspended) is accepted here: this only runs
// when a debug aid (the probe or the flood test) is switched on or off, never in a normal session.
inline TransactionGate transactionGate;
inline bool AttachResolve()
{
    return transactionGate.AttachResolve(
        [] { return installed.load() && context != nullptr; }, // the main hooks are installed (evaluated under the gate)
        []
        {
            resolve=reinterpret_cast<ResolveCall>((*reinterpret_cast<void***>(context))[57]);
            ext::core::DetourThreadScope threads;
            if (!threads.Ok()) { LOG_WARN("FFXIV lighting: unable to enlist game threads; the ResolveSubresource detour was not attached"); return false; }
            if (DetourTransactionBegin()!=NO_ERROR) return false;
            bool ok=threads.Update()==NO_ERROR;
            ok &= DetourAttach(reinterpret_cast<PVOID*>(&resolve),OnResolve)==NO_ERROR;
            LONG result=ERROR_INVALID_FUNCTION;
            if (ok) result=DetourTransactionCommit(); else DetourTransactionAbort();
            return result==NO_ERROR;
        });
}
inline void DetachResolve()
{
    transactionGate.DetachResolve(
        []
        {
            ext::core::DetourThreadScope threads;
            if (!threads.Ok()) { LOG_WARN("FFXIV lighting: unable to enlist game threads; the ResolveSubresource detour stays attached"); return false; }
            if (DetourTransactionBegin()!=NO_ERROR) return false;
            bool ok=threads.Update()==NO_ERROR;
            ok &= DetourDetach(reinterpret_cast<PVOID*>(&resolve),OnResolve)==NO_ERROR;
            if (ok) return DetourTransactionCommit()==NO_ERROR; // `resolve` is the original function again
            DetourTransactionAbort();
            return false;
        });
}
inline const ext::ffxiv::chara::probe::ResolveHookControl resolveControl { &AttachResolve, &DetachResolve };
inline HRESULT STDMETHODCALLTYPE CreateShader(ID3D11Device* d,const void* code,SIZE_T size,ID3D11ClassLinkage* link,ID3D11PixelShader** out)
{
    auto hr=create(d,code,size,link,out);
    if (SUCCEEDED(hr) && out && *out)
        if (auto id=Identify(code,size))
        {
            if (SUCCEEDED((*out)->SetPrivateData(tag,sizeof(id),&id))) ++created[id-1];
        }
    if (SUCCEEDED(hr) && out && *out && code && size>=32 && size<=0xFFFFFFFFull && !std::memcmp(code,"DXBC",4))
    {
        const UINT crc=Crc(code,size); // hashed once: the scene-stage tag and the UI boundary stamp share it
        ext::ffxiv::catalog::OnPixelShaderCreated(code,size,crc); // ext: catalog health (counts every catalog CRC here, ALWAYS: armed or not; lock free, no allocation, no log)
        ext::hdr::FfxivSceneHdr::Tag(*out,crc);
        ext::ffxiv::chara::OnShaderCreated(*out,crc,static_cast<uint32_t>(size)); // ext: S11 character preview roles (after SceneHdr::Tag wrote the UI tag)
        ext::ffxiv::ui::OnShaderCreated(*out,crc,static_cast<uint32_t>(size)); // ext: S10 A/E3 (stamps the boundary stages ALWAYS: a Mode switched on later needs them; D1 registry only while armed)
        ext::ffxiv::plates::OnPixelShaderCreated(*out,crc); // ext: native nameplates (stamps the four nameplate variants ALWAYS: a key switched on later needs them)
        ext::ffxiv::diag::OnPixelShaderCreated(*out,code,size,crc); // ext: developer draw diagnostic (one atomic test; reflects the bytecode once per shader)
        ext::ffxiv::relight::OnShaderCreated(*out,code,size,crc); // ext: contact shadows (one atomic test; the reflection of a sun light draw is parsed once)
    }
    return hr;
}
template<class F> inline void AroundBody(ID3D11DeviceContext* c,const char* kind,F&& original)
{
    if(ext::ffxiv::ui::Armed() && !inside){inside=true;ext::ffxiv::ui::OnDraw(c,c==context);inside=false;} // ext: S10 A boundary detector (before every early-out; `inside` keeps its own copies away from OnCopy)
    if(!inside && ext::ffxiv::diag::Capturing()) ext::ffxiv::diag::OnDraw(c,kind); // ext: developer draw diagnostic (ONE relaxed atomic load while the key is off)
    const bool wanted=ext::Cfg().nr.whitePointSource.value_or_default()==2;
    if (!inside && c==context && ext::hdr::FfxivSceneHdr::Requested()) {
        struct HdrGuard { HdrGuard(){inside=true;} ~HdrGuard(){inside=false;} } guard;
        UINT id=0,bytes=sizeof(id);ComPtr<ID3D11PixelShader> ps;c->PSGetShader(&ps,nullptr,nullptr);if(ps)ps->GetPrivateData(tag,&bytes,&id);
        if(id==6 || !wanted)FfxivLightingScan::Tick(c,wanted);
        FfxivLightingScan::Before(c,id);
        UINT hdrStage=0,hdrBytes=sizeof(hdrStage);if(ps)ps->GetPrivateData(ext::hdr::FfxivSceneHdr::shaderTag,&hdrBytes,&hdrStage);
        // ext: S11 the HDR replays run the draw lambda too; the character preview layer must not see them as draws
        auto replay=[&]{ext::ffxiv::chara::ReplayScope scope;original();};
        if(hdrStage==6)ext::hdr::FfxivHdrPreview::BeforeDraw(c,replay);
        original();FfxivLightingScan::After(c,id);
        ext::hdr::FfxivSceneHdr::AfterDraw(c,replay);return;
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
// The seam of the contact shadow pass: it wraps only the game's draw (it hands the hook body a draw call that runs the pass first), and
// only on the game's own immediate context - a draw on any other context is passed through, since the classification reads the immediate
// context's tracked shader and the pass enters its private state on that context. The feature bit is tested first: one relaxed load while
// the key is off, and nothing else.
template<class F> inline void Around(ID3D11DeviceContext* c,const char* kind,F&& original)
{
    if(!inside && c==context && ext::Active(ext::F::ContactShadows)) // ext: contact shadows (ONE relaxed load while off)
    {
        bool first=true;
        auto wrapped=[&]{
            // The first call of the hook body runs the pass with the game's draw; every later call is a replay of the same draw (the scene
            // HDR path repeats it to re-run the tone chain, the lighting scan runs it twice) and runs the draw alone: the pass belongs to the
            // draw the frame makes, and running it again would double its cost and write the mask copy a second time.
            ext::ffxiv::relight::RunDrawOnce(first,ext::hdr::FfxivSceneHdr::Requested(),[&]{ext::ffxiv::relight::BoundDraw(c,original);},original);
        };
        AroundBody(c,kind,wrapped);
        return;
    }
    AroundBody(c,kind,original);
}
inline void STDMETHODCALLTYPE OnDraw(ID3D11DeviceContext* c,UINT n,UINT start) { ext::ffxiv::ui::plate::NoteDraw('D',n,1,start,0); Around(c,"Draw",[&]{ext::ffxiv::chara::Around(c,ext::ffxiv::chara::DrawApi::Draw,[&]{draw(c,n,start);});}); }
inline void STDMETHODCALLTYPE OnIndexed(ID3D11DeviceContext* c,UINT n,UINT start,INT b)
{
    ext::ffxiv::ui::plate::NoteDraw('I',n,1,start,b); // ext: developer probe (one atomic test while PlateDiagnostics is off)
    // ext: native nameplates. A plate draw of a frame the plate engine hides is drawn into the plate image and not drawn
    // here; every other draw, and every draw while the key is off, is untouched (one atomic load, then 0). `original` is
    // the real DrawIndexed, so the redirected draw never re-enters these hooks.
    if (c == context && !inside && ext::ffxiv::plates::InterceptIndexed(c, n, start, b, reinterpret_cast<void*>(indexed)))
        return;
    auto normal = [&]{ Around(c,"DrawIndexed",[&]{ext::ffxiv::chara::Around(c,ext::ffxiv::chara::DrawApi::DrawIndexed,[&]{indexed(c,n,start,b);});}); };
    normal();
}
inline void STDMETHODCALLTYPE OnInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,UINT f) { ext::ffxiv::ui::plate::NoteDraw('N',n,i,start,0); Around(c,"DrawInstanced",[&]{ext::ffxiv::chara::Around(c,ext::ffxiv::chara::DrawApi::DrawInstanced,[&]{instanced(c,n,i,start,f);});}); }
inline void STDMETHODCALLTYPE OnIndexedInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT start,INT b,UINT f)
{
    ext::ffxiv::ui::plate::NoteDraw('X',n,i,start,b);
    auto normal = [&]{ Around(c,"DrawIndexedInstanced",[&]{ext::ffxiv::chara::Around(c,ext::ffxiv::chara::DrawApi::DrawIndexedInstanced,[&]{indexedInstanced(c,n,i,start,b,f);});}); };
    normal();
}
inline void STDMETHODCALLTYPE OnIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { ext::ffxiv::ui::plate::NoteDraw('Y',0,0,0,0); Around(c,"DrawInstancedIndirect",[&]{ext::ffxiv::chara::Around(c,ext::ffxiv::chara::DrawApi::DrawInstancedIndirect,[&]{indirect(c,b,o);});}); }
inline void STDMETHODCALLTYPE OnIndexedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT o) { ext::ffxiv::ui::plate::NoteDraw('Y',0,0,0,0); Around(c,"DrawIndexedInstancedIndirect",[&]{ext::ffxiv::chara::Around(c,ext::ffxiv::chara::DrawApi::DrawIndexedInstancedIndirect,[&]{indexedIndirect(c,b,o);});}); }
// ext: S11 hooks the character preview layer needs besides the draws: ClearRenderTargetView, Begin, End, CreateVertexShader.
using ClearRtv=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT[4]);
using QueryCall=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Asynchronous*);
using CreateVs=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11VertexShader**);
inline ClearRtv clearRtv=nullptr; inline QueryCall beginQuery=nullptr,endQuery=nullptr; inline CreateVs createVs=nullptr;
inline void STDMETHODCALLTYPE OnClearRtv(ID3D11DeviceContext* c,ID3D11RenderTargetView* v,const FLOAT color[4])
{ clearRtv(c,v,color); ext::ffxiv::chara::OnClearRenderTargetView(c,v,color); }
inline void STDMETHODCALLTYPE OnBegin(ID3D11DeviceContext* c,ID3D11Asynchronous* q)
{ ext::ffxiv::chara::OnQueryBegin(c,q); beginQuery(c,q); }
inline void STDMETHODCALLTYPE OnEnd(ID3D11DeviceContext* c,ID3D11Asynchronous* q)
{ endQuery(c,q); ext::ffxiv::chara::OnQueryEnd(c,q); }
inline HRESULT STDMETHODCALLTYPE OnCreateVertexShader(ID3D11Device* d,const void* code,SIZE_T size,ID3D11ClassLinkage* link,ID3D11VertexShader** out)
{
    auto hr=createVs(d,code,size,link,out);
    if(SUCCEEDED(hr) && out && *out) ext::ffxiv::chara::OnVertexShaderCreated(d,*out,code,size);
    if(SUCCEEDED(hr) && out && *out) ext::ffxiv::ui::plate::OnVertexShaderCreated(*out,code,size); // ext: developer probe (names the vertex shader of a draw)
    return hr;
}
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
    clearRtv=reinterpret_cast<ClearRtv>(cv[50]);beginQuery=reinterpret_cast<QueryCall>(cv[27]);endQuery=reinterpret_cast<QueryCall>(cv[28]); // ext: S11
    setPs=reinterpret_cast<SetPs>(cv[9]); // ext: contact shadows' pixel-shader tracker
    createVs=reinterpret_cast<CreateVs>(dv[12]);
    // Single-thread enlistment only, deliberately, even though Detach() below enlists every
    // process thread. Install happens once per device, early; Detours itself allocates (per
    // ext::core::DetourThreadScope's own comment, now verified against the real detours.cpp
    // source) while every enlisted thread sits suspended for the whole transaction, so enlisting
    // more threads here would spread a confirmed deadlock hazard to a path that fires on every
    // single session, not just the rare detach path. The crash risk multi-thread enlistment
    // exists to prevent (resuming a thread with its IP inside a patched prologue) is accepted
    // here, matching this codebase's existing convention at 120+ other single-thread Detours call
    // sites -- see the code-review ledger entry for the full trade-off.
    // The transaction runs under the layer's gate (see AttachResolve): the unhook thread's main detach or the draw thread's run-time resolve
    // transaction must not be open at the same time.
    bool began=false;
    LONG result=ERROR_INVALID_FUNCTION;
    transactionGate.Run([&]
    {
        if (DetourTransactionBegin()!=NO_ERROR) return 0;
        began=true;
        bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
#define LIGHT_ATTACH(o,h) ok &= DetourAttach(reinterpret_cast<PVOID*>(&o),h)==NO_ERROR
        LIGHT_ATTACH(create,CreateShader); LIGHT_ATTACH(draw,OnDraw); LIGHT_ATTACH(indexed,OnIndexed); LIGHT_ATTACH(setPs,OnSetPs);
        LIGHT_ATTACH(createLayout,OnCreateLayout);
        LIGHT_ATTACH(instanced,OnInstanced); LIGHT_ATTACH(indexedInstanced,OnIndexedInstanced);
        LIGHT_ATTACH(indirect,OnIndirect); LIGHT_ATTACH(indexedIndirect,OnIndexedIndirect);
        LIGHT_ATTACH(copy,OnCopy);LIGHT_ATTACH(copyRegion,OnCopyRegion);
        LIGHT_ATTACH(clearRtv,OnClearRtv);LIGHT_ATTACH(beginQuery,OnBegin);LIGHT_ATTACH(endQuery,OnEnd);LIGHT_ATTACH(createVs,OnCreateVertexShader); // ext: S11
#undef LIGHT_ATTACH
        ext::ffxiv::catalog::OnHooksInstalled(); // ext: time zero of the catalog health log (before the creation detour can run)
        if (ok) result=DetourTransactionCommit(); else DetourTransactionAbort();
        return 0;
    });
    if (!began) { create=nullptr; return; }
    if (result==NO_ERROR) { context=c.Get(); installed.store(true); ext::ffxiv::chara::probe::SetResolveHookControl(&resolveControl); ext::ffxiv::chara::OnInstalled(c.Get(),reinterpret_cast<void*>(draw)); LOG_INFO("FFXIV lighting: native exposure hooks ready"); }
    else { create=nullptr; createLayout=nullptr; LOG_ERROR("FFXIV lighting: hook installation failed {}",result); }
}
// The main detach transaction; runs under the layer's gate.
inline void DetachTransaction()
{
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
    LIGHT_DETACH(create,CreateShader); LIGHT_DETACH(draw,OnDraw); LIGHT_DETACH(indexed,OnIndexed); LIGHT_DETACH(setPs,OnSetPs);
    LIGHT_DETACH(createLayout,OnCreateLayout);
    LIGHT_DETACH(instanced,OnInstanced); LIGHT_DETACH(indexedInstanced,OnIndexedInstanced);
    LIGHT_DETACH(indirect,OnIndirect); LIGHT_DETACH(indexedIndirect,OnIndexedIndirect);
    LIGHT_DETACH(copy,OnCopy);LIGHT_DETACH(copyRegion,OnCopyRegion);
    if (transactionGate.ResolveAttached()) LIGHT_DETACH(resolve,OnResolve); // ext: S11 (only when the probe attached it, see AttachResolve)
    LIGHT_DETACH(clearRtv,OnClearRtv);LIGHT_DETACH(beginQuery,OnBegin);LIGHT_DETACH(endQuery,OnEnd);LIGHT_DETACH(createVs,OnCreateVertexShader); // ext: S11
#undef LIGHT_DETACH
    if (ok) { if (DetourTransactionCommit()==NO_ERROR) { installed.store(false); create=nullptr; createLayout=nullptr; context=nullptr; transactionGate.ResolveGone(); ext::ffxiv::chara::probe::SetResolveHookControl(nullptr); ext::ffxiv::chara::OnDetached(); } }
    else DetourTransactionAbort();
}
inline void Detach()
{
    ext::hdr::FfxivSceneHdr::EndFrame();
    ext::ffxiv::chara::Detach(); // ext: S11 releases the preview layer's GPU objects while the device is still valid
    FfxivLightingScan::Stop();
    // The gate is taken only now: on the draw thread chara::Detach() above reaches the probe's run-time removal of the resolve detour,
    // which takes the same gate.
    transactionGate.Run([] { DetachTransaction(); return 0; });
}
}
