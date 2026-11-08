#include "pch.h"

#include "ext/ffxiv/engine/SsrApply.h"
#include "ext/core/InjectedStateWindowDiagnostic.h"
#include "ext/core/InjectedOmTraceDiagnostic.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/InjectedWorkDiagnostic.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame
#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC RDEF reader the engine's own index uses
#include "ext/ffxiv/diag/ShaderCrc.h"      // the CRC a passed-on line names the draw's shader by
#include "ext/ffxiv/engine/EnginePass.h"   // the module's own feature bit and publish
#include "ext/ffxiv/engine/SsrPass.h"
#include "ext/ffxiv/engine/WaterExecutionTrace.h"
#include "ext/ffxiv/engine/SsrPairRules.h"
#include "ext/ffxiv/engine/SsrNativeContext.h"
#include "ext/ffxiv/diag/DrawDiag.h"
#include "ext/ffxiv/chara/CharaProbeDump.h"
#include "ext/present/PresentSeams.h"      // the output-frame counter, the frame token

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <vector>

// Screen-space reflections (ext/ffxiv/engine): the seam's entries and the draw path (SsrApply.h). The pure decisions are in SsrRules.h, the GPU
// work in SsrPass.cpp.
//
// When a shader is stamped: while the game creates its pixel shaders, whatever the key says, because the game creates the shaders of its water
// material while it loads and a switch turned on later has to find them. A cheap byte scan for the reflection name drops every other shader before
// the reflection parse; nothing is logged while the key is off. The stamp is written on the shader object, so there is no table to fill and
// nothing to overflow.
//
// Cost with the key off: one relaxed load per indexed draw (the caller's) and nothing else - nothing here is reached for a draw whose shader the
// index did not claim as a reader of the planar map. Cost with the key on: one relaxed load per reader draw, one PSGetShaderResources for its
// slot, one GetResource, and the slot swap and its restore; the frame's first water draw that can serve the frame also runs the pass once.
namespace ext::ffxiv::engine::ssr
{
namespace
{
using Microsoft::WRL::ComPtr;

void DumpOmTrace(ID3D11DeviceContext* context)
{
    using namespace ext::core::om_trace;
    for (auto& c : captures)
    {
        if (!c.committed.load(std::memory_order_acquire) || c.id == 0) continue;
        bool expected = false;
        if (!c.dumped.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
        UINT telemetry = UINT_MAX;
        const bool haveTelemetry = ext::core::state_window::ReadTrackedOm(context, telemetry);
        const uint32_t count = (std::min)(c.reserved.load(std::memory_order_acquire), kRowBudget);
        LOG_INFO("[SSR-OMTRACE] capture={} owner={} context={:p} context1={:p} unwrapHr=0x{:08X} unwrapped={:p} identity={:p} vtable={:p} v0={:p} v33={:p} v34={:p} unwrapVtable={:p} unwrapV0={:p} unwrapV33={:p} unwrapV34={:p} rows={} complete={} inflight={} overflow={} partial={} committed={} trackedTelemetry={}=>{}",
                 c.id, c.owner, reinterpret_cast<void*>(c.context), reinterpret_cast<void*>(c.context1), uint32_t(c.unwrapHr),
                 reinterpret_cast<void*>(c.unwrapped), reinterpret_cast<void*>(c.identity), reinterpret_cast<void*>(c.vtable),
                 reinterpret_cast<void*>(c.vFirst), reinterpret_cast<void*>(c.v33), reinterpret_cast<void*>(c.v34),
                 reinterpret_cast<void*>(c.unwrapVtable), reinterpret_cast<void*>(c.unwrapVFirst), reinterpret_cast<void*>(c.unwrapV33),
                 reinterpret_cast<void*>(c.unwrapV34), count,
                 c.completed.load(), c.inFlight.load(), c.overflow.load(), c.partial.load() ? 1 : 0, c.committed.load() ? 1 : 0,
                 c.telemetryBefore == UINT32_MAX ? -1 : int(c.telemetryBefore), c.telemetryAfter == UINT32_MAX ? (haveTelemetry ? int(telemetry) : -1) : int(c.telemetryAfter));
        LOG_INFO("[SSR-OMTRACE] id={} incomingIdentity={:p} typedUnwrappedContext={:p}", c.id,
                 reinterpret_cast<void*>(c.incomingIdentity), reinterpret_cast<void*>(c.unwrappedContext));
        const auto logRouteAddress = [&c](const char* label, uintptr_t address)
        {
            if (!address) return;
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCSTR>(address), &module))
            {
                char moduleName[MAX_PATH] {};
                GetModuleFileNameA(module, moduleName, MAX_PATH);
                LOG_INFO("[SSR-OMTRACE] id={} route={} address={:p} module={}+0x{:X}", c.id, label,
                         reinterpret_cast<void*>(address), moduleName, address - reinterpret_cast<uintptr_t>(module));
            }
            else
                LOG_INFO("[SSR-OMTRACE] id={} route={} address={:p} module=unresolved", c.id, label, reinterpret_cast<void*>(address));
        };
        logRouteAddress("incoming-v0", c.vFirst);
        logRouteAddress("incoming-v33", c.v33);
        logRouteAddress("incoming-v34", c.v34);
        logRouteAddress("unwrapped-v0", c.unwrapVFirst);
        logRouteAddress("unwrapped-v33", c.unwrapV33);
        logRouteAddress("unwrapped-v34", c.unwrapV34);
        for (uint32_t i = 0; i < count; ++i)
        {
            const Row& r = c.rows[i];
            if (!r.ready.load(std::memory_order_acquire)) continue;
            LOG_INFO("[SSR-OMTRACE] id={} row={} phase={} api={} decision={} requested={} count={}=>{} thread={} owner={} context={:p} immediateContext={:p} rtv0={:p} dsv={:p} caller={:p}",
                     r.id, i, r.phase, r.api, r.decision, r.requested, r.oldCount, r.newCount, r.thread, r.owner,
                     reinterpret_cast<void*>(r.context), reinterpret_cast<void*>(r.immediateContext), reinterpret_cast<void*>(r.firstRtv), reinterpret_cast<void*>(r.dsv), reinterpret_cast<void*>(r.callsite));
            for (uint32_t j = 0; j < r.stackCount; ++j)
            {
                HMODULE module = nullptr;
                const auto address = reinterpret_cast<LPCSTR>(r.stack[j]);
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, address, &module))
                {
                    char moduleName[MAX_PATH] {};
                    GetModuleFileNameA(module, moduleName, MAX_PATH);
                    LOG_INFO("[SSR-OMTRACE] id={} row={} stack{}={:p} module={}+0x{:X}", r.id, i, j,
                             reinterpret_cast<void*>(r.stack[j]), moduleName, r.stack[j] - reinterpret_cast<uintptr_t>(module));
                }
                else
                    LOG_INFO("[SSR-OMTRACE] id={} row={} stack{}={:p} module=unresolved", r.id, i, j, reinterpret_cast<void*>(r.stack[j]));
            }
        }
    }
}

// The one name that makes a shader a sampler of the engine's planar reflection map: the shared table's own row, the row the draw path classifies
// a reader with.
constexpr std::string_view kReflectionName = engine::kNames[11].name;
static_assert(kReflectionName == "g_SamplerReflectionMap",
              "the reflection name is the shared table's row (the row the draw path's IsReflectionDraw reads): a row that moved would leave every "
              "water reader unstamped");

