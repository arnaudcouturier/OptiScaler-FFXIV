#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/game/Game.h"
#include "ext/gpu/GpuLifetime.h"
#include "ext/hdr/Hdr10.h"
#include "ext/hdr/HdrConstants.h"
#include "ext/hdr/screenshot/HdrScreenshot.h"

#include <Config.h>
#include <State.h>
#include <Hdr10_Shader.h>

#include <atomic>
#include <mutex>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Logic, defaults and log messages are those of the reference build's shaders/hdr/Hdr10.cpp (the dense style is kept so
// the two stay diffable). Changes: namespace, config through ext::Cfg().hdr, the FFXIV exe test is a game-module test,
// "active" is the F::Hdr feature bit and nightly's hdrOutputActive/outputColorSpace are filled (SEAMS 5), the pool is an
// ext::gpu::Owner instead of hard-wired calls in the ExecuteCommandLists/Reset hooks.
namespace ext::hdr {
namespace {
std::atomic<bool> filledState{false};
std::mutex statusMutex;
std::string status="Off (restart required to enable)";
void Message(std::string text){std::lock_guard lock(statusMutex);status=std::move(text);}
struct Packet {
 ComPtr<ID3D12Device> device;
 ComPtr<ID3D12Resource> image, source;
 SceneInput scene;
 ComPtr<ID3D12DescriptorHeap> heap;
 ComPtr<ID3D12RootSignature> root;
 ComPtr<ID3D12PipelineState> pso;
 std::function<bool()> done;
 UINT width=0,height=0;ULONGLONG lastUse=0;bool recorded=false;
};
struct Pool {
 std::mutex mutex; gpu::GpuLifetime lifetime; std::vector<std::unique_ptr<Packet>> packets;
 int loggedReShadeHighlights=-1, loggedContinuous=-1;
 float loggedUiExpansion=-2;
 float loggedPaper=-1, loggedPeak=-1, loggedExpansion=-1, loggedContrast=-1, loggedSaturation=-1, loggedVibrance=-2;
};
Pool& P(){static auto* p=new Pool;return *p;}
void CollectLocked(Pool& pool,bool keepWarm=true){
 const auto now=GetTickCount64();
 for(auto& p:pool.packets)if(ReusableRecordedWork(p->recorded,p->done)){p->source.Reset();p->scene={};}
 std::erase_if(pool.packets,[&](const auto& p){
     if(!ReusableRecordedWork(p->recorded,p->done))return false;
     return !keepWarm || now-p->lastUse>=2000;
 });
}
 // Never destroy in-flight GPU ownership under loader lock.
void Barrier(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&x);
}
bool Init(Packet& p,ID3D12Device*d,UINT w,UINT h){
 p.device=d;p.width=w;p.height=h;
 D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R10G10B10A2_UNORM};
 if(FAILED(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) || !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;
 D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
 if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&p.heap))))return false;
 D3D12_DESCRIPTOR_RANGE ranges[2]{};
 ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,4};
 D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
 params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,20};
 D3D12_ROOT_SIGNATURE_DESC rd{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
 if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return false;
 if(FAILED(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&p.root))))return false;
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=p.root.Get();pd.CS={Hdr10_cso,sizeof(Hdr10_cso)};
 if(FAILED(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&p.pso))))return false;
 D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=w;td.Height=h;td.DepthOrArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R10G10B10A2_UNORM;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
 D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
 return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&p.image)));
}

