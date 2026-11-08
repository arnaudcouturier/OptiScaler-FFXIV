// Exercise the production SSR pass from inside native D3D11 Draw/DrawIndexed detours.
#define main OriginalSsrWarpMain
#include "ssr_warp.cpp"
#undef main

#include <detours/detours.h>
#include <windows.h>
#include <array>
#include <atomic>
#include "ext/core/D3D11DrawDispatch.h"

namespace {
using namespace ext::ffxiv::engine::ssr;
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
DrawFn g_draw = nullptr;
DrawIndexedFn g_drawIndexed = nullptr;
DrawFn g_resourceDraw = nullptr;
DrawIndexedFn g_resourceDrawIndexed = nullptr;
void* g_literalDraw = nullptr;
void* g_literalIndexed = nullptr;
bool g_resourceFirst = false;
bool g_legacyCached = false;
std::atomic<unsigned> g_legacyNativeCalls{0};
std::atomic<unsigned> g_resourceDrawCandidates{0}, g_resourceIndexedCandidates{0};
std::atomic<unsigned> g_resourceFreshFirstEntries{0}, g_resourceFreshDuplicateEntries{0};
ID3D11DeviceContext1* g_context = nullptr;
Pass* g_pass = nullptr;
PassInputs g_inputs{};
DiagnosticStages g_stage = DiagnosticStages::PipelineOnly;
bool g_diagCopy = false;
std::atomic<bool> g_armed{false};
thread_local bool g_reentrant = false;
std::atomic<unsigned> g_drawCalls{0}, g_indexedCalls{0}, g_injections{0}, g_originals{0};
std::atomic<unsigned> g_freshIndexedCallbacks{0}, g_outerIndexedCalls{0}, g_nativeIndexedForwards{0};
std::atomic<unsigned> g_nestedCalls{0};
std::atomic<unsigned> g_nullResults{0};
ID3D11DeviceContext* g_contextIdentity = nullptr;
void Inject(ID3D11DeviceContext* c) {
    if (!g_armed.load(std::memory_order_acquire) || g_reentrant || c != g_contextIdentity) return;
    g_reentrant = true;
    ++g_injections;
    auto* result = g_pass->Run(c, g_inputs, g_diagCopy, false, false, false, g_stage);
    if(!result)++g_nullResults;
    g_reentrant = false;
}
void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* c, UINT n, UINT start) {
    ++g_drawCalls;
    if (g_reentrant) ++g_nestedCalls;
    if (ext::core::draw_dispatch::IsFreshReentry(c, ext::core::draw_dispatch::Api::Draw)) { g_draw(c,n,start); return; }
    Inject(c);
    ext::core::draw_dispatch::Forward(c,13,g_literalDraw,g_draw,ext::core::draw_dispatch::Api::Draw,n,start);
    ++g_originals;
}
void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    ++g_indexedCalls;
    if (g_reentrant) ++g_nestedCalls;
    if (ext::core::draw_dispatch::IsFreshReentry(c, ext::core::draw_dispatch::Api::Indexed)) { ++g_freshIndexedCallbacks; g_drawIndexed(c,n,start,base); return; }
    ++g_outerIndexedCalls;
    Inject(c);
    ext::core::draw_dispatch::Forward(c,12,g_literalIndexed,g_drawIndexed,ext::core::draw_dispatch::Api::Indexed,n,start,base);
    ++g_nativeIndexedForwards;
    ++g_originals;
}
void STDMETHODCALLTYPE ResourceDraw(ID3D11DeviceContext* c, UINT n, UINT start) {
    using namespace ext::core::draw_dispatch;
    const bool fresh = IsFreshReentry(c, Api::Draw);
    LayerScope layer(c, Api::Draw, reinterpret_cast<void*>(&ResourceDraw));
    Forward(c,13,g_literalDraw,g_resourceDraw,Api::Draw,n,start);
    if (!(fresh && !layer)) ++g_resourceDrawCandidates;
}
void STDMETHODCALLTYPE ResourceDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    using namespace ext::core::draw_dispatch;
    const bool fresh = IsFreshReentry(c, Api::Indexed);
    LayerScope layer(c, Api::Indexed, reinterpret_cast<void*>(&ResourceDrawIndexed));
    Forward(c,12,g_literalIndexed,g_resourceDrawIndexed,Api::Indexed,n,start,base);
    if (fresh && layer) ++g_resourceFreshFirstEntries;
    if (fresh && !layer) ++g_resourceFreshDuplicateEntries;
    if (!(fresh && !layer)) ++g_resourceIndexedCandidates;
}
bool LayerTransaction(bool applicationLayer, bool on) {
    if (DetourTransactionBegin()!=NO_ERROR) return false;
    bool ok=DetourUpdateThread(GetCurrentThread())==NO_ERROR;
    if (applicationLayer) {
        if(on) { ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_draw),HookDraw)==NO_ERROR; ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_drawIndexed),HookDrawIndexed)==NO_ERROR; }
        else { ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_draw),HookDraw)==NO_ERROR; ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_drawIndexed),HookDrawIndexed)==NO_ERROR; }
    } else {
        if(on) { ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_resourceDraw),ResourceDraw)==NO_ERROR; ok &= DetourAttach(reinterpret_cast<PVOID*>(&g_resourceDrawIndexed),ResourceDrawIndexed)==NO_ERROR; }
        else { ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_resourceDraw),ResourceDraw)==NO_ERROR; ok &= DetourDetach(reinterpret_cast<PVOID*>(&g_resourceDrawIndexed),ResourceDrawIndexed)==NO_ERROR; }
    }
    const LONG committed=DetourTransactionCommit();
    return committed==NO_ERROR && ok;
}
bool Attach(bool on) {
    // Match production installers: each layer is committed in its own transaction.
    const bool firstIsResource = g_resourceFirst == on;
    if (!LayerTransaction(!firstIsResource, on)) return false;
    if (!LayerTransaction(firstIsResource, on)) return false;
    return true;
}
std::wstring OwnerPath(void* address) {
    HMODULE owner=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&owner))return L"<unknown>";
    wchar_t path[32768]{};DWORD n=GetModuleFileNameW(owner,path,static_cast<DWORD>(std::size(path)));return n?std::wstring(path,n):L"<unknown>";
}
struct CbBinding { ComPtr<ID3D11Buffer> buffer; UINT first=0,count=0; };
CbBinding GetPS0(ID3D11DeviceContext1* c) { CbBinding b;ID3D11Buffer* p=nullptr;c->PSGetConstantBuffers1(0,1,&p,&b.first,&b.count);b.buffer.Attach(p);return b; }
void* GetRtv0(ID3D11DeviceContext* c) { ID3D11RenderTargetView* p=nullptr;c->OMGetRenderTargets(1,&p,nullptr);void* id=p;if(p)p->Release();return id; }
bool Target(ID3D11Device* d, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv) {
    D3D11_TEXTURE2D_DESC td{}; td.Width=64; td.Height=64; td.MipLevels=1; td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(d->CreateTexture2D(&td,nullptr,&tex)) && SUCCEEDED(d->CreateRenderTargetView(tex.Get(),nullptr,&rtv));
}
bool Read(Harness& h, ID3D11Texture2D* src, uint8_t pixel[4], unsigned& colored) {
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; if(FAILED(h.device->CreateTexture2D(&d,nullptr,&stage))) return false;
    h.context->CopyResource(stage.Get(),src); D3D11_MAPPED_SUBRESOURCE m{}; HRESULT mapHr=h.context->Map(stage.Get(),0,D3D11_MAP_READ,0,&m); std::printf("readback Map HRESULT=0x%08lx\n",mapHr); if(FAILED(mapHr)) return false;
    auto* p=static_cast<const uint8_t*>(m.pData); std::memcpy(pixel,p+m.RowPitch*32+32*4,4); colored=0;
    for(UINT y=0;y<64;y++) for(UINT x=0;x<64;x++) if(p[m.RowPitch*y+x*4+2]<200) ++colored;
    h.context->Unmap(stage.Get(),0); return true;
}
void Clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* r) { const float blue[4]{0,0,1,1}; c->ClearRenderTargetView(r,blue); }
}