// Whether the bytecode contains `name` as a byte string: the cheap test that drops the shaders that cannot be a reader before the reflection
// parse. Deliberately a plain scan, like the shared index's.
bool Mentions(const void* code, size_t size, std::string_view name)
{
    if (code == nullptr || size < name.size())
        return false;
    const auto* bytes = static_cast<const unsigned char*>(code);
    for (size_t i = 0; i + name.size() <= size; ++i)
        if (bytes[i] == static_cast<unsigned char>(name[0]) && std::memcmp(bytes + i, name.data(), name.size()) == 0)
            return true;
    return false;
}

// The slots one shader declares come from the pure rules (SsrRules.h's SlotsOfShader), which the tests parse a compiled shader with; the parse
// itself is not repeated here.

// One bound texture as the classification needs it: its format and size, the view's own dimension and array size, the resource behind it and its
// sample count. The caller keeps the view's reference; nothing here owns it.
struct BoundTexture
{
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11Texture2D* texture = nullptr;
    const void* resource = nullptr;
    uint32_t format = 0, width = 0, height = 0, samples = 0;
    uint32_t viewDimension = 0; // D3D11_SRV_DIMENSION of the view, for the read binding a pass-on reports
    uint32_t arraySize = 0;
    bool twoD = false;
};

BoundTexture Describe(ID3D11ShaderResourceView* view)
{
    BoundTexture out;
    if (view == nullptr)
        return out;
    out.view = view;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    view->GetDesc(&desc);
    out.format = uint32_t(desc.Format);
    out.samples = 1;
    out.viewDimension = uint32_t(desc.ViewDimension);
    out.twoD = desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!resource)
        return out;
    out.resource = resource.Get();
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        out.twoD = false;
        return out;
    }
    D3D11_TEXTURE2D_DESC texture {};
    static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&texture);
    out.texture = static_cast<ID3D11Texture2D*>(resource.Get());
    out.width = texture.Width;
    out.height = texture.Height;
    out.samples = texture.SampleDesc.Count;
    out.arraySize = texture.ArraySize;
    out.twoD = out.twoD && texture.ArraySize == 1;
    return out;
}

struct State
{
    Pass pass;
    Session session;
    LogGate log;
    Params params;
    PassedOnGate passedOnGate; // one line a session per passed-on reason, out of the same budget
    // The frame's memory of the engine's planar map, its view and this module's result (SsrRules.h's FrameCache shape).
    FrameCache frame;
    ComPtr<ID3D11ShaderResourceView> selectedView;
    std::atomic<bool> diagnosticOn { false };
    uint32_t diagnosticMode = 0, diagnosticCopies = 0, diagnosticBypasses = 0;
    uint32_t diagnosticForeignDrawsLastRun = 0;
    bool diagnosticJVerifiedDistinct = false;
    bool diagnosticJRouteChanged = false;
    bool diagnosticJCompatible = false;
    bool diagnosticJPassCompleted = false;
    uint32_t diagnosticLoggedMode = UINT32_MAX;
    uint64_t diagnosticSuppressedCallbacks = 0;
    bool diagnosticSuppressionActiveDuringPass = false;
    uint64_t diagnosticKeyFrame = UINT64_MAX;
    bool diagnosticF7Down = false;
    std::atomic<bool> keyOn { false };
    std::atomic<bool> armed { false };  // the published feature bit's own condition (the shared arming asks for it)
    std::atomic<bool> inPass { false }; // true while this module's own GPU work runs
    uint32_t runs = 0;
    uint32_t readers = 0;
    uint32_t declined = 0;
    uint32_t servedLastFrame = 0; // the water draws of the last finished frame
    uint32_t declines[kDeclineReasonCount] {};
    uint32_t passedOn[kPassedOnCount] {}; // draws passed on before the frame's one attempt, one count per reason
    ReaderFacts lastFacts;
    ReadBinding read; // the slot and the view the last water draw the gate passed on had read (OtherMap), for a capture
    Params lastParams; // the tuning the last successful run drew with (the run line is written by a later run)
    bool haveRun = false;
    bool haveArmed = false; // a card state has been published: the first publish logs its state, every change logs the new one
    bool loggedOff = false;
    bool loggedRun = false;    // the one line that says what the pass computes
    bool loggedServed = false; // the one line that says how many readers a frame served
    bool loggedFailure = false; // the first failure's reason, once
    // Independent of the draw table: a pair can finish draining after F8's sixteen rows are complete.
    struct PairImage { std::string name; ComPtr<ID3D11Resource> staging; D3D11_TEXTURE2D_DESC desc{}; uint32_t rowBytes=0, rows=1, format=0; uint64_t bytes=0; };
    struct PairSlot { uint64_t token=0, run=0; bool complete=true; std::vector<PairImage> images; };
    PairSlot pair[2];
    uint32_t pairRuns=0;
    bool pairArmed=false, pairFinished=false;
    uint64_t pairBytes=0;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

void PollPair(ID3D11DeviceContext* context, State& state)
{
    if (context == nullptr || state.pairRuns != 2) return;
    for (auto& slot : state.pair) for (auto& image : slot.images) {
        if (!image.staging) continue;
        D3D11_MAPPED_SUBRESOURCE map{};
        HRESULT hr=context->Map(image.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
        if (hr==DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (FAILED(hr) || !map.pData) { state.pairBytes-=image.bytes; image.staging.Reset(); LOG_WARN("SSR pair capture: staging map failed for {}",image.name); continue; }
        static const auto root=ext::ffxiv::chara::probe::dump::OutputRoot();
        auto dir=root/L"SsrFramePair"/(std::to_wstring(state.pair[0].token)+L"-"+std::to_wstring(state.pair[0].run));
        std::error_code ec; std::filesystem::create_directories(dir,ec);
        const std::string filename=pair::FileName(static_cast<uint32_t>(&slot==&state.pair[1]),image.name);
        std::ofstream out(dir/(std::filesystem::path(filename).wstring()+L".raw"),std::ios::binary);
        if(out) for(uint32_t y=0;y<image.rows;++y) out.write(static_cast<const char*>(map.pData)+size_t(y)*map.RowPitch,image.rowBytes);
        context->Unmap(image.staging.Get(),0);
        if(!out) LOG_WARN("SSR pair capture: failed to write {}",image.name);
        else { const auto manifest=dir/L"manifest.csv"; const bool exists=std::filesystem::exists(manifest); std::ofstream meta(manifest,std::ios::app); if(meta) { if(!exists) meta<<pair::kManifestHeader; meta<<filename<<".raw,"<<slot.token<<","<<slot.run<<","<<image.desc.Width<<","<<image.desc.Height<<","<<image.format<<","<<image.rowBytes<<"\n"; } }
        image.staging.Reset(); state.pairBytes-=image.bytes;
    }
    bool pending=false; for(auto& slot:state.pair) for(auto& image:slot.images) pending|=bool(image.staging);
    if(!pending && !state.pairFinished) { state.pairFinished=true; LOG_INFO("SSR pair capture: raw images drained under {}",ext::ffxiv::chara::probe::dump::OutputRoot().string()); }
}

void DiscardPairSlot(State& state, State::PairSlot& slot)
{
    for(auto& image:slot.images) state.pairBytes-=image.bytes;
    slot.images.clear(); slot.complete=true;
}

void ResetPair(State& state)
{
    for(auto& slot:state.pair) DiscardPairSlot(state,slot);
    state.pairBytes=0; state.pairRuns=0; state.pairFinished=true;
}

bool StagePairTexture(ID3D11Device* device, ID3D11DeviceContext* context, State& state, State::PairSlot& slot, const char* name, ID3D11Texture2D* source)
{
    if(!source) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    uint32_t bpp=0;
    switch(desc.Format) { case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: bpp=1; break; case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: bpp=2; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: bpp=4; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: bpp=8; break; case DXGI_FORMAT_R32G32B32A32_FLOAT: bpp=16; break; default: return false; }
    const uint32_t rowBytes=desc.Width*bpp; const uint64_t bytes=uint64_t(rowBytes)*desc.Height;
    if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || desc.MipLevels!=1 || !pair::Fits(state.pairBytes,bytes)) return false;
    D3D11_TEXTURE2D_DESC stage=desc; stage.Usage=D3D11_USAGE_STAGING; stage.BindFlags=0; stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ; stage.MiscFlags=0;
    State::PairImage image; image.name=name; image.desc=desc; image.rowBytes=rowBytes; image.rows=desc.Height; image.format=desc.Format; image.bytes=bytes;
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(device->CreateTexture2D(&stage,nullptr,&texture))) return false;
    context->CopyResource(texture.Get(),source); image.staging=texture; state.pairBytes+=bytes; slot.images.push_back(std::move(image)); return true;
}

bool StagePairBuffer(ID3D11Device* device, ID3D11DeviceContext* context, State& state, State::PairSlot& slot, const char* name, ID3D11Buffer* source)
{
    if(!source) return false; D3D11_BUFFER_DESC d{}; source->GetDesc(&d); if(!pair::Fits(state.pairBytes,d.ByteWidth)) return false;
    D3D11_BUFFER_DESC sd=d; sd.Usage=D3D11_USAGE_STAGING; sd.BindFlags=0; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ; sd.MiscFlags=0;
    ComPtr<ID3D11Buffer> stage; if(FAILED(device->CreateBuffer(&sd,nullptr,&stage))) return false;
    context->CopyResource(stage.Get(),source); State::PairImage image; image.name=name; image.staging=stage; image.bytes=d.ByteWidth; image.rowBytes=d.ByteWidth; image.rows=1; image.format=0; slot.images.push_back(std::move(image)); state.pairBytes+=d.ByteWidth; return true;
}

// Marks this module's own GPU work: its draw re-enters the draw seam (it is drawn on the game's own immediate context) and the shared module must
// never classify or count it. A nested guard restores, never clears.
struct OwnWork
{
    const bool previous;
    OwnWork() : previous(S().inPass.exchange(true, std::memory_order_relaxed))
    {
        if (!previous)
            engine::SetOwnWork(true);
    }
    ~OwnWork()
    {
        if (!previous)
            engine::SetOwnWork(false);
        S().inPass.store(previous, std::memory_order_relaxed);
    }
    OwnWork(const OwnWork&) = delete;
    OwnWork& operator=(const OwnWork&) = delete;
};

// One bound view the caller owns once, and either releases or hands to a swap guard.
struct ViewRef
{
    ID3D11ShaderResourceView* view = nullptr;
    ~ViewRef() { Reset(); }
    void Reset()
    {
        if (view != nullptr)
            view->Release();
        view = nullptr;
    }
    ID3D11ShaderResourceView* Release()
    {
        ID3D11ShaderResourceView* released = view;
        view = nullptr;
        return released;
    }
};

// One refused draw. Every reason leaves the draw exactly as the game made it, and each is one line of the log the first time it happens.
// `line` replaces the short line when the caller has something more to say (the first water draw of a frame reports what it bound, so a session
// can tell the two shapes that refuse alike apart).
Decline Refuse(Decline reason, std::string_view line = {})
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Ssr, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    State& state = S();
    state.declined++;
    if (uint32_t(reason) < kDeclineReasonCount)
        state.declines[uint32_t(reason)]++;
    if (state.log.Take(uint32_t(reason)))
        LOG_INFO("{}", line.empty() ? std::string(FormatDecline(reason)) : std::string(line));
    if (state.session.NoteDecline() && !state.loggedOff)
    {
        state.loggedOff = true;
        LOG_WARN("{}", FormatSwitchedOff());
    }
    return reason;
}