// nightly's hdrOutputActive / outputColorSpace are the HDR truth (SEAMS 5). OptiHDR output negotiates the same space the
// wrapped swapchain's UpdateOutputColorSpace would have recorded for it.
void FillOutputState(bool on){
 auto& state=State::Instance();
 if(on){
  OutputColorSpace space{};space.dxgiColorSpace=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;space.transfer=ColorTransfer::PQ;
  space.primaries=ColorPrimaries::Rec2020;space.range=ColorRange::Full;space.model=ColorModel::RGB;space.valid=true;
  state.outputColorSpace=space;state.hdrOutputActive=true;filledState=true;
 } else if(filledState.exchange(false)){state.outputColorSpace=OutputColorSpace{};state.hdrOutputActive=false;}
}
// The pool is one GPU submission owner (S4 mapping of Hdr10::BeginSubmission / Hdr10::Reset). Registered once, the first
// time OptiHDR negotiates its output; it stays enabled for the process, so lists recorded while HDR was active keep
// their lifetime tracking.
void EnsureOwner(ID3D12Device* device){
 static std::once_flag once;static int handle=-1;
 std::call_once(once,[]{
  gpu::Owner owner;
  owner.begin=[](void*,UINT n,ID3D12CommandList*const* l){return BeginSubmission(n,l);};
  owner.quarantine=[](void*,UINT n,ID3D12CommandList*const* l){QuarantineSubmission(n,l);};
  owner.reset=[](void*,ID3D12CommandList* c){Reset(c);};
  owner.after=screenshot::AfterExecute;
  handle=gpu::RegisterOwner(owner);
 });
 if(handle>=0 && device)restrack::EnsureHooks(device);
}
}
bool OutputIsHdr(){return State::Instance().hdrOutputActive;}
bool Request(HWND hwnd){
 if(!ext::Cfg().hdr.ffxivHdr.value_or_default() || ext::game::Current().hdr==nullptr)return false;
 const auto fg=Config::Instance()->FGOutput.value_or_default();
 if(fg!=FGOutput::NoFG && fg!=FGOutput::DLSSG && fg!=FGOutput::XeFG){Message("OptiHDR requires DLSS-G, XeFG, or FG off");return false;}
 ComPtr<IDXGIFactory1> factory; if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
 auto monitor=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
 for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)!=S_OK)break;
  for(UINT j=0;;++j){ComPtr<IDXGIOutput>o;if(a->EnumOutputs(j,&o)!=S_OK)break;ComPtr<IDXGIOutput6>o6;
   if(SUCCEEDED(o.As(&o6))){DXGI_OUTPUT_DESC1 desc{};if(SUCCEEDED(o6->GetDesc1(&desc))&&desc.Monitor==monitor&&desc.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)return true;}
  }
 }
 Message("SDR fallback: enable Windows HDR on the game display before launch");return false;
}
void Deactivate(){ext::SetActive(ext::F::Hdr,false);FillOutputState(false);auto& pool=P();std::lock_guard lock(pool.mutex);CollectLocked(pool,false);}
std::string Status(){std::lock_guard lock(statusMutex);return status;}
bool Configure(IDXGISwapChain4*c){
 DXGI_SWAP_CHAIN_DESC1 d{}; if(!c||FAILED(c->GetDesc1(&d))||d.Format!=DXGI_FORMAT_R10G10B10A2_UNORM)return false;
 UINT flags=0;const auto cs=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
 if(FAILED(c->CheckColorSpaceSupport(cs,&flags))||!(flags&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)||FAILED(c->SetColorSpace1(cs))){Message("HDR10 output negotiation failed");return false;}
 ComPtr<ID3D12Device> device;c->GetDevice(IID_PPV_ARGS(&device));EnsureOwner(device.Get());
 FillOutputState(true);ext::SetActive(ext::F::Hdr,true);Message("HDR10 active: SDR expansion; game HUD shares the same curve");
 LOG_INFO("FFXIV HDR10 output active (PQ / BT.2020); SDR upscaler and NR inputs preserved");return true;
}
ID3D12Resource* Convert(ID3D12Device*d,ID3D12GraphicsCommandList*c,ID3D12Resource*src,D3D12_RESOURCE_STATES state,SceneInput scene){
 if(!src||!c||!d)return nullptr;auto sd=src->GetDesc();
 DXGI_FORMAT format=sd.Format;
 switch(format){case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:format=DXGI_FORMAT_R8G8B8A8_UNORM;break;
 case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:format=DXGI_FORMAT_B8G8R8A8_UNORM;break;default:break;}
 if(sd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||sd.SampleDesc.Count!=1||sd.DepthOrArraySize!=1||(format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_B8G8R8A8_UNORM&&format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&format!=DXGI_FORMAT_R10G10B10A2_UNORM))return nullptr;
 auto& pool=P();std::lock_guard lock(pool.mutex);pool.lifetime.Collect();CollectLocked(pool);Packet* packet=nullptr;
 for(auto& p:pool.packets)if(ReusableRecordedWork(p->recorded,p->done)){packet=p.get();break;}
 if(!packet){if(pool.packets.size()>=32){LOG_ERROR("HDR10 conversion pool busy; no in-flight packet was reused");return nullptr;}pool.packets.push_back(std::make_unique<Packet>());packet=pool.packets.back().get();}
 auto&p=*packet;
 if(p.device.Get()!=d||p.width!=sd.Width||p.height!=sd.Height||!p.image){p=Packet{};if(!Init(p,d,(UINT)sd.Width,sd.Height)){Message("HDR10 conversion allocation failed");return nullptr;}}
 p.lastUse=GetTickCount64();p.source=src;p.scene=std::move(scene);
 auto cpu=p.heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d->CreateShaderResourceView(src,&srv,cpu);
 const auto stride=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
 for(auto* resource:{p.scene.hdr.Get(),p.scene.reference.Get(),p.scene.previewMask.Get()}){
  cpu.ptr+=stride;srv.Format=resource?resource->GetDesc().Format:DXGI_FORMAT_R16G16B16A16_FLOAT;
  d->CreateShaderResourceView(resource,&srv,cpu);
 }
 cpu.ptr+=stride;D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R10G10B10A2_UNORM;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d->CreateUnorderedAccessView(p.image.Get(),nullptr,&uav,cpu);
 auto&cfg=ext::Cfg().hdr;HdrConstantSource cs;
 cs.paper=cfg.paper.value_or_default();cs.peak=cfg.peak.value_or_default();cs.expansion=cfg.expansion.value_or_default();
 cs.contrast=cfg.contrast.value_or_default();cs.saturation=cfg.saturation.value_or_default();cs.vibrance=cfg.vibrance.value_or_default();
 cs.legacyComposition=cfg.legacyComposition.value_or_default();
 {const std::optional<float> ui=cfg.uiExpansion; // one snapshot: the menu thread's Reset can clear it between two reads
  const auto resolved=ResolveUiExpansion(ui,cs.expansion);cs.uiExpansionSet=resolved.first;cs.uiExpansion=resolved.second;}
 cs.sceneRequested=cfg.mode.value_or_default()==1;
 cs.sceneAvailable=bool(p.scene.hdr)&&bool(p.scene.reference);
 cs.reshadeHighlights=cfg.reshadeHighlights.value_or_default();
 cs.referenceSrgb=p.scene.referenceSrgb;cs.hasPreviewMask=bool(p.scene.previewMask);
 std::copy(p.scene.rect.begin(),p.scene.rect.end(),cs.rect);
 cs.width=p.width;cs.height=p.height;
 HdrConstants k=BuildHdrConstants(cs);
 if(cs.sceneRequested){Message(k.scene?"Scene HDR active: RenoDX FP16 scene + HDR-mapped UI; 10-bit PQ output":"Scene HDR waiting for verified scene: paper-white SDR fallback");}
 else Message("HDR10 active: SDR expansion; game HUD shares the same curve");
 if(pool.loggedPaper!=k.paper || pool.loggedPeak!=k.peak || pool.loggedExpansion!=k.expansion || pool.loggedContrast!=k.contrast || pool.loggedSaturation!=k.saturation || pool.loggedVibrance!=k.vibrance || pool.loggedReShadeHighlights!=int(k.reshadeHighlights)){
  LOG_INFO("HDR10 conversion: paper white {} nits, peak {} nits, expansion {}, contrast {}, saturation {}, vibrance {}, SDR white maps to {} nits, ReShade highlight preservation {}",
           k.paper,k.peak,k.expansion,k.contrast,k.saturation,k.vibrance,k.paper+(k.peak-k.paper)*k.expansion,k.reshadeHighlights!=0);
  pool.loggedPaper=k.paper;pool.loggedPeak=k.peak;pool.loggedExpansion=k.expansion;pool.loggedContrast=k.contrast;pool.loggedSaturation=k.saturation;pool.loggedVibrance=k.vibrance;pool.loggedReShadeHighlights=int(k.reshadeHighlights);
 }
 if(pool.loggedContinuous!=int(k.continuous) || pool.loggedUiExpansion!=k.uiExpansion){
  LOG_INFO("HDR10 composition: {}, UI expansion {}",k.continuous?"continuous":"legacy two-rule",
           cs.uiExpansionSet?std::to_string(k.uiExpansion):"follows HighlightExpansion ("+std::to_string(k.uiExpansion)+")");
  pool.loggedContinuous=int(k.continuous);pool.loggedUiExpansion=k.uiExpansion;
 }
 pool.lifetime.Record(c);p.recorded=true;p.done=pool.lifetime.ReuseProbe(c);if(p.scene.retire)p.scene.retire(p.done);
 if(k.scene){Barrier(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
 if(NeedsPreviewMaskBarrier(k))Barrier(c,p.scene.previewMask.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Barrier(c,src,state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,p.image.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 ID3D12DescriptorHeap* heaps[]={p.heap.Get()};c->SetDescriptorHeaps(1,heaps);c->SetComputeRootSignature(p.root.Get());c->SetPipelineState(p.pso.Get());c->SetComputeRootDescriptorTable(0,p.heap->GetGPUDescriptorHandleForHeapStart());c->SetComputeRoot32BitConstants(1,20,&k,0);c->Dispatch((p.width+7)/8,(p.height+7)/8,1);
 Barrier(c,p.image.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Barrier(c,src,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,state);
 if(k.scene){Barrier(c,p.scene.hdr.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);Barrier(c,p.scene.reference.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);}
 if(NeedsPreviewMaskBarrier(k))Barrier(c,p.scene.previewMask.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
 return p.image.Get();
}
void NoteGameColorSpace(DXGI_COLOR_SPACE_TYPE cs){
 OutputColorSpace space{};space.dxgiColorSpace=cs;space.range=ColorRange::Full;space.model=ColorModel::RGB;
 switch(cs){
 case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:space.transfer=ColorTransfer::SRGB;space.primaries=ColorPrimaries::Rec709;space.valid=true;break;
 case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:space.transfer=ColorTransfer::Linear;space.primaries=ColorPrimaries::Rec709;space.valid=true;break;
 case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:space.transfer=ColorTransfer::PQ;space.primaries=ColorPrimaries::Rec2020;space.valid=true;break;
 case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:space.transfer=ColorTransfer::SRGB;space.primaries=ColorPrimaries::Rec2020;space.valid=true;break;
 case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:space.transfer=ColorTransfer::HLG;space.primaries=ColorPrimaries::Rec2020;space.model=ColorModel::YCbCr;space.valid=true;break;
 default:break;
 }
 auto& state=State::Instance();state.outputColorSpace=space;
 state.hdrOutputActive=cs==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}
std::function<bool()> TrackCommands(ID3D12GraphicsCommandList*c){auto& p=P();std::lock_guard lock(p.mutex);p.lifetime.Record(c);return p.lifetime.ReuseProbe(c);}
gpu::GpuSubmission BeginSubmission(UINT n,ID3D12CommandList*const*l){return P().lifetime.BeginSubmission(n,l);}
void QuarantineSubmission(UINT n,ID3D12CommandList*const*l){P().lifetime.QuarantineSubmission(n,l);}
void Reset(ID3D12CommandList*c){screenshot::Reset(c);auto& pool=P();std::lock_guard lock(pool.mutex);pool.lifetime.ResetRecording(c);CollectLocked(pool,ext::Active(ext::F::Hdr));}
}