int main(int argc,char** argv) {
    if(argc!=4){std::puts("usage: draw_dispatch_layers_hw <N|L|O|P|ALL> <diagCopy 0|1> <RESOURCE_FIRST|APP_FIRST|LEGACY_CACHE>");return 2;}
    const std::string mode=argv[1]; g_diagCopy=std::strcmp(argv[2],"1")==0;
    g_resourceFirst=std::strcmp(argv[3],"RESOURCE_FIRST")==0;
    g_legacyCached=std::strcmp(argv[3],"LEGACY_CACHE")==0;
    if(mode=="ALL")g_stage=DiagnosticStages::All;
    else if(mode=="N")g_stage=DiagnosticStages::SwapOnly; else if(mode=="L")g_stage=DiagnosticStages::PipelineOnly;
    else if(mode=="O")g_stage=DiagnosticStages::TargetBindingsOnly; else if(mode=="P")g_stage=DiagnosticStages::OtherBindingsOnly; else return 2;
    wchar_t sys[MAX_PATH]{}; UINT sl=GetSystemDirectoryW(sys,MAX_PATH); std::wstring dll(sys,sl); dll+=L"\\d3d11.dll";
    HMODULE mod=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32); if(!mod)return 3;
    using Create=HRESULT(WINAPI*)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
    auto create=reinterpret_cast<Create>(GetProcAddress(mod,"D3D11CreateDevice")); Harness h;
    D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1},actual{};
    HRESULT hr=create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,requested,1,D3D11_SDK_VERSION,&h.device,&actual,&h.context);
    if(FAILED(hr)||!h.device||!h.context){std::printf("device failure hr=0x%08lx\n",hr);return 3;}
    ComPtr<IDXGIDevice> xd;ComPtr<IDXGIAdapter> ad;DXGI_ADAPTER_DESC desc{};h.device.As(&xd);xd->GetAdapter(&ad);ad->GetDesc(&desc);
    auto deviceOwner=*reinterpret_cast<void***>(h.device.Get()); auto contextOwner=*reinterpret_cast<void***>(h.context.Get());
    std::printf("Factory=%ls adapter=%ls vendor=%04x device=%04x FL=%x deviceVtableOwner=%ls contextVtableOwner=%ls\n",dll.c_str(),desc.Description,desc.VendorId,desc.DeviceId,actual,OwnerPath(deviceOwner[0]).c_str(),OwnerPath(contextOwner[0]).c_str());
    CHECK(desc.VendorId==0x10de && desc.DeviceId==0x2684 && actual>=D3D_FEATURE_LEVEL_11_1);
    ComPtr<ID3D11DeviceContext1> c1; CHECK(SUCCEEDED(h.context.As(&c1))); if(!c1)return Finish("ssr_draw_hook_injection_probe");
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> rtv;CHECK(Target(h.device.Get(),target,rtv));
    Frame frame=BuildScene(true,true,16,16);CHECK(h.Upload(frame));
    g_inputs.planar=h.planarView.Get();g_inputs.planarTexture=h.planarTexture.Get();g_inputs.vposition=h.positionView.Get();
    g_inputs.scene=h.sceneTexture.Get();g_inputs.camera=h.camera.Get();g_inputs.facts=frame.facts;g_inputs.params=frame.params;
    Pass pass;g_pass=&pass;g_context=c1.Get();g_contextIdentity=h.context.Get();
    void** vt=*reinterpret_cast<void***>(h.context.Get());g_literalDraw=vt[13];g_literalIndexed=vt[12];g_draw=reinterpret_cast<DrawFn>(vt[13]);g_drawIndexed=reinterpret_cast<DrawIndexedFn>(vt[12]);g_resourceDraw=reinterpret_cast<DrawFn>(vt[13]);g_resourceDrawIndexed=reinterpret_cast<DrawIndexedFn>(vt[12]);
    ID3D11RenderTargetView* raw=rtv.Get();h.context->OMSetRenderTargets(1,&raw,nullptr);D3D11_VIEWPORT vp{0,0,64,64,0,1};h.context->RSSetViewports(1,&vp);
    const char* shader="struct V{float3 p:POSITION;}; struct O{float4 p:SV_Position;}; O VS(V i){O o;o.p=float4(i.p,1);return o;} cbuffer C:register(b0){float4 color;} float4 PS():SV_Target{return color;}";
    ComPtr<ID3DBlob> vb,pb,err;CHECK(SUCCEEDED(D3DCompile(shader,strlen(shader),"draw-hook",nullptr,nullptr,"VS","vs_5_0",0,0,&vb,&err)));CHECK(SUCCEEDED(D3DCompile(shader,strlen(shader),"draw-hook",nullptr,nullptr,"PS","ps_5_0",0,0,&pb,&err)));
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;CHECK(SUCCEEDED(h.device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs)));CHECK(SUCCEEDED(h.device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps)));
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};ComPtr<ID3D11InputLayout> layout;CHECK(SUCCEEDED(h.device->CreateInputLayout(&element,1,vb->GetBufferPointer(),vb->GetBufferSize(),&layout)));
    const float vertices[9]{-1,-1,0,3,-1,0,-1,3,0};D3D11_BUFFER_DESC vbd{};vbd.ByteWidth=sizeof(vertices);vbd.Usage=D3D11_USAGE_IMMUTABLE;vbd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA init{};init.pSysMem=vertices;ComPtr<ID3D11Buffer> vertexBuffer;CHECK(SUCCEEDED(h.device->CreateBuffer(&vbd,&init,&vertexBuffer)));
    const uint16_t indices[3]{0,1,2};D3D11_BUFFER_DESC ibd{};ibd.ByteWidth=sizeof(indices);ibd.Usage=D3D11_USAGE_IMMUTABLE;ibd.BindFlags=D3D11_BIND_INDEX_BUFFER;init.pSysMem=indices;ComPtr<ID3D11Buffer> indexBuffer;CHECK(SUCCEEDED(h.device->CreateBuffer(&ibd,&init,&indexBuffer)));
    h.context->IASetInputLayout(layout.Get());h.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=12,offset=0;h.context->IASetVertexBuffers(0,1,vertexBuffer.GetAddressOf(),&stride,&offset);h.context->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);h.context->VSSetShader(vs.Get(),nullptr,0);h.context->PSSetShader(ps.Get(),nullptr,0);
    D3D11_BUFFER_DESC cbd{};cbd.ByteWidth=4096;cbd.Usage=D3D11_USAGE_DEFAULT;cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> colorBuffer;CHECK(SUCCEEDED(h.device->CreateBuffer(&cbd,nullptr,&colorBuffer)));float regs[256][4]{};regs[16][1]=1;regs[16][3]=1;h.context->UpdateSubresource(colorBuffer.Get(),0,nullptr,regs,0,0);ID3D11Buffer* cb=colorBuffer.Get();UINT first=16,count=16;c1->PSSetConstantBuffers1(0,1,&cb,&first,&count);c1->PSSetConstantBuffers1(2,1,&cb,&first,&count);c1->VSSetConstantBuffers1(0,1,&cb,&first,&count);c1->VSSetConstantBuffers1(1,1,&cb,&first,&count);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;CHECK(SUCCEEDED(h.device->CreateRasterizerState(&rd,&raster)));h.context->RSSetState(raster.Get());
    uint8_t before[4]{},after[4]{};unsigned cov0=0,cov1=0;
    Clear(h.context.Get(),rtv.Get());h.context->DrawIndexed(3,0,0);CHECK(Read(h,target.Get(),before,cov0));std::printf("baseline indexed pixel=%u,%u,%u,%u coverage=%u\n",before[0],before[1],before[2],before[3],cov0);
    CHECK(before[0]==0&&before[1]==255&&before[2]==0&&before[3]==255&&cov0>0);
    const auto rangeBefore=GetPS0(c1.Get());const void* rtvBefore=GetRtv0(h.context.Get());CHECK(rangeBefore.buffer.Get()==colorBuffer.Get()&&rangeBefore.first==16&&rangeBefore.count==16);
    if(!g_legacyCached){CHECK(Attach(true));g_armed=true;}
    constexpr unsigned kWarmIterations=100;
    for(unsigned i=0;i<kWarmIterations;i++) {
        raw=rtv.Get();h.context->OMSetRenderTargets(1,&raw,nullptr);h.context->RSSetViewports(1,&vp);h.context->RSSetState(raster.Get());
        h.context->IASetInputLayout(layout.Get());h.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);h.context->IASetVertexBuffers(0,1,vertexBuffer.GetAddressOf(),&stride,&offset);h.context->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);h.context->VSSetShader(vs.Get(),nullptr,0);h.context->PSSetShader(ps.Get(),nullptr,0);c1->PSSetConstantBuffers1(0,1,&cb,&first,&count);c1->PSSetConstantBuffers1(2,1,&cb,&first,&count);c1->VSSetConstantBuffers1(0,1,&cb,&first,&count);c1->VSSetConstantBuffers1(1,1,&cb,&first,&count);
        Clear(h.context.Get(),rtv.Get());
        if(g_legacyCached){auto* result=pass.Run(h.context.Get(),g_inputs,g_diagCopy,false,false,false,g_stage);if(!result)++g_nullResults;g_drawIndexed(h.context.Get(),3,0,0);++g_legacyNativeCalls;}
        else h.context->DrawIndexed(3,0,0);
    }
    g_armed=false;
    if(g_legacyCached){CHECK(g_indexedCalls==0&&g_injections==0&&g_legacyNativeCalls==kWarmIterations);}
    else {CHECK(g_outerIndexedCalls==kWarmIterations&&g_nativeIndexedForwards==kWarmIterations&&g_injections==kWarmIterations);CHECK(g_indexedCalls==g_outerIndexedCalls+g_freshIndexedCallbacks);CHECK(g_originals==g_drawCalls+g_outerIndexedCalls);CHECK(g_resourceIndexedCandidates==kWarmIterations);
        if(!g_diagCopy){if(g_resourceFirst)CHECK(g_resourceFreshFirstEntries==kWarmIterations&&g_resourceFreshDuplicateEntries==0);else CHECK(g_resourceFreshFirstEntries==0&&g_resourceFreshDuplicateEntries==kWarmIterations);}
        else CHECK(g_resourceFreshFirstEntries==0&&g_resourceFreshDuplicateEntries==0);
    }
    CHECK(g_nullResults==0);
    if(g_stage==DiagnosticStages::All) CHECK(g_drawCalls==(g_legacyCached?0:kWarmIterations)&&g_nestedCalls==(g_legacyCached?0:kWarmIterations)&&g_resourceDrawCandidates==(g_legacyCached?0:kWarmIterations)&&pass.ActualDrawSubmissions()>=1);
    else CHECK(g_drawCalls==0&&g_nestedCalls==0&&pass.ActualDrawSubmissions()==0);
    CHECK(Read(h,target.Get(),after,cov1));auto ops=pass.LastDiagnosticOperations();const auto rangeAfter=GetPS0(c1.Get());const void* rtvAfter=GetRtv0(h.context.Get());
    HRESULT deviceReason=h.device->GetDeviceRemovedReason();std::printf("device GetDeviceRemovedReason HRESULT=0x%08lx\n",deviceReason);CHECK(deviceReason==S_OK);
    std::printf("after pixel=%u,%u,%u,%u coverage=%u iterations=%u hookDraw=%u indexedCallbacks=%u outerIndexed=%u freshIndexed=%u nativeIndexedForwards=%u injections=%u originals=%u nested=%u nullResults=%u SSRdraws=%u cbRange=%u/%u->%u/%u cbIdentity=%d rtvIdentity=%d opsCopies=%u pipelineBinds=%u stateSwaps=%u diagCopy=%u resourceDrawCandidates=%u resourceIndexedCandidates=%u resourceFreshFirst=%u resourceFreshDuplicate=%u legacyNative=%u\n",after[0],after[1],after[2],after[3],cov1,kWarmIterations,g_drawCalls.load(),g_indexedCalls.load(),g_outerIndexedCalls.load(),g_freshIndexedCallbacks.load(),g_nativeIndexedForwards.load(),g_injections.load(),g_originals.load(),g_nestedCalls.load(),g_nullResults.load(),pass.ActualDrawSubmissions(),rangeBefore.first,rangeBefore.count,rangeAfter.first,rangeAfter.count,rangeBefore.buffer.Get()==rangeAfter.buffer.Get(),rtvBefore==rtvAfter,ops.copies,ops.pipelineBinds,ops.stateSwaps,unsigned(g_diagCopy),g_resourceDrawCandidates.load(),g_resourceIndexedCandidates.load(),g_resourceFreshFirstEntries.load(),g_resourceFreshDuplicateEntries.load(),g_legacyNativeCalls.load());
    CHECK(rangeBefore.buffer.Get()==rangeAfter.buffer.Get()&&rangeBefore.first==rangeAfter.first&&rangeBefore.count==rangeAfter.count);
    CHECK(rtvBefore==rtvAfter);
    if(g_legacyCached){CHECK(after[0]==0&&after[1]==0&&after[2]==255&&after[3]==255&&cov1==0);CHECK(g_legacyNativeCalls==100);std::puts("LEGACY EXPECTED RED: cached pointer produced exact blue zero-coverage output after S_OK Map");}
    else {CHECK(std::memcmp(before,after,4)==0&&cov0==cov1);CHECK(Attach(false));}
    CHECK(ext::core::draw_dispatch::currentFrame==nullptr && ext::core::draw_dispatch::currentLayerFrame==nullptr);
    return Finish("ssr_draw_hook_injection_probe");
}