// One draw passed on before the frame's one attempt: counted, and written once a session per reason. Deliberately not a refusal - the draw
// keeps the engine's texel, the session's refusal streak is untouched and the module never stands itself down for a draw it does not answer for -
// and deliberately not silent either: a session whose card was on and whose log had no run line reads here which condition stopped the water
// draws. The shader's CRC is named where the draw's own shader is at hand, and so is the binding the slot held where the draw path read one, so
// the bytecode a session could not use and the texture its stamp pointed at are both identified.
void NotePassedOn(PassedOn reason, ID3D11PixelShader* shader = nullptr, const ReadBinding* read = nullptr)
{
    State& state = S();
    if (uint32_t(reason) >= kPassedOnCount)
        return;
    state.passedOn[uint32_t(reason)]++;
    if (read != nullptr && read->have)
        state.read = *read; // a capture reports the last water draw the gate passed on, not the last reason it wrote a line for
    if (state.passedOnGate.Take(reason, state.log))
        LOG_INFO("{}", FormatPassedOn(reason, ext::ffxiv::diag::ReadShaderCrc(shader), read));
}

// The frame's one attempt: reads the water draw's inputs, decides, and runs the pass. Returns this module's view for the frame, or null after a
// refusal (which is counted here).
ID3D11ShaderResourceView* RunFrame(ID3D11DeviceContext* context, State& state, const ReaderSlots& slots, const BoundTexture& planar)
{
    PollPair(context,state);
    ReaderFacts facts;
    facts.hasPlanar = true;
    facts.planarFormat = planar.format;
    facts.planarWidth = planar.width;
    facts.planarHeight = planar.height;
    facts.planarSamples = planar.samples;
    facts.replay = ext::ffxiv::chara::t_replay;

    ComPtr<ID3D11ShaderResourceView> vposition;
    if (slots.HasVPosition())
        context->PSGetShaderResources(slots.vposition, 1, &vposition);
    if (const BoundTexture described = Describe(vposition.Get()); described.view != nullptr)
    {
        facts.vpositionFormat = described.format;
        facts.vpositionWidth = described.width;
        facts.vpositionHeight = described.height;
        facts.vpositionSamples = described.samples;
        facts.hasVPosition = described.twoD && described.width > 0;
    }

    ComPtr<ID3D11Buffer> camera;
    if (slots.HasCamera())
        context->PSGetConstantBuffers(slots.camera, 1, &camera);
    if (camera)
    {
        D3D11_BUFFER_DESC desc {};
        camera->GetDesc(&desc);
        facts.cameraBytes = desc.ByteWidth;
        facts.hasCamera = desc.ByteWidth > 0;
    }

    // The scene the frame had rendered when this draw arrived: the draw's own first render target. It is a copy source only, so nothing of the
    // game is written; a draw that writes no colour target is not this pass's draw.
    ComPtr<ID3D11Texture2D> scene;
    {
        ID3D11RenderTargetView* target = nullptr;
        context->OMGetRenderTargets(1, &target, nullptr);
        ComPtr<ID3D11RenderTargetView> view;
        view.Attach(target);
        if (view)
        {
            ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (resource)
                resource->GetType(&dimension);
            if (resource && dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            {
                scene = static_cast<ID3D11Texture2D*>(resource.Get());
                D3D11_TEXTURE2D_DESC desc {};
                scene->GetDesc(&desc);
                facts.sceneFormat = uint32_t(desc.Format);
                facts.sceneWidth = desc.Width;
                facts.sceneHeight = desc.Height;
                facts.sceneSamples = desc.SampleDesc.Count;
                facts.hasScene = desc.ArraySize == 1 && desc.Width > 0;
            }
        }
    }

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    // The engine renders the picture at a half-pixel viewport origin. The march works on scene texels, whose grid has no half pixel, so the window
    // the mapping and the region are computed from is the texels the picture covers: the origin is floored rather than carried as 0.5, which would
    // put our texel 0 on the boundary of scene texel 0 and one texel 1 on scene texel 2.
    facts.window[0] = std::floor(viewport.TopLeftX);
    facts.window[1] = std::floor(viewport.TopLeftY);
    facts.window[2] = viewport.Width;
    facts.window[3] = viewport.Height;

    const Decline reason = Classify(facts);
    if (reason != Decline::None)
    {
        Refuse(reason, FormatRefusedReader(reason, facts, slots));
        return nullptr;
    }

    PassInputs passInputs;
    passInputs.planar = planar.view;
    passInputs.planarTexture = planar.texture;
    passInputs.vposition = vposition.Get();
    passInputs.scene = scene.Get();
    passInputs.camera = camera.Get();
    passInputs.facts = facts;
    passInputs.params = state.params;

    // I preserves reader classification and the frame's one-attempt bookkeeping, then returns the
    // engine's original view without staging, querying, mapping, or invoking any pass work.
    if (state.diagnosticOn.load(std::memory_order_relaxed) && state.diagnosticMode == 8)
    {
        state.frame.Record(planar.resource, planar.view, nullptr);
        state.selectedView = planar.view;
        ++state.diagnosticBypasses;
        if (state.diagnosticBypasses == 1 || state.diagnosticLoggedMode != 8)
            LOG_INFO("SSR diagnostic mode I: bypasses {}, Pass::Run calls and pass counters unchanged", state.diagnosticBypasses);
        state.diagnosticLoggedMode = 8;
        return planar.view;
    }

    // Only the actual capture-start callback arms this path. The draw-diagnostic feature bit
    // also stays on while waiting for F8, so it cannot distinguish waiting from capturing.
    const bool pairThisRun=!(state.diagnosticOn.load(std::memory_order_relaxed) && state.diagnosticMode >= 9) &&
                           state.pairArmed && !state.pairFinished && pair::IsPairSlot(state.pairRuns);
    State::PairSlot* pairSlot=pairThisRun ? &state.pair[state.pairRuns] : nullptr;
    ComPtr<ID3D11Device> pairDevice;
    if(pairSlot) {
        context->GetDevice(&pairDevice);
        pairSlot->token=ext::present::FrameCount(); pairSlot->run=state.runs+1;
        // These copies are enqueued before Pass::Run copies/uses inputs or writes its result.
        const bool copied=StagePairTexture(pairDevice.Get(),context,state,*pairSlot,"view-position",Describe(vposition.Get()).texture) &&
                          StagePairTexture(pairDevice.Get(),context,state,*pairSlot,"planar-source",planar.texture) &&
                          StagePairBuffer(pairDevice.Get(),context,state,*pairSlot,"camera-source",camera.Get());
        if(!copied) { pairSlot->complete=false; LOG_WARN("SSR pair capture: input staging failed or exceeded 512 MiB; capture refused"); }
    }

    ComPtr<ID3D11DeviceContext> nativeContext;
    ComPtr<ID3D11Device> incomingResourceDevice;
    ID3D11DeviceContext* passContext = context;
    bool nativeRouteVerified = false;
    if (state.diagnosticOn.load(std::memory_order_relaxed) && state.diagnosticMode == 9)
    {
        state.diagnosticJVerifiedDistinct = false;
        state.diagnosticJRouteChanged = false;
        state.diagnosticJCompatible = false;
        state.diagnosticJPassCompleted = false;
        ComPtr<ID3D11Device> sceneDevice, nativeDevice;
        if (scene) scene->GetDevice(&sceneDevice);
        const auto sceneDeviceId = native_context::Identity(sceneDevice.Get());
        if (sceneDeviceId) sceneDeviceId.As(&nativeDevice);
        if (nativeDevice)
        {
            nativeDevice->GetImmediateContext(&nativeContext);
            ComPtr<ID3D11Device> contextDevice, vpositionDevice, cameraDevice;
            context->GetDevice(&contextDevice);
            planar.texture->GetDevice(&incomingResourceDevice);
            if (vposition) vposition->GetDevice(&vpositionDevice);
            if (camera) camera->GetDevice(&cameraDevice);
            nativeRouteVerified = nativeContext && native_context::VerifiedRoute(context, nativeContext.Get(), nativeDevice.Get(),
                                                                                 scene.Get(), planar.texture, vposition.Get(), camera.Get());
            if (!nativeRouteVerified && state.diagnosticLoggedMode != 9)
            {
                LOG_INFO("SSR diagnostic J verification components: incomingCandidate={}, incomingDevice={}, planarDevice={}, vpositionDevice={}, cameraDevice={}",
                         native_context::Same(context, nativeContext.Get()), native_context::Same(contextDevice.Get(), nativeDevice.Get()),
                         native_context::Same(incomingResourceDevice.Get(), nativeDevice.Get()),
                         !vposition || native_context::Same(vpositionDevice.Get(), nativeDevice.Get()),
                         !camera || native_context::Same(cameraDevice.Get(), nativeDevice.Get()));
            }
        }
        static bool loggedIdentity = false;
        const auto incomingId = native_context::Identity(context);
        const auto candidateId = native_context::Identity(nativeContext.Get());
        const auto resourceId = native_context::Identity(incomingResourceDevice.Get());
        if (!loggedIdentity)
        {
            LOG_INFO("SSR diagnostic J identity: incoming={:p}, resourceDevice={:p}, candidateImmediate={:p}, unwrappedIncoming={:p}, unwrappedResourceDevice={:p}, unwrappedCandidate={:p}, match={}, rawContextDistinct={}",
                     static_cast<void*>(context), static_cast<void*>(incomingResourceDevice.Get()), static_cast<void*>(nativeContext.Get()),
                     static_cast<void*>(incomingId.Get()), static_cast<void*>(resourceId.Get()), static_cast<void*>(candidateId.Get()), nativeRouteVerified ? "yes" : "no",
                     context != nativeContext.Get() ? "yes" : "no");
            loggedIdentity = true;
        }
        state.diagnosticJRouteChanged = nativeContext && context != nativeContext.Get();
        state.diagnosticJCompatible = nativeRouteVerified;
        if (nativeRouteVerified) passContext = nativeContext.Get();
        else
        {
            state.diagnosticJVerifiedDistinct = false;
            state.diagnosticJRouteChanged = nativeContext && context != nativeContext.Get();
            ++state.diagnosticBypasses;
            if (state.diagnosticLoggedMode != 9)
                LOG_WARN("SSR diagnostic J route comparison NOT EXERCISED: raw incoming and candidate contexts {}; compatibility verification failed; engine map retained", state.diagnosticJRouteChanged ? "differ" : "match");
            state.diagnosticLoggedMode = 9;
            state.frame.Record(planar.resource, planar.view, nullptr);
            state.selectedView = planar.view;
            return planar.view;
        }
    }

    ID3D11ShaderResourceView* ours = nullptr;
    {
        // The character preview's marker is raised around the work too, so its copy and clear hooks do not count this module's draw as the
        // game's.
        OwnWork own;
        ext::ffxiv::chara::ReplayScope replay;
        const bool diagnostic = state.diagnosticOn.load(std::memory_order_relaxed);
        const uint64_t foreignDrawsBefore = ext::core::state_window::Read().count;
        const bool suppressObservers = diagnostic && state.diagnosticMode == 4;
        state.diagnosticSuppressionActiveDuringPass = false;
        const uint64_t suppressedBefore = diagnostic ? ext::core::diagnostic::TotalSuppressedCallbacks() : 0;
        if (suppressObservers)
        {
            ext::core::diagnostic::SuppressObserversScope suppression(true);
            state.diagnosticSuppressionActiveDuringPass = ext::core::diagnostic::ObserversSuppressed();
            const auto stages = !diagnostic ? DiagnosticStages::All : state.diagnosticMode == 13 ? DiagnosticStages::SwapOnly : state.diagnosticMode == 10 ? DiagnosticStages::PreparationOnly :
                            state.diagnosticMode == 11 ? DiagnosticStages::PipelineOnly : state.diagnosticMode == 12 ? DiagnosticStages::CombinedNoDraw :
                            state.diagnosticMode == 14 ? DiagnosticStages::TargetBindingsOnly : state.diagnosticMode == 15 ? DiagnosticStages::OtherBindingsOnly : DiagnosticStages::All;
            const bool splitMode = state.diagnosticMode >= 10 && state.diagnosticMode <= 12;
            ours = state.pass.Run(passContext, passInputs, diagnostic, diagnostic && (state.diagnosticMode >= 5 && state.diagnosticMode <= 7 || state.diagnosticMode == 12),
                                  diagnostic && (state.diagnosticMode == 6 || state.diagnosticMode == 11 || splitMode), diagnostic && state.diagnosticMode == 7, stages);
        }
        else
        {
            const auto stages = !diagnostic ? DiagnosticStages::All : state.diagnosticMode == 13 ? DiagnosticStages::SwapOnly : state.diagnosticMode == 10 ? DiagnosticStages::PreparationOnly :
                                state.diagnosticMode == 11 ? DiagnosticStages::PipelineOnly : state.diagnosticMode == 12 ? DiagnosticStages::CombinedNoDraw :
                                state.diagnosticMode == 14 ? DiagnosticStages::TargetBindingsOnly : state.diagnosticMode == 15 ? DiagnosticStages::OtherBindingsOnly : DiagnosticStages::All;
            const bool splitMode = state.diagnosticMode >= 10 && state.diagnosticMode <= 12;
            ours = state.pass.Run(passContext, passInputs, diagnostic, diagnostic && ((state.diagnosticMode >= 5 && state.diagnosticMode <= 7) || state.diagnosticMode == 12),
                                  diagnostic && (state.diagnosticMode == 6 || state.diagnosticMode == 11 || splitMode), diagnostic && state.diagnosticMode == 7, stages);
        }
        if (diagnostic) DumpOmTrace(passContext);
        if (diagnostic)
            state.diagnosticSuppressedCallbacks = ext::core::diagnostic::TotalSuppressedCallbacks() - suppressedBefore;
        const uint64_t foreignDrawsAfter = ext::core::state_window::Read().count;
        state.diagnosticForeignDrawsLastRun = static_cast<uint32_t>(foreignDrawsAfter - foreignDrawsBefore);
    }
    if (ours == nullptr)
    {
        if(pairSlot) DiscardPairSlot(state,*pairSlot);
        const char* failure = state.pass.Failure();
        if (failure != nullptr && !state.loggedFailure)
        {
            state.loggedFailure = true;
            LOG_WARN("Screen-space reflections: the pass did not run ({})", failure);
        }
        Refuse(Decline::Failure);
        return nullptr;
    }

    // The capture frame's pictures of this stage: the engine's planar reflection map and the one this pass made for the water draws, copied after
    // the pass returned, on the context the pass ran on (its own guards, the observer scope and the work marker are gone).
    ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Ssr, passContext, planar.view, ours, ext::ffxiv::diag::StageWindow {});

    if(pairSlot) {
        if(pairSlot->complete && !(StagePairTexture(pairDevice.Get(),context,state,*pairSlot,"output",state.pass.OutputTexture()) &&
            StagePairTexture(pairDevice.Get(),context,state,*pairSlot,"scene",state.pass.SceneTexture()) &&
            StagePairBuffer(pairDevice.Get(),context,state,*pairSlot,"camera",state.pass.CameraBuffer()) &&
            StagePairBuffer(pairDevice.Get(),context,state,*pairSlot,"constants",state.pass.ParametersBuffer()))) {
            pairSlot->complete=false; LOG_WARN("SSR pair capture: output staging failed or exceeded 512 MiB; capture refused");
        }
        if(!pairSlot->complete) { DiscardPairSlot(state,*pairSlot); state.pairFinished=true; }
        else {
        ++state.pairRuns;
        if(state.pairRuns==2) PollPair(context,state);
        }
    }

    state.frame.Record(planar.resource, planar.view, state.pass.OutputTexture());
    state.selectedView = ours;
    if (state.diagnosticOn.load(std::memory_order_relaxed) && state.diagnosticMode == 9 && nativeRouteVerified)
    {
        state.diagnosticJPassCompleted = true;
        state.diagnosticJVerifiedDistinct = state.diagnosticJRouteChanged && ours != planar.view;
        if (state.diagnosticLoggedMode != 9)
        {
            if (state.diagnosticJRouteChanged)
                LOG_INFO("SSR diagnostic J route comparison VERIFIED: raw contexts differ and compatibility identities match; full SSR pass ran");
            else
                LOG_INFO("SSR applied; context route unchanged; route comparison NOT EXERCISED; full SSR pass ran on the same route");
        }
    }
    if (state.diagnosticOn.load(std::memory_order_relaxed))
    {
        if (state.pass.DiagnosticEngineCopyPerformedThisRun()) ++state.diagnosticCopies;
        if (state.diagnosticMode == 1) state.selectedView = planar.view;
        else if (state.diagnosticMode == 2 && state.pass.DiagnosticCopyAvailable()) state.selectedView = state.pass.DiagnosticEngineCopy();
        else if (state.diagnosticMode == 2 || state.diagnosticMode == 13 || state.diagnosticMode == 14 || state.diagnosticMode == 15) state.selectedView = planar.view;
        // Mode changes are logged at the key edge. Keep periodic health lines sparse so the
        // diagnostic does not introduce per-frame disk/logging work into the experiment.
        static std::atomic<uint32_t> overlapLogCount { 0 };
        const bool reportOverlap = state.diagnosticForeignDrawsLastRun != 0 && overlapLogCount.fetch_add(1, std::memory_order_relaxed) < 8;
        if (state.diagnosticCopies == 1 || state.diagnosticLoggedMode != state.diagnosticMode || (state.runs + 1) % 300 == 0 || reportOverlap)
        {
            const auto& ops = state.pass.LastDiagnosticOperations();
            const auto windowStats = ext::core::state_window::Read();
            BOOL multithreadProtected = FALSE;
            ComPtr<ID3D11Multithread> multithread;
            const bool haveMultithread = SUCCEEDED(passContext->QueryInterface(IID_PPV_ARGS(&multithread)));
            if (haveMultithread) multithreadProtected = multithread->GetMultithreadProtected();
            LOG_INFO("SSR diagnostic mode {} run {}; K-P preserve engine reflection; last ops copy/clear/map/stateSwap/draw/timingBegin/timingEnd/GetData/sceneOM/pipelineBinds={}/{}/{}/{}/{}/{}/{}/{}/{}/{}, trackedOM before/after {},{} (unknown=-1), nativeTargetSame {}, native-window overlaps total/this pass {}/{}, passOwner {}, lastOverlapOwner/caller {}/{}, lastOverlapDraw {}, multithreadProtected {}",
                     state.diagnosticMode, state.runs + 1, ops.copies, ops.clears, ops.maps, ops.stateSwaps, ops.fullscreenDraws, ops.timingBegins, ops.timingEnds, ops.timingGetData, ops.sceneOmSets, ops.pipelineBinds, ops.trackedOmBefore, ops.trackedOmAfter, ops.nativeTargetRestored ? "yes" : "no",
                     windowStats.count, state.diagnosticForeignDrawsLastRun, GetCurrentThreadId(), windowStats.owner, windowStats.caller, windowStats.kind,
                     haveMultithread ? (multithreadProtected ? "yes" : "no") : "unavailable");
            state.diagnosticLoggedMode = state.diagnosticMode;
        }
    }
    state.runs++;
    state.lastFacts = facts;
    state.lastParams = state.params;
    state.haveRun = true;
    state.session.NoteRun();
    // The run line waits for the first timestamp pair to land, so the one line a session gets carries the GPU cost it asks for; a pass whose device
    // never measured writes the "not measured yet" clause instead of staying silent forever. It is the tuning of the run it reports - the sliders a
    // later run was drawn with can have moved by the time the pair has landed.
    if (RunLineDue(state.loggedRun, state.pass.HasGpuMilliseconds(), state.pass.CanMeasureGpuTime()))
    {
        state.loggedRun = true;
        LOG_INFO("{}", FormatRun(state.lastParams, facts.planarWidth, facts.planarHeight, state.pass.HasGpuMilliseconds(),
                                 state.pass.GpuMilliseconds()));
    }
    return ours;
}
} // namespace

void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    if (!Mentions(code, size, kReflectionName))
        return; // most of the game's shaders, on a byte scan alone: only a sampler of the planar map is read and stamped
    try
    {
        detail::StampShader(shader, PackSlots(SlotsOfShader(code, size)));
    }
    catch (...)
    {
    }
}

BoundReader SwapReader(ID3D11DeviceContext* context, Anchors anchors)
{
    BoundReader none;
    if (context == nullptr || !IsReflectionAnchors(anchors))
    {
        NotePassedOn(PassedOn::NotReflection);
        return none;
    }
    State& state = S();
    if (!state.armed.load(std::memory_order_relaxed) || !state.session.Armed())
    {
        NotePassedOn(PassedOn::NotArmed);
        return none;
    }
    if (state.inPass.load(std::memory_order_relaxed))
    {
        NotePassedOn(PassedOn::OwnWork);
        return none; // this module's own work: never classified, never swapped
    }
    // Another module's replay of a game draw (the character preview, a diagnostic replay) is not a draw of the frame: it keeps the engine's own
    // texels and never spends the frame's one attempt, so a preview opened beside the reflections shows the engine's picture and the frame's own
    // water draw still runs the pass. The gate below asks the same fact again, so the decision is also written where a test can pin it.
    if (ext::ffxiv::chara::t_replay)
    {
        NotePassedOn(PassedOn::Replay);
        return none;
    }

    // Everything below talks to the device and reads the game's own objects. A throw is then a counted refusal, not an exception through the
    // game's draw hook: the draw runs unchanged with the engine's texture, which is what failing open means here.
    try
    {
        // The frame token is the output-frame counter both present paths tick (ext/present/PresentSeams.h), one relaxed load; a new frame forgets
        // everything the last one recorded, so a texture or a view the game rebuilt cannot answer for a draw of the next frame.
        const uint64_t token = ext::present::FrameCount();
        if (state.diagnosticOn.load(std::memory_order_relaxed) && state.diagnosticKeyFrame != token)
        {
            state.diagnosticKeyFrame = token;
            const bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            if (down && !state.diagnosticF7Down)
            {
                const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                if (control)
                    state.diagnosticMode = state.diagnosticMode == 11 ? 13 : state.diagnosticMode == 13 ? 14 : state.diagnosticMode == 14 ? 15 : state.diagnosticMode == 15 ? 8 : state.diagnosticMode == 8 ? 11 : 11;
                else if (shift)
                    state.diagnosticMode = state.diagnosticMode == 0 ? 9 : 0;
                else
                    state.diagnosticMode = (state.diagnosticMode + 1) % 16;
                state.frame.Reset(); // make the selected mode apply to this frame's first reader
                static constexpr const char* names[] = { "A SSR result", "B original engine SRV with setter", "C private exact engine-map copy", "D SSR pass, no native SRV setter", "E D with injected-work observers suppressed", "F D without fullscreen SSR draw", "G F without timing queries", "H F with scene copy while bound", "I bypass SSR pass", "J native-context compatibility run", "K preparation only", "L pipeline only", "M combined without draw", "N state swap only", "O target bindings only", "P other bindings only" };
                LOG_INFO("SSR diagnostic mode {}: {}", state.diagnosticMode, names[state.diagnosticMode]);
            }
            state.diagnosticF7Down = down;
        }
        if (state.frame.hasFrame && state.frame.frame != token)
            state.servedLastFrame = state.frame.served; // the frame that just ended, before Begin() clears it
        state.frame.Begin(token);
        // One line a session: how many water draws a frame served. A session compares it with the number of reflection-map bindings it sees,
        // because a reader whose shader's reflection was stripped is never served and the two numbers then differ.
        if (!state.loggedServed && state.servedLastFrame > 0)
        {
            state.loggedServed = true;
            LOG_INFO("{}", FormatServed(state.servedLastFrame));
        }

        // The shader the engine bound for this draw: the one whose own stamp says where its planar map, its view position and its camera table
        // are. Its own stamp answers, so it does not matter how many shaders the game created after it; a shader this module never read (the key
        // was on before the game created it) is a refusal at the frame's first water draw, not a pass with guessed slots.
        ReaderSlots slots;
        ComPtr<ID3D11PixelShader> pixelShader;
        {
            context->PSGetShader(&pixelShader, nullptr, nullptr);
            slots = UnpackSlots(detail::ReadStamp(pixelShader.Get()));
        }
        if (!slots.HasReflection())
        {
            NotePassedOn(PassedOn::NoStamp, pixelShader.Get());
            return none; // a shader whose stamp never landed, or one that only declares the map: another draw of the frame, not a refusal
        }
        if (!slots.water)
        {
            NotePassedOn(PassedOn::NotWater, pixelShader.Get());
            return none; // a sampler of the map that is not a water surface: this pass does not answer for its draws
        }

        ViewRef bound;
        context->PSGetShaderResources(slots.reflection, 1, &bound.view);
        const BoundTexture planar = Describe(bound.view);
        if (planar.view == nullptr)
        {
            NotePassedOn(PassedOn::NoMap, pixelShader.Get());
            return none; // nothing bound under that name: another draw of the frame, not a refusal
        }
        if (!planar.twoD || !IsPlanarTarget(planar.format, planar.width, planar.height))
        {
            // The stamp and the view it pointed at, so the next capture says which slot this shader's map is at instead of leaving it to be
            // inferred: the diagnosis of one session could not tell the sampler's register from the texture's without it.
            ReadBinding read;
            read.have = true;
            read.stamp = PackSlots(slots);
            read.slot = slots.reflection;
            read.view = planar.viewDimension;
            read.array = planar.arraySize;
            read.format = planar.format;
            read.width = planar.width;
            read.height = planar.height;
            read.texture = uint64_t(reinterpret_cast<uintptr_t>(planar.resource));
            NotePassedOn(PassedOn::OtherMap, pixelShader.Get(), &read);
            return none; // not the engine's planar map: another draw of the frame, not a refusal
        }

        // The frame's answer for this draw (SsrRules.h's ServeAnswer): a replay of a game draw by another module is passed on before either branch,
        // so it is never served this frame's texture and never spends the frame's one attempt.
        const Serve answer = ServeAnswer(state.frame, ext::ffxiv::chara::t_replay);
        if (answer == Serve::PassOn)
        {
            NotePassedOn(PassedOn::AttemptSpent);
            return none; // a replay, or this frame's one attempt is spent
        }
        if (answer == Serve::Attempt)
        {
            state.frame.TakeAttempt();
            ID3D11ShaderResourceView* result = RunFrame(context, state, slots, planar);
            if (result == nullptr)
                return none;
            if (state.diagnosticOn.load(std::memory_order_relaxed) && state.diagnosticMode == 8)
                return none;
            if (state.diagnosticOn.load(std::memory_order_relaxed) && state.diagnosticMode == 9 && !state.diagnosticJCompatible)
                return none;
        }
        // The cheap test first: the engine binds one view object for the whole frame, so a different view is a different binding. A second view
        // of the same resource (a game that makes one per pass) is served too, which is why the resource is compared as well.
        const bool sameBinding = planar.view == state.frame.engineView || state.frame.Has(planar.resource);
        if (!sameBinding)
        {
            NotePassedOn(PassedOn::OtherBinding);
            return none;
        }

        ID3D11ShaderResourceView* ours = state.selectedView.Get();
        if (ours == nullptr)
        {
            NotePassedOn(PassedOn::NoOutput);
            return none;
        }
        // D/E/F/G/H leave the native reflection binding untouched. J ran the pass
        // on the verified native immediate context and intentionally swaps on the incoming context.
        if (state.diagnosticOn.load(std::memory_order_relaxed) && ((state.diagnosticMode >= 3 && state.diagnosticMode <= 8) || (state.diagnosticMode >= 10 && state.diagnosticMode <= 15)))
            return none;
        // The swap asks the slot again and hands back the game's view with the reference the guard needs to restore it; the description's own
        // reference goes back here, so exactly one is held for the length of the draw.
        ID3D11ShaderResourceView* replaced = detail::SwapSlot(context, slots.reflection, ours);
        if (replaced == nullptr)
        {
            NotePassedOn(PassedOn::NoSlot);
            return none;
        }
        bound.Reset();
        state.frame.Serve();
        state.readers++;
        return BoundReader(context, slots.reflection, replaced);
    }
    catch (...)
    {
        Refuse(Decline::Exception);
        return none;
    }
}

bool Armed()
{
    State& state = S();
    return state.armed.load(std::memory_order_relaxed) && state.session.Armed();
}

Params CurrentParams() { return S().params; }

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.key = state.keyOn.load(std::memory_order_relaxed);
    status.identity = IsIdentity(state.params);
    status.armed = CardArmed(status.key, state.params, state.session.Armed());
    status.runs = state.runs;
    status.readers = state.readers;
    status.declined = state.declined;
    status.servedLastFrame = state.servedLastFrame;
    for (uint32_t i = 0; i < kPassedOnCount; ++i)
        status.passedOn[i] = state.passedOn[i];
    status.haveGpuMilliseconds = state.pass.HasGpuMilliseconds();
    status.gpuMilliseconds = state.pass.GpuMilliseconds();
    status.diagnostic = state.diagnosticOn.load(std::memory_order_relaxed);
    status.diagnosticCopyAvailable = state.pass.DiagnosticCopyAvailable();
    status.diagnosticMode = state.diagnosticMode;
    status.diagnosticJVerifiedDistinct = state.diagnosticJVerifiedDistinct;
    status.diagnosticJRouteChanged = state.diagnosticJRouteChanged;
    status.diagnosticJCompatible = state.diagnosticJCompatible;
    status.diagnosticJPassCompleted = state.diagnosticJPassCompleted;
    status.diagnosticOperations = state.pass.LastDiagnosticOperations();
    status.diagnosticForeignDraws = ext::core::state_window::Read().count;
    status.diagnosticForeignDrawsLastRun = state.diagnosticForeignDrawsLastRun;
    const auto windowStats = ext::core::state_window::Read();
    status.diagnosticWindowOwner = windowStats.owner;
    status.diagnosticWindowCaller = windowStats.caller;
    status.diagnosticPipelineBinds = status.diagnosticOperations.pipelineBinds;
    status.diagnosticCopies = state.diagnosticCopies;
    status.diagnosticBypasses = state.diagnosticBypasses;
    status.diagnosticDraws = state.pass.ActualDrawSubmissions();
    status.diagnosticTimingBegins = state.pass.GpuTimingBegins();
    status.diagnosticTimingEnds = state.pass.GpuTimingEnds();
    status.diagnosticTimingGetDataCalls = state.pass.GpuTimingGetDataCalls();
    status.diagnosticUavMaskBeforeCopy = state.pass.DiagnosticUavMaskBeforeCopy();
    status.diagnosticUavMaskAfterCopy = state.pass.DiagnosticUavMaskAfterCopy();
    status.diagnosticUavLostMask = uint8_t(status.diagnosticUavMaskBeforeCopy & ~status.diagnosticUavMaskAfterCopy);
    status.diagnosticPredicatePresent = state.pass.DiagnosticPredicatePresent();
    status.diagnosticSuppressedCallbacks = state.diagnosticSuppressedCallbacks;
    status.diagnosticSuppressionActiveDuringPass = state.diagnosticSuppressionActiveDuringPass;
    return status;
}

Diagnostics CurrentDiagnostics()
{
    State& state = S();
    const Status status = CurrentStatus();
    Diagnostics diagnostics;
    diagnostics.key = status.key;
    diagnostics.identity = status.identity;
    diagnostics.armed = status.armed;
    diagnostics.runs = status.runs;
    diagnostics.readers = status.readers;
    diagnostics.declined = status.declined;
    diagnostics.servedLastFrame = status.servedLastFrame;
    for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
        diagnostics.declines[i] = state.declines[i];
    for (uint32_t i = 0; i < kPassedOnCount; ++i)
        diagnostics.passedOn[i] = state.passedOn[i];
    diagnostics.read = state.read;
    diagnostics.haveRun = state.haveRun;
    if (diagnostics.haveRun)
    {
        diagnostics.sourceWidth = state.lastFacts.sceneWidth;
        diagnostics.sourceHeight = state.lastFacts.sceneHeight;
        diagnostics.targetWidth = state.lastFacts.planarWidth;
        diagnostics.targetHeight = state.lastFacts.planarHeight;
        for (size_t i = 0; i < 4; ++i)
            diagnostics.window[i] = state.lastFacts.window[i];
    }
    diagnostics.haveGpuMilliseconds = state.pass.HasGpuMilliseconds();
    diagnostics.gpuMilliseconds = state.pass.GpuMilliseconds();
    return diagnostics;
}

FrameObservation CurrentFrameObservation()
{
    State& state = S();
    FrameObservation out;
    out.hasToken = state.frame.hasFrame;
    out.token = state.frame.frame;
    out.tried = state.frame.tried;
    out.valid = state.frame.valid;
    out.served = state.frame.served;
    out.runs = state.runs;
    out.declined = state.declined;
    return out;
}

void Publish(bool on)
{
    State& state = S();
    state.keyOn.store(on, std::memory_order_relaxed);
    Params params;
    params.on = on;
    params.steps = ClampSteps(float(ext::Cfg().ffxiv.screenSpaceReflectionsSteps.value_or_default()));
    params.thickness = ClampThickness(ext::Cfg().ffxiv.screenSpaceReflectionsThickness.value_or_default());
    params.blend = ClampBlend(ext::Cfg().ffxiv.screenSpaceReflectionsBlend.value_or_default());
    params.fade = ClampFade(ext::Cfg().ffxiv.screenSpaceReflectionsFade.value_or_default());
    state.params = params;
    // The frame's texture belongs to the tuning it was computed with, and an off key (or an identity one) gives the device objects back.
    state.frame.Reset();
    if (!on) state.selectedView.Reset();
    // One line per change of the card's own arming, blend included: whether the reflections were armed at the water is the one fact a session
    // could not read anywhere before, and the first publish writes the state the session starts with.
    const bool armedNow = Wants(params);
    if (!armedNow) water_trace::RequestDisarm();
    if (!state.haveArmed || state.armed.load(std::memory_order_relaxed) != armedNow)
    {
        state.haveArmed = true;
        LOG_INFO("{}", FormatPublish(armedNow, params.blend));
    }
    state.armed.store(armedNow, std::memory_order_relaxed);
    state.loggedRun = false;
    state.loggedFailure = false;
    state.loggedServed = false;
    state.servedLastFrame = 0;
    if (!on)
    {
        state.session = Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.pass.Reset();
    }
    else if (state.session.off)
    {
        state.session = Session {}; // turning the switch back on after a switch-off is the player's re-arm
    }
    // The draw path's own arming is the shared module's: it asks this module whether it wants a draw, so the shared switch and this one arm the
    // same seam whichever of them moved.
    engine::Publish(ext::Cfg().ffxiv.enginePassOverrides.value_or_default());
}

void PublishDiagnostic(bool on)
{
    State& state = S();
    if (!on) water_trace::RequestDisarm();
    state.diagnosticOn.store(on, std::memory_order_relaxed);
    ext::core::om_trace::enabled.store(on, std::memory_order_relaxed);
    ext::core::state_window::enabled.store(on, std::memory_order_relaxed);
    state.diagnosticMode = 0;
    state.diagnosticCopies = 0;
    state.diagnosticBypasses = 0;
    state.diagnosticLoggedMode = UINT32_MAX;
    state.diagnosticSuppressedCallbacks = 0;
    state.diagnosticSuppressionActiveDuringPass = false;
    state.diagnosticKeyFrame = UINT64_MAX;
    state.diagnosticF7Down = false;
    state.frame.Reset();
    state.selectedView.Reset();
    if (!on) state.pass.ResetDiagnosticCopy();
    LOG_INFO("SSR diagnostic {} (F7 cycles A-P; Ctrl+F7 cycles L,N,O,P,I; Shift+F7 toggles A/J)", on ? "enabled" : "disabled");
}

void SetDiagnosticModeForTest(uint32_t mode)
{
    S().diagnosticMode = mode % 16;
    S().frame.Reset();
}

void Release()
{
    State& state = S();
    water_trace::RequestDisarm();
    state.frame.Reset();
    state.selectedView.Reset();
    state.servedLastFrame = 0;
    state.pass.Reset();
    ResetPair(state); state.pairArmed=false; state.pairFinished=false;
}

void OnDiagnosticCaptureStart()
{
    State& state = S();
    if (ext::Cfg().ffxiv.drawDiagnosticsDump.value_or_default() != "ssr-pair") return;
    if (state.pairArmed && !state.pairFinished)
        LOG_WARN("SSR pair capture: new capture superseded the pending pair");
    ResetPair(state);
    state.pairArmed = true;
    state.pairFinished = false;
    LOG_INFO("SSR pair capture: armed for two adjacent successful module runs");
}

void OnRenderCallback(ID3D11DeviceContext* context)
{
    State& state=S();
    if(state.pairRuns==1 && pair::TimedOut(state.pair[0].token,ext::present::FrameCount())) {
        LOG_WARN("SSR pair capture: timed out before the adjacent successful module execution");
        ResetPair(state);
    }
    PollPair(context,state);
}
} // namespace ext::ffxiv::engine::ssr

namespace ext::ffxiv::engine::ssr
{
// The stage dump's question about this stage's values, asked at the capture frame (the armed question is Armed()).
std::string StageValues()
{
    const Params& params = S().params;
    return std::format("steps={} thickness={:.2f} blend={:.2f} fade={:.2f}", params.steps, double(params.thickness), double(params.blend),
                       double(params.fade));
}
} // namespace ext::ffxiv::engine::ssr

namespace
{
// The registration of the config hook, set when this DLL's static initialisers run.
struct RegisterScreenSpaceReflections
{
    RegisterScreenSpaceReflections()
    {
        ext::FfxivConfig::screenSpaceReflectionsOnLoaded = &ext::ffxiv::engine::ssr::Publish;
        ext::FfxivConfig::screenSpaceReflectionsDiagnosticOnLoaded = &ext::ffxiv::engine::ssr::PublishDiagnostic;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Ssr, &ext::ffxiv::engine::ssr::Armed, &ext::ffxiv::engine::ssr::StageValues);
    }
} g_registerScreenSpaceReflections;
} // namespace
