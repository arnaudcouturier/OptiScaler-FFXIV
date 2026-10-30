#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/PathText.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaNr.h"
#include "ext/ffxiv/chara/CharaWindowSet.h"
#include "ext/nr/runtime/NrPreviewRuntime.h"
#include "ext/ffxiv/chara/CharaNrRecipe.h"
#include "ext/gpu/Gpu.h"
#include "ext/nr/DlssNr.h"
#include "ext/nr/NrFocus.h"
#include "ext/nr/native/DlssNrNative.h"
#include "ext/nr/pass/DlssNr_Dx12.h"
#include "ext/present/PresentSeams.h"

#include <State.h>
#include <gpu_time/GpuTime_Dx12.h>
#include <with_dx12/dx11_with_dx12.h>
#include <with_dx12/with_dx12.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>

// Neural Rendering for the character previews (design: phase3-ledger/CHARA-NR-DESIGN.md, option A2). See CharaNr.h.
//
// Everything is guarded by one mutex (draw thread, menu thread, bridge shutdown). The rules (CharaNrRules.h) and the GPU recipe
// (CharaNrRecipe.h) are pure and tested; this file is the glue to Direct3D, the bridge and the model forwarder.
namespace ext::ffxiv::chara::nr
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr DXGI_FORMAT kResultFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kScratchFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

double MsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// An image owned by both devices: created on the game's D3D11 device with an NT shared handle, opened on the bridge's D3D12 device.
struct Shared
{
    Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C res {};
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;

    ID3D12Resource* r12() const { return res.Dx12Resource; }
    ID3D11Texture2D* t11() const { return res.SharedTexture; }
    void Release()
    {
        srv.Reset();
        rtv.Reset();
        Dx11WithDx12::ReleaseSharedTexture(&res);
    }
};

// One stream per preview image, in one of two modes (CharaNrRules.h, StreamShape): bring-up (the native image and synthetic guides, native
// size) or production (the super-resolution output at twice the render size, copies of the preview's depth and motion at the render size).
// The mode is part of the shape the stream was built for; a change of mode rebuilds the stream after the settle time. Everything else is
// shared between the modes: the fences, the command lists and allocators, the compute pass recorder, the model creation and its linger,
// the stream table and its release rules.
struct Stream
{
    uint32_t width = 0, height = 0;       // the colour image and the model's input size
    uint32_t guideWidth = 0, guideHeight = 0; // the depth and motion images (production: the render size, bring-up: same as the image)
    bool production = false;
    bool preSr = false;           // NR before SR (production is false then); the stream's placement is PlacementFrom(production, preSr)
    bool late = false;            // the hand-off variant, decided when the stream is created (a config change rebuilds the stream); never in production or pre-SR
    Shared colour[2];             // the late variant uses both (the run before may still read one while the copy writes the other)
    Shared result[2], depth, motion;
    uint64_t serial[2] = {};      // identifies the result in result[i] (a module-wide counter, never reused): the debug tint is made once per serial
    ComPtr<ID3D12Resource> proxy, original, modelOut;
    void* feature = nullptr;
    bool featureReady = false;
    uint64_t createDone = 0;      // completion value of the list that created the feature
    uint64_t doneValue[2] = {};   // done-fence value after which result[i] (and colour[i] in the late variant) is no longer in use
    bool guidesInit = false;
    bool gpuObjects = false;
    uint64_t vramBefore = 0;      // video memory in use before the stream's images and model were created
    uint64_t vramAddedMb = 0;     // this stream's part of the module's account (VideoMemoryAccount), given back when its objects are released
    std::chrono::steady_clock::time_point created {};
    bool readyLogged = false;
};

struct Module
{
    std::recursive_mutex mutex;
    Latches latches;
    bool inited = false;
    bool holdOff = false; // the bridge announced its shutdown; cleared when it initialises again

    ComPtr<ID3D11Device> dev11;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ID3D12Device* dev12 = nullptr;       // the bridge's objects, not owned
    ID3D12CommandQueue* queue = nullptr;
    // Two fences, each signalled from one side only (CharaNrRules.h, "The fence hand-off"):
    ComPtr<ID3D11Fence> readyFence11;    // ready: created on D3D11, only the D3D11 context signals it ...
    ComPtr<ID3D12Fence> readyFence12;    // ... opened on D3D12, where only the queue waits on it
    ComPtr<ID3D12Fence> doneFence12;     // done: created on D3D12, only the queue signals it; its values are also the completion values
    ComPtr<ID3D11Fence> doneFence11;     // ... opened on D3D11, where only the context waits on it
    FenceCounter ready;                  // values of the ready fence
    FenceCounter done;                   // values of the done fence (the completion counter: allocators, releases, creation)
    bool doneBroken = false;             // a done signal failed after its list ran: the next drain repairs the fence first
    bool tableFullLogged = false;        // the one line about a full stream table
    bool setSkipLogged = false;          // the one line about a preview window outside the processed set
    std::atomic<bool> releasePending { false }; // the old objects are kept and released in Maintain: a device change found the GPU busy, or a teardown threw (set from the handler of a teardown entry, so atomic)

    DlssNr::PreviewBackend backend;
    // The NR runtime revision (the model file the card selected) that each stream's model instance was built from. A stream whose
    // revision is not the backend's is rebuilt from the new model path once the GPU is finished with it (see SyncRuntime, RunCoreSteps).
    uint64_t streamRuntime[kMaxStreams] = {};
    // A failure the MODEL caused (it would not initialise, or the run failed in the model's recording step). Unlike the session latch of
    // `latches` it ends when another NR runtime is selected (SyncRuntime), so a bad model file does not switch previews off until restart.
    std::atomic<bool> modelLatched { false };
    uint64_t latchedRuntime = 0; // the NR runtime revision that was current when the model latch was set
    uint64_t probedRuntime = 0; // the NR runtime revision the model probe was made for (a later revision probes again)
    std::unique_ptr<DlssNr_Dx12> recorder;
    std::unique_ptr<GpuTime_Dx12> timer;
    TimerRing timerRing; // mirrors the query slots of `timer` (CharaNrRules.h): which list wrote each, its done value and mode
    ComPtr<ID3D12CommandAllocator> allocators[kAllocators];
    uint64_t allocatorValue[kAllocators] = {};
    size_t allocatorNext = 0;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<IDXGIAdapter3> adapter;

    // probes, cached for the life of the bridge's device
    ID3D12Device* probedDevice = nullptr;
    Probe nvidia = Probe::Unknown, forwarder = Probe::Unknown, model = Probe::Unknown;

    StreamTable table;
    Stream streams[kMaxStreams];
    StateLog stateLog;
    CostAccumulator gpuCost, cpuCost;
    CostAccumulator productionGpuCost; // GPU time of the production runs alone, from the fenced per-slot read
    CostAccumulator preSrGpuCost;      // ... and of the pre-SR runs alone
    Availability availability = Availability::ConfigOff;
    std::atomic<uint8_t> availabilitySnapshot { static_cast<uint8_t>(Availability::Available) }; // readable without the mutex
    uint64_t lastMaintain = 0;

    std::atomic<bool> holds { false };
    std::atomic<uint64_t> runs { 0 };                                      // both modes
    FallbackTally fallbacks;                                               // both modes; `fallbacks.production` is the production entry alone
    std::atomic<uint64_t> productionRuns { 0 };                            // the production entry alone (a subset of `runs`)
    std::atomic<uint64_t> preSrRuns { 0 };                                 // the pre-SR entry alone (a subset of `runs`)
    std::atomic<uint64_t> serialCounter { 0 };
    std::atomic<uint32_t> streamCount { 0 }, liveCount { 0 };
    std::atomic<uint64_t> lastGpuUs { 0 }, lastCpuUs { 0 };
    std::atomic<uint64_t> lastProductionGpuUs { 0 }, lastProductionCpuUs { 0 };
    std::atomic<uint64_t> lastPreSrGpuUs { 0 }, lastPreSrCpuUs { 0 };
    VideoMemoryAccount vram; // the figure of the menu: what the streams' images and models added
    // Why a call was skipped, split by cause (guarded by the mutex), and the most streams that existed at once: the log line of the
    // preview activity reads them through Status.
    uint64_t skipTableFull = 0;
    uint64_t skipSettling = 0;
    uint64_t skipNoList = 0;
    uint32_t streamsPeak = 0;
};

Module& M()
{
    static Module& m = *new Module; // never destroyed: the hooks outlive static destruction
    return m;
}

void Latch(const char* why)
{
    if (M().latches.LatchNr())
        LOG_WARN("CharaView NR: {}; switched off for this session, native previews retained", why);
}

// The latch for a failure the selected model file caused: previews fall back to the native image until another runtime is selected.
void LatchModel(const char* why)
{
    if (!M().modelLatched.exchange(true))
    {
        M().latchedRuntime = DlssNr::PreviewRuntimeRevision();
        LOG_WARN("CharaView NR: {}; switched off until another NR runtime is selected, native previews retained", why);
    }
}

bool Late() { return ext::Cfg().ffxiv.chara.previewNrLate.value_or_default(); }

GateFacts Facts()
{
    Module& m = M();
    GateFacts f;
    f.configOn = Wanted();
    f.latched = m.latches.NrLatched() || m.modelLatched;
    f.bridgeUp = WithDx12::IsInited() && Dx11WithDx12::GetD3D12Device() != nullptr && Dx11WithDx12::GetD3D12CommandQueue() != nullptr;
    f.bridgeSuspended = m.holdOff || DlssNr::PreviewBackendSuspended();
    f.releasePending = m.releasePending;
    f.nvidia = m.nvidia;
    f.forwarder = m.forwarder;
    f.model = m.model;
    return f;
}

Availability Refresh()
{
    Module& m = M();
    const Availability a = Evaluate(Facts());
    m.availability = a;
    m.availabilitySnapshot.store(static_cast<uint8_t>(a), std::memory_order_relaxed);
    if (m.stateLog.ShouldLog(a))
    {
        if (a == Availability::Available)
            LOG_INFO("CharaView NR: available");
        else
            LOG_INFO("CharaView NR: unavailable, {}", ReasonText(a));
    }
    return a;
}

uint64_t VramUsedBytes()
{
    Module& m = M();
    if (m.adapter == nullptr)
        return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info {};
    return SUCCEEDED(m.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ? info.CurrentUsage : 0;
}

// ---- Fences and lists -------------------------------------------------------------------------------------------------------
bool GpuDone(uint64_t value)
{
    Module& m = M();
    return value == 0 || m.doneFence12 == nullptr || m.doneFence12->GetCompletedValue() >= value;
}

// Waits on the CPU for completion value `value` (shutdown paths only). False: not proven complete.
bool WaitCompletion(uint64_t value, DWORD timeoutMs)
{
    Module& m = M();
    if (GpuDone(value))
        return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
        return false;
    bool done = false;
    if (SUCCEEDED(m.doneFence12->SetEventOnCompletion(value, event)))
        done = WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done;
}

// A done signal failed behind a list that ran: that value is never reached, so nothing waiting for it (allocator reuse, the drain at
// release) would ever be satisfied. A newer value signalled behind everything queued before it (queue order) catches the fence up.
// On a removed device the completed value is the maximum anyway and this changes nothing.
void RepairDone()
{
    Module& m = M();
    if (!m.doneBroken || m.queue == nullptr || m.doneFence12 == nullptr)
        return;
    // The queue is a non-owning pointer into the bridge. After a device change or a bridge restart it may be the old one (gone): never
    // signal through it. The old objects are then released once the done fence reports them complete, which Maintain retries.
    const bool queueUsable = !m.releasePending && m.queue == Dx11WithDx12::GetD3D12CommandQueue();
    RepairDoneStep(m.done, m.doneBroken, queueUsable, [&](uint64_t value) { return SUCCEEDED(m.queue->Signal(m.doneFence12.Get(), value)); });
}

// Resets the shared list on an allocator whose earlier work finished. False: none is free (not an error).
bool BeginList()
{
    Module& m = M();
    std::array<size_t, kAllocators> order {};
    const size_t free = FreeAllocators(m.allocatorValue, m.allocatorNext, [](uint64_t value) { return GpuDone(value); }, order);
    for (size_t i = 0; i < free; ++i)
    {
        const size_t index = order[i];
        if (FAILED(m.allocators[index]->Reset()))
            continue;
        if (FAILED(ext::gpu::ResetCommands(m.list.Get(), m.allocators[index].Get())))
            continue;
        m.allocatorNext = (index + 1) % kAllocators;
        m.allocatorValue[index] = m.done.last + 1; // the value the done signal of this list takes
        return true;
    }
    return false;
}

// Closes and executes the list. False: the list could not be closed and nothing ran.
bool ExecuteList()
{
    Module& m = M();
    if (FAILED(m.list->Close()))
        return false;
    ID3D12CommandList* lists[] = { m.list.Get() };
    ext::gpu::ExecuteCommands(m.queue, 1, lists);
    return true;
}

// Queues the done signal behind the executed list. `value` is set before the signal is queued, so a caller that tracks the list
// (release waits for it) has the value even when the signal itself failed.
bool SignalDone(uint64_t& value)
{
    Module& m = M();
    return SignalDoneStep(m.done, m.doneBroken, value, [&](uint64_t v) { return SUCCEEDED(m.queue->Signal(m.doneFence12.Get(), v)); });
}

// A list without a hand-off (the model creation): executes it and signals the done fence. `value` is non-zero as soon as the list ran.
bool SubmitList(uint64_t& value)
{
    value = 0;
    return ExecuteList() && SignalDone(value);
}

// ---- Images ----------------------------------------------------------------------------------------------------------------
bool CreateShared(const char* name, uint32_t w, uint32_t h, DXGI_FORMAT format, UINT bind, bool withSrv, bool withRtv, Shared& out)
{
    Module& m = M();
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(m.dev11->CreateTexture2D(&td, nullptr, &texture)))
        return false;
    ComPtr<IDXGIResource1> dxgi;
    HANDLE handle = nullptr;
    if (FAILED(texture.As(&dxgi)) ||
        FAILED(dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)) ||
        handle == nullptr)
        return false;
    out.res.SharedTexture = texture.Detach();
    out.res.OwnsSharedTexture = true;
    out.res.UsesNTHandle = true;
    out.res.Dx11Handle = handle;
    if (!Dx11WithDx12::OpenHandle(name, m.dev12, out.res.SharedTexture, &out.res))
        return false;
    if (withSrv && FAILED(m.dev11->CreateShaderResourceView(out.t11(), nullptr, &out.srv)))
        return false;
    if (withRtv && FAILED(m.dev11->CreateRenderTargetView(out.t11(), nullptr, &out.rtv)))
        return false;
    return true;
}

bool CreatePrivate(uint32_t w, uint32_t h, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kScratchFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(M().dev12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        nullptr, IID_PPV_ARGS(&out)));
}

// The objects of a stream for `shape`. Bring-up: the colour image has the format of the native image (`nativeFormat`) and the guides have the
// image's size. Production: the colour image is the format of the super-resolution output (RGBA16F, filled by a copy), the guides have the
// render size; the late variant does not exist (the alpha pass needs this render's colour), so there is one colour image. Pre-SR: the colour
// image is the format of the render-size colour the SR path evaluates (RGBA8, filled by a copy), the guides have the same size as the colour.
bool CreateStreamGpu(Stream& s, const StreamShape& shape, DXGI_FORMAT nativeFormat)
{
    const Placement placement = PlacementOf(shape);
    const uint32_t w = shape.width, h = shape.height;
    const uint32_t gw = CarriesGuides(placement) ? shape.guideWidth : w, gh = CarriesGuides(placement) ? shape.guideHeight : h;
    const bool late = LateVariantInUse(shape);
    const DXGI_FORMAT colourFormat = static_cast<DXGI_FORMAT>(ColourFormatFor(placement, static_cast<uint32_t>(nativeFormat)));
    s.width = w;
    s.height = h;
    s.guideWidth = gw;
    s.guideHeight = gh;
    s.production = shape.production;
    s.preSr = shape.preSr;
    s.late = late; // decided here, for the life of the stream
    if (!CreateShared("CharaNr colour", w, h, colourFormat, D3D11_BIND_SHADER_RESOURCE, false, false, s.colour[0]) ||
        (late && !CreateShared("CharaNr colour 1", w, h, colourFormat, D3D11_BIND_SHADER_RESOURCE, false, false, s.colour[1])) ||
        !CreateShared("CharaNr result 0", w, h, kResultFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, true, false, s.result[0]) ||
        !CreateShared("CharaNr result 1", w, h, kResultFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, true, false, s.result[1]) ||
        !CreateShared("CharaNr depth", gw, gh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, false, true, s.depth) ||
        !CreateShared("CharaNr motion", gw, gh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, false, false, s.motion) ||
        !CreatePrivate(w, h, s.proxy) || !CreatePrivate(w, h, s.original) || !CreatePrivate(w, h, s.modelOut))
        return false;
    s.gpuObjects = true;
    return true;
}

// Frees a stream's objects. The caller has proven that the GPU finished with them (or is shutting down after a drain).
void ReleaseStreamObjects(Stream& s, bool releaseFeature)
{
    Module& m = M();
    if (s.feature != nullptr && releaseFeature && m.backend.release != nullptr)
    {
        // The main pass destroys and evaluates its model under the same lock: a release from another thread (the bridge shutdown) must
        // not run in the middle of its evaluate.
        DlssNr::PreviewEvaluateScope serialised;
        m.backend.release(s.feature);
    }
    s.feature = nullptr;
    s.colour[0].Release();
    s.colour[1].Release();
    s.result[0].Release();
    s.result[1].Release();
    s.depth.Release();
    s.motion.Release();
    s.proxy.Reset();
    s.original.Reset();
    s.modelOut.Reset();
    m.vram.Released(s.vramAddedMb); // every way a stream's objects go (linger, rebuild, mode switch, full release) gives its figure back
    s = Stream {};
}

void UpdateHolds()
{
    Module& m = M();
    m.holds.store(m.inited || m.table.UsedCount() != 0, std::memory_order_relaxed);
    const size_t used = m.table.UsedCount();
    m.streamCount.store(static_cast<uint32_t>(used), std::memory_order_relaxed);
    if (used > m.streamsPeak)
        m.streamsPeak = static_cast<uint32_t>(used);
}

void ReleaseGlobal()
{
    Module& m = M();
    m.recorder.reset();
    m.timer.reset();
    m.timerRing = TimerRing {};
    m.list.Reset();
    for (auto& a : m.allocators)
        a.Reset();
    for (auto& v : m.allocatorValue)
        v = 0;
    m.readyFence12.Reset();
    m.readyFence11.Reset();
    m.doneFence12.Reset();
    m.doneFence11.Reset();
    m.ctx4.Reset();
    m.dev5.Reset();
    m.dev11.Reset();
    m.adapter.Reset();
    m.backend = DlssNr::PreviewBackend {};
    m.dev12 = nullptr;
    m.queue = nullptr;
    m.ready = FenceCounter {};
    m.done = FenceCounter {};
    m.doneBroken = false;
    m.inited = false;
    m.probedDevice = nullptr;
    m.nvidia = m.forwarder = m.model = Probe::Unknown;
}

// Releases every stream and the shared objects. `drain`: wait for the GPU first (shutdown); a failed wait keeps the model
// features alive (a leak is better than freeing what the GPU still runs) and returns false.
bool ReleaseAll(bool drain)
{
    Module& m = M();
    bool idle = true;
    if (m.doneFence12 != nullptr)
    {
        RepairDone();
        if (drain && m.readyFence12 != nullptr)
        {
            // The D3D12 queue may be waiting for a ready value that the D3D11 context has queued but not flushed (this can run on any
            // thread, so the context is not touched): signalling the ready fence from the CPU releases those waits, and the queued
            // work can finish. Nothing waits on the ready fence after this.
            m.readyFence12->Signal(m.ready.last);
        }
        idle = drain ? WaitCompletion(m.done.last, 5000) : GpuDone(m.done.last);
    }
    if (!idle)
    {
        LOG_WARN("CharaView NR: the GPU did not finish in time at release; the images and model instances are kept");
        return false;
    }
    for (size_t i = 0; i < kMaxStreams; ++i)
    {
        if (m.table.At(static_cast<int>(i)).Used())
        {
            ReleaseStreamObjects(m.streams[i], true);
            m.table.Free(static_cast<int>(i));
        }
    }
    ReleaseGlobal();
    m.vram.Reset();
    m.releasePending = false;
    UpdateHolds();
    return true;
}

Probe ProbeNvidia(ID3D12Device* device, ComPtr<IDXGIAdapter3>& adapter3)
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        return Probe::Unknown;
    DXGI_ADAPTER_DESC1 desc {};
    if (FAILED(adapter->GetDesc1(&desc)))
        return Probe::Unknown;
    adapter.As(&adapter3);
    return desc.VendorId == 0x10DE ? Probe::Present : Probe::Missing;
}

// One-time setup per bridge device. False: not possible now (the availability says why) or failed (the latch is set).
bool EnsureInit(ID3D11DeviceContext* context)
{
    Module& m = M();
    ID3D12Device* dev12 = Dx11WithDx12::GetD3D12Device();
    ID3D12CommandQueue* queue = Dx11WithDx12::GetD3D12CommandQueue();
    if (dev12 == nullptr || queue == nullptr)
        return false;
    if (m.inited && (m.dev12 != dev12 || m.queue != queue))
    {
        LOG_INFO("CharaView NR: the bridge's D3D12 device changed; preview NR objects are rebuilt");
        if (!ReleaseAll(true))
        {
            // The GPU did not finish in time, so the old objects stay (they belong to the previous device and queue). Carrying on with
            // them would mix the old device's objects with the new bridge's queue: the module is not usable until the release succeeds,
            // which Maintain retries.
            m.releasePending = true;
            UpdateHolds();
            return false;
        }
    }
    if (m.releasePending)
        return false;
    if (m.inited)
        return true;
    if (::State::Instance().isRunningOnLinux)
    {
        // The bridge waits for D3D12 on the CPU there; a GPU wait queued on its shared queue could stall it. Not supported yet.
        Latch("not supported when running under Wine or Proton yet");
        return false;
    }

    if (m.probedDevice != dev12)
    {
        m.probedDevice = dev12;
        m.nvidia = ProbeNvidia(dev12, m.adapter);
        m.forwarder = m.model = Probe::Unknown;
    }
    if (Refresh() != Availability::Available)
        return false;
    if (m.forwarder == Probe::Unknown)
    {
        const DlssNr::PreviewBackendStatus status = DlssNr::AcquirePreviewBackend(dev12, m.backend);
        m.probedRuntime = m.backend.revision;
        m.forwarder = status == DlssNr::PreviewBackendStatus::ForwarderMissing ? Probe::Missing : Probe::Present;
        m.model = status == DlssNr::PreviewBackendStatus::ModelMissing ? Probe::Missing : (status == DlssNr::PreviewBackendStatus::ForwarderMissing ? Probe::Unknown : Probe::Present);
        if (status == DlssNr::PreviewBackendStatus::CapabilityFailed)
        {
            Latch("the NGX core would not provide its capability parameters");
            return false;
        }
        if (Refresh() != Availability::Available)
            return false;
    }

    context->GetDevice(&m.dev11);
    if (m.dev11 == nullptr || FAILED(m.dev11.As(&m.dev5)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&m.ctx4))))
    {
        Latch("the D3D11 fence interfaces (ID3D11Device5, ID3D11DeviceContext4) are not available");
        ReleaseGlobal();
        return false;
    }

    m.dev12 = dev12;
    m.queue = queue;
    // ready: created on D3D11, opened on D3D12. done: created on D3D12, opened on D3D11. Each is signalled from its creator's side only.
    HANDLE readyHandle = nullptr;
    HANDLE doneHandle = nullptr;
    const bool fences =
        SUCCEEDED(m.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m.readyFence11))) &&
        SUCCEEDED(m.readyFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &readyHandle)) && readyHandle != nullptr &&
        SUCCEEDED(dev12->OpenSharedHandle(readyHandle, IID_PPV_ARGS(&m.readyFence12))) &&
        SUCCEEDED(dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m.doneFence12))) &&
        SUCCEEDED(dev12->CreateSharedHandle(m.doneFence12.Get(), nullptr, GENERIC_ALL, nullptr, &doneHandle)) && doneHandle != nullptr &&
        SUCCEEDED(m.dev5->OpenSharedFence(doneHandle, IID_PPV_ARGS(&m.doneFence11)));
    if (readyHandle != nullptr)
        CloseHandle(readyHandle);
    if (doneHandle != nullptr)
        CloseHandle(doneHandle);
    if (!fences)
    {
        Latch("the module's own D3D11/D3D12 fence could not be created");
        ReleaseGlobal();
        return false;
    }

    if (!DlssNr::EnsureGpuHooks(dev12))
    {
        Latch("the GPU submission hooks are not available");
        ReleaseGlobal();
        return false;
    }

    const D3D12_COMMAND_LIST_TYPE type = WithDx12::GetD3D12CommandListType();
    bool lists = true;
    for (auto& a : m.allocators)
        lists = lists && SUCCEEDED(dev12->CreateCommandAllocator(type, IID_PPV_ARGS(&a)));
    lists = lists && SUCCEEDED(dev12->CreateCommandList(0, type, m.allocators[0].Get(), nullptr, IID_PPV_ARGS(&m.list))) &&
            SUCCEEDED(m.list->Close());
    m.recorder = std::make_unique<DlssNr_Dx12>("CharaNr compute pass", dev12);
    m.timer = std::make_unique<GpuTime_Dx12>(dev12);
    m.timerRing = TimerRing {};
    if (!lists || m.recorder == nullptr || !m.recorder->IsInit())
    {
        Latch("the command lists or the compute pass could not be created");
        ReleaseGlobal();
        return false;
    }
    m.inited = true;
    UpdateHolds();
    LOG_INFO("CharaView NR: ready on the bridge's D3D12 device; model {}; own fence and {} command allocators", ext::PathText(m.backend.modelPath), kAllocators);
    return true;
}

// ---- Recording -------------------------------------------------------------------------------------------------------------
D3D12_RESOURCE_STATES ToD3D12(State s)
{
    switch (s)
    {
    case State::Common: return D3D12_RESOURCE_STATE_COMMON;
    case State::Uav: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case State::Srv: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

class GpuRecorder
{
  public:
    GpuRecorder(Module& m, Stream& s, ID3D12GraphicsCommandList* list) : _m(m), _s(s), _list(list) {}

    void TimerStart()
    {
        if (_m.timer != nullptr)
        {
            _m.timerRing.NoteStart(); // GpuTime_Dx12::Start advances its slot here, also for a list that is abandoned later
            _m.timer->Start(_list);
        }
    }
    void TimerEnd()
    {
        if (_m.timer != nullptr)
            _m.timer->End(_list);
    }
    void Barrier(Tex t, State from, State to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = Res(t);
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = ToD3D12(from);
        b.Transition.StateAfter = ToD3D12(to);
        _list->ResourceBarrier(1, &b);
    }
    bool Pass(const DlssNrConstants& c, Tex source, Tex model, Tex original, Tex target, Tex keep)
    {
        return _m.recorder->DispatchPass(_list, c, Res(source), Res(model), Res(original), nullptr, nullptr, Res(target), Res(keep));
    }
    bool Evaluate(const ModelCall& c)
    {
        const DlssNr::PreviewBackend& b = _m.backend;
        DlssNr::PreviewEvaluateScope serialised; // the main pass holds the same lock around its evaluate
        DlssNr::PreviewBindModel(b.modelPath);   // the shared capability block must hold this module's callbacks
        // The capability block is shared with the main NR pass: its interface pointers are cleared before every evaluate.
        if (b.setExtras != nullptr)
            b.setExtras(b.capabilityParams, 1.0f, nullptr, nullptr, nullptr, 0, 0, 0, 0);
        DlssNrNative::SetPrecision(ext::Cfg().nr.precision.value_or_default());
        DlssNrNative::BeginEvaluate(_s.feature, c.reset != 0, 1, false);
        const int result = b.evaluate(_list, _s.feature, b.capabilityParams, Res(Tex::Proxy), Res(Tex::Depth), Res(Tex::Motion),
                                      Res(Tex::ModelOut), c.width, c.height, c.guideWidth, c.guideHeight, c.motionWidth, c.motionHeight,
                                      c.depthBaseX, c.depthBaseY, c.motionBaseX, c.motionBaseY, c.depthInverted, c.reset, c.intensity,
                                      c.style, c.structure, c.tone, c.skin, c.autoMask, c.mvScaleX, c.mvScaleY);
        DlssNrNative::EndEvaluate(result == 1);
        if (result != 1)
        {
            const char* text = b.lastModelError != nullptr ? b.lastModelError() : "";
            LOG_ERROR("CharaView NR: the model evaluation returned {:#x} ({})", static_cast<unsigned>(result), text != nullptr ? text : "");
        }
        return result == 1;
    }

  private:
    ID3D12Resource* Res(Tex t) const
    {
        switch (t)
        {
        case Tex::Colour0: return _s.colour[0].r12();
        case Tex::Colour1: return _s.colour[1].r12(); // exists in the late variant only
        case Tex::Proxy: return _s.proxy.Get();
        case Tex::Original: return _s.original.Get();
        case Tex::Depth: return _s.depth.r12();
        case Tex::Motion: return _s.motion.r12();
        case Tex::ModelOut: return _s.modelOut.Get();
        case Tex::Result0: return _s.result[0].r12();
        case Tex::Result1: return _s.result[1].r12();
        case Tex::None: break;
        }
        return nullptr;
    }

    Module& _m;
    Stream& _s;
    ID3D12GraphicsCommandList* _list;
};

Settings SettingsFromConfig()
{
    const ext::NrConfig& nr = ext::Cfg().nr;
    Settings s;
    s.whitePointScale = nr.whitePointScale.value_or_default();
    s.transferStrength = nr.transferStrength.value_or_default();
    s.colourStrength = nr.colourStrength.value_or_default();
    s.maxRatio = nr.maxRatio.value_or_default();
    s.transfer = nr.transfer.value_or_default();
    s.skinProtection = nr.skinProtection.value_or_default();
    s.skinToneEnabled = nr.skinToneEnabled.value_or_default();
    s.skinDetail = nr.skinDetail.value_or_default();
    s.skinColour = nr.skinColour.value_or_default();
    s.environmentDetail = nr.environmentDetail.value_or_default();
    s.environmentColour = nr.environmentColour.value_or_default();
    s.reversibleMode = nr.reversibleMode.value_or_default();
    s.applyModel = nr.applyModel.value_or_default();
    return s;
}

ModelTuning TuningFromBackend()
{
    const DlssNr::PreviewTuning t = DlssNr::CurrentPreviewTuning();
    ModelTuning m;
    m.intensity = t.intensity;
    m.style = t.style;
    m.structure = t.structure;
    m.tone = t.tone;
    m.skin = t.skin;
    m.autoMask = t.autoMask;
    return m;
}

// The steps of one run, issued in the order RunFrame / RunHandoff dictate (CharaNrRules.h; the timeline test drives the same templates).
// Any failure is reported to the caller as a bool.
struct HandoffOps
{
    Module& m;
    Stream& s;
    StreamState& st;
    ID3D11DeviceContext* ctx;
    ID3D11Texture2D* native;
    const ProductionRequest* production; // null: bring-up (also the request of the pre-SR entry, whose images are named the same way)
    Placement placement;                  // what the copies are (CopyPlanFor)
    FramePlan plan;
    int buffer = 0;
    double cpuMs = 0.0;

    // The late variant overwrites a colour image the run before last read: the D3D11 queue first waits for that run (done fence).
    bool WaitColourFree(uint64_t v) { return SUCCEEDED(m.ctx4->Wait(m.doneFence11.Get(), v)); }
    // The copies into the shareable images, as CopyPlanFor lists them. Bring-up: the native image -> the colour image. Production and pre-SR: the
    // input colour (the super-resolution output, or the render-size colour the SR evaluate reads) -> the colour image, and the slot's depth and
    // motion -> the guide images (sizes and formats were checked by the caller). All of them are issued before the ready signal, so the D3D12
    // queue starts after every one of them.
    void CopyColour()
    {
        const CopyPlan copies = CopyPlanFor(placement);
        for (size_t i = 0; i < copies.count; ++i)
        {
            switch (copies.kinds[i])
            {
            case CopyKind::Colour:
                ctx->CopyResource(s.colour[plan.colour == Tex::Colour1 ? 1 : 0].t11(), production != nullptr ? static_cast<ID3D11Resource*>(production->colour) : native);
                break;
            case CopyKind::Depth: ctx->CopyResource(s.depth.t11(), production->depth); break;
            case CopyKind::Motion: ctx->CopyResource(s.motion.t11(), production->motion); break;
            }
        }
    }
    bool Signal11(uint64_t v) { return SUCCEEDED(m.ctx4->Signal(m.readyFence11.Get(), v)); }
    bool Wait12(uint64_t v) { return SUCCEEDED(m.queue->Wait(m.readyFence12.Get(), v)); }
    // Records the list, executes it and queues the done signal. `done` is the value the signal takes (0: the list did not run).
    bool Record(uint64_t& done)
    {
        const auto start = std::chrono::steady_clock::now();
        bool ok = false;
        {
            DlssNr::PreviewStateEnvelope envelope(m.list.Get());
            GpuRecorder recorder(m, s, m.list.Get());
            ok = RecordRun(recorder, plan);
        }
        if (!ok)
        {
            m.list->Close(); // never executed
            return false;
        }
        if (!ExecuteList())
            return false;
        const bool signalled = SignalDone(done);
        st.lastSubmitted = done; // the list ran, whether or not its signal could be queued: release waits for it
        if (signalled)
            cpuMs = MsSince(start);
        return signalled;
    }
    bool Wait11(uint64_t v) { return SUCCEEDED(m.ctx4->Wait(m.doneFence11.Get(), v)); }
};
// Builds the model on its own list; evaluation waits for that list's completion (never in the same list or frame).
RunOutcome CreateFeatureStep(int index, Stream& s, StreamState& st)
{
    Module& m = M();
    if (!BeginList())
    {
        ++m.skipNoList;
        return RunOutcome::Skipped;
    }
    const DlssNr::PreviewBackend& b = m.backend;
    const DlssNr::PreviewTuning t = DlssNr::CurrentPreviewTuning();
    const auto start = std::chrono::steady_clock::now();
    {
        DlssNr::PreviewEvaluateScope serialised;
        DlssNr::PreviewBindModel(b.modelPath); // the shared capability block must hold this module's callbacks
        if (b.setExtras != nullptr)
            b.setExtras(b.capabilityParams, 1.0f, nullptr, nullptr, nullptr, 0, 0, 0, 0);
        DlssNr::PreviewStateEnvelope envelope(m.list.Get());
        s.feature = b.create(b.modelPath.c_str(), b.dataPath.c_str(), m.dev12, m.list.Get(), b.capabilityParams, s.width, s.height, t.preset,
                             t.intensity, t.style, t.structure, t.tone, t.skin, t.autoMask ? 1 : 0, 1);
    }
    const double createMs = MsSince(start);
    if (s.feature == nullptr)
    {
        m.list->Close();
        const char* text = b.lastModelError != nullptr ? b.lastModelError() : "";
        LOG_ERROR("CharaView NR: the model could not be created for {}x{} ({})", s.width, s.height, text != nullptr ? text : "");
        LatchModel("the model would not initialise");
        return RunOutcome::Failed;
    }
    uint64_t value = 0;
    const bool submitted = SubmitList(value);
    if (value != 0)
        st.lastSubmitted = value; // the list ran even if its signal could not be queued: the model must not be freed under it
    if (!submitted)
    {
        Latch("the model creation list could not be submitted");
        return RunOutcome::Failed;
    }
    s.createDone = value;
    s.created = start;
    m.streamRuntime[index] = b.revision;
    const uint64_t after = VramUsedBytes();
    LOG_INFO("CharaView NR: {} model instance {} created for {}x{} in {:.1f} ms (preset {}, style {}); video memory in use {} MB (before the stream's images) -> {} MB",
             PlacementName(PlacementFrom(s.production, s.preSr)), index, s.width, s.height, createMs, t.preset, t.style, s.vramBefore >> 20, after >> 20);
    return RunOutcome::Preparing;
}

void NoteFeatureReady(Stream& s, int index)
{
    if (s.readyLogged)
        return;
    s.readyLogged = true;
    Module& m = M();
    const uint64_t used = VramUsedBytes();
    const uint64_t added = AddedMegabytes(s.vramBefore, used);
    m.vram.Added(s.vramAddedMb, added);
    LOG_INFO("CharaView NR: model instance {} ready after {:.0f} ms; video memory in use {} MB ({} MB more than before the stream's images and model were created)", index,
             MsSince(s.created), used >> 20, added);
}

void LogCostIfDue(uint64_t now)
{
    Module& m = M();
    if (!m.cpuCost.Due(now))
        return;
    const auto cpu = m.cpuCost.Take(now);
    const auto gpu = m.gpuCost.Take(now);
    const auto productionGpu = m.productionGpuCost.Take(now);
    const auto preSrGpu = m.preSrGpuCost.Take(now);
    if (gpu.count != 0)
        LOG_INFO("CharaView NR cost: {} runs, GPU {:.2f} ms average ({:.2f} max), CPU recording {:.2f} ms average ({:.2f} max); {} stream(s)", cpu.count,
                 gpu.average, gpu.max, cpu.average, cpu.max, m.table.UsedCount());
    else
        LOG_INFO("CharaView NR cost: {} runs, CPU recording {:.2f} ms average ({:.2f} max); GPU time not measured yet", cpu.count, cpu.average, cpu.max);
    if (productionGpu.count != 0)
        LOG_INFO("CharaView NR cost, production runs alone: GPU {:.2f} ms average ({:.2f} max) over {} measured runs", productionGpu.average,
                 productionGpu.max, productionGpu.count);
    if (preSrGpu.count != 0)
        LOG_INFO("CharaView NR cost, pre-SR runs alone: GPU {:.2f} ms average ({:.2f} max) over {} measured runs", preSrGpu.average, preSrGpu.max,
                 preSrGpu.count);
}


RunOutcome Run(int index, Stream& s, const RunRequest& req, const ProductionRequest* production, Placement placement)
{
    Module& m = M();
    ReplayScope own; // the copy and clear below go through the hooked context: they are not the game's work
    StreamState& st = m.table.At(index);

    const bool late = s.late; // decided when the stream was created: the run and its consumer always agree
    PlanInputs planInputs;
    planInputs.width = s.width;
    planInputs.height = s.height;
    planInputs.carriesGuides = production != nullptr;
    planInputs.guidesInit = s.guidesInit;
    if (production != nullptr)
    {
        planInputs.guides = production->guides;
        planInputs.superResolutionReset = production->superResolutionReset;
    }
    planInputs.late = late;
    planInputs.now = req.now;
    ResetReason reset = ResetReason::None;
    FramePlan plan = MakeFramePlan(st, planInputs, &reset); // the one place the plan is made (tested as it is)
    const int resultBuffer = ResultBufferForRun(st.runs);
    plan.settings = SettingsFromConfig();
    plan.tuning = TuningFromBackend();

    // A free command allocator is needed before anything is queued: none free (many windows open, a slow GPU) is not an error, the
    // native image stays for this render.
    if (!BeginList())
    {
        ++m.skipNoList;
        return RunOutcome::Skipped;
    }

    ID3D11DeviceContext* ctx = req.context;
    if (plan.initGuides)
    {
        const FLOAT farPlane[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // the bring-up depth: everything at the far plane, not inverted
        ctx->ClearRenderTargetView(s.depth.rtv.Get(), farPlane);
    }

    const int buffer = resultBuffer;
    HandoffOps ops { m, s, st, ctx, req.native, production, placement, plan, buffer };
    const HandoffOutcome out = RunFrame(ops, m.ready, late, s.doneValue[buffer]);
    if (!out.ok)
    {
        if (out.failedAt == HandoffStep::ReuseWait || out.failedAt == HandoffStep::Signal11 || out.failedAt == HandoffStep::Wait12)
            m.list->Close(); // the list was reset for this run but never recorded
        char text[96];
        std::snprintf(text, sizeof(text), "a run failed at the %s step", HandoffStepName(out.failedAt));
        if (out.failedAt == HandoffStep::Record)
            LatchModel(text); // the recording step includes the model's evaluate
        else
            Latch(text);
        return RunOutcome::Failed;
    }

    s.doneValue[buffer] = out.done;
    m.timerRing.NoteSubmitted(out.done, placement); // the list of this run wrote the timer slot Start advanced to
    s.serial[buffer] = m.serialCounter.fetch_add(1, std::memory_order_relaxed) + 1; // a new result: its tint (if any) is stale
    s.guidesInit = true;
    st.everRan = true;
    st.lastRun = req.now;
    st.ranAttempt = st.attempts; // this call produced the result that is there now
    ++st.runs;
    st.chain = reset != ResetReason::None ? 1 : st.chain + 1; // the late variant shows the run before the newest only inside one chain
    m.runs.fetch_add(1, std::memory_order_relaxed);
    m.lastCpuUs.store(static_cast<uint64_t>(ops.cpuMs * 1000.0), std::memory_order_relaxed);
    m.cpuCost.Add(ops.cpuMs);
    ext::present::RecordSample(ext::present::Phase::PreviewNr, static_cast<uint64_t>(ops.cpuMs * 1000.0)); // PT frame-pacing trace
    if (placement == Placement::PostSr)
    {
        m.productionRuns.fetch_add(1, std::memory_order_relaxed);
        m.lastProductionCpuUs.store(static_cast<uint64_t>(ops.cpuMs * 1000.0), std::memory_order_relaxed);
    }
    else if (placement == Placement::PreSr)
    {
        m.preSrRuns.fetch_add(1, std::memory_order_relaxed);
        m.lastPreSrCpuUs.store(static_cast<uint64_t>(ops.cpuMs * 1000.0), std::memory_order_relaxed);
    }
    // The timer reads the slot of the run two back; it is read only when that run's list is complete, and the time is attributed to the mode
    // of that run (not of the current one): with several streams in a frame the slot may belong to another stream or the other mode.
    if (m.timer != nullptr && m.timerRing.CanRead(GpuDone))
    {
        const Placement measured = m.timerRing.ReadMode();
        if (const auto gpuMs = m.timer->ReadGpuTime(m.queue); gpuMs.has_value())
        {
            m.lastGpuUs.store(static_cast<uint64_t>(*gpuMs * 1000.0), std::memory_order_relaxed);
            m.gpuCost.Add(*gpuMs);
            if (measured == Placement::PostSr)
            {
                m.lastProductionGpuUs.store(static_cast<uint64_t>(*gpuMs * 1000.0), std::memory_order_relaxed);
                m.productionGpuCost.Add(*gpuMs);
            }
            else if (measured == Placement::PreSr)
            {
                m.lastPreSrGpuUs.store(static_cast<uint64_t>(*gpuMs * 1000.0), std::memory_order_relaxed);
                m.preSrGpuCost.Add(*gpuMs);
            }
        }
    }
    if (st.runs == 1)
        LOG_INFO("CharaView NR: stream {} first {} run {}x{} (guides {}x{}; {}, history {})", index, PlacementName(placement), s.width, s.height,
                 s.guideWidth, s.guideHeight, late ? "late variant" : "same frame", ResetReasonText(reset));
    LogCostIfDue(req.now);
    return RunOutcome::Ran;
}
} // namespace

bool Wanted() { return ext::Cfg().ffxiv.chara.previewNr.value_or_default(); }
bool Latched() { return M().latches.NrLatched() || M().modelLatched; }
bool Available() { return static_cast<Availability>(M().availabilitySnapshot.load(std::memory_order_relaxed)) == Availability::Available; }
bool HoldsResources() { return M().holds.load(std::memory_order_relaxed); }
Latches& SharedLatches() { return M().latches; }

// The images a production run copies from, as the D3D11 side reports them.
static ImageInfo InfoOf(ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC d {};
    if (texture != nullptr)
        texture->GetDesc(&d);
    return ImageInfo { d.Width, d.Height, static_cast<uint32_t>(d.Format) };
}

// The NR runtime picker (ext/nr/runtime) selected another model file: the backend follows (new model path and revision) and the streams
// built from the old file are rebuilt one by one in RunCoreSteps. A selection without a model makes the module unavailable (the native
// image stays) until a model is selected again. Caller holds the module mutex and the module is initialised.
static void SyncRuntime()
{
    Module& m = M();
    const uint64_t revision = DlssNr::PreviewRuntimeRevision();
    if (revision == m.backend.revision)
        return;
    DlssNr::PreviewBackend next;
    const DlssNr::PreviewBackendStatus status = DlssNr::AcquirePreviewBackend(m.dev12, next);
    switch (status)
    {
    case DlssNr::PreviewBackendStatus::Ready:
        m.backend = next;
        m.model = Probe::Present;
        LOG_INFO("CharaView NR: NR runtime revision {}; model {}; streams built from the previous model are rebuilt", next.revision,
                 ext::PathText(next.modelPath));
        break;
    case DlssNr::PreviewBackendStatus::ModelMissing:
        // Remember the revision so this is not retried every call; the entry points stay as they were (a stream holding the old model
        // keeps it until it is rebuilt), and the availability now says the model is missing.
        m.backend.revision = revision;
        m.backend.modelPath.clear();
        m.model = Probe::Missing;
        LOG_INFO("CharaView NR: NR runtime revision {} has no model; previews keep the native image until one is selected", revision);
        break;
    default:
        Latch("the NGX core would not provide its capability parameters");
        break;
    }
    Refresh();
}

// Looks at the NR runtime selection and does what ObservePreviewRuntime says. Called from the maintenance call (which runs whatever the
// availability is) AND at the top of every draw: a model failure latch and a missing model both CLOSE the gate in front of the draw
// entry points, so the draws alone could never see the selection change that is supposed to open it again (review N1).
// Caller holds the module mutex.
static void ObserveRuntime()
{
    Module& m = M();
    ext::nr::runtime::PreviewRuntimeView view;
    view.inited = m.inited;
    view.modelMissing = m.model == Probe::Missing;
    view.modelLatched = m.modelLatched;
    view.backendRevision = m.backend.revision;
    view.probedRevision = m.probedRuntime;
    view.latchedRevision = m.latchedRuntime;
    const uint64_t revision = DlssNr::PreviewRuntimeRevision();
    const ext::nr::runtime::PreviewRuntimeActions action = ext::nr::runtime::ObservePreviewRuntime(view, revision);
    if (action.clearModelLatch)
    {
        m.modelLatched = false; // another model gets its own chance
        LOG_INFO("CharaView NR: NR runtime revision {}; the model failure of the previous runtime is forgotten", revision);
    }
    if (action.resync && m.inited)
        SyncRuntime();
    else if (action.reprobe)
        m.forwarder = m.model = Probe::Unknown; // started without a model, or one that was not installed: EnsureInit probes again
    if (action.clearModelLatch || action.resync || action.reprobe)
        Refresh();
}

// All three placements run through this one function; `production` is null for the bring-up (and set for the other two, whose requests carry
// the copied images). The stream table, the availability and creation steps, the settle-and-rebuild rule, the model creation and the readiness
// wait are common; the placement decides the shape the stream is built for and what the hand-off copies (see Stream).
static RunOutcome RunCoreSteps(const RunRequest& req, const ProductionRequest* production, Placement placement)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (req.context == nullptr || req.native == nullptr || req.width == 0 || req.height == 0)
        return RunOutcome::Skipped;
    if (production != nullptr && (production->colour == nullptr || production->depth == nullptr || production->motion == nullptr ||
                                  production->guides.width == 0 || production->guides.height == 0))
        return RunOutcome::Skipped;

    // Every call that reaches a preview that has a stream is an attempt. The result in the stream is current only while the newest
    // attempt produced it: a render that was skipped, still preparing or failed leaves the older result stale, and a consumer draw must
    // not show it for this render (ResultFor).
    const uintptr_t key = reinterpret_cast<uintptr_t>(req.native);
    if (const int known = m.table.Find(key); known != kNoStream)
        ++m.table.At(known).attempts;
    // [DlssNr] DisableOnFocusLoss for the previews: while the game window is in the background the run ends as Paused (attempt counted above, so no stale
    // image; not counted as a fallback, see CountsAsFallback).
    if (!ext::nr::FocusAllowsPreviewNr(Wanted() && ext::Cfg().nr.disableOnFocusLoss.value_or_default()))
    {
        if (const int openStream = m.table.Find(key); openStream != kNoStream)
            m.table.At(openStream).lastLive = req.now; // the preview window is still open: Maintain must not release the model during a long alt-tab
        return RunOutcome::Paused;
    }

    // Several windows: with the budget on, only an image the set processes may own a stream. The others keep the native picture, which
    // is what the set exists for; the stream of an image that left the set is released by the linger rule as usual.
    g_windowSet.Configure(ext::Cfg().ffxiv.chara.previewBudget.value_or_default(),
                          static_cast<uint32_t>(ext::Cfg().ffxiv.chara.previewMaxWindows.value_or_default()));
    if (!g_windowSet.Processed(key))
    {
        if (!m.setSkipLogged)
        {
            m.setSkipLogged = true;
            LOG_INFO("CharaView NR: a preview window is not one of the processed windows (PreviewMaxWindows); it keeps the game's picture and "
                     "gets no NR model while it is outside the set");
        }
        return RunOutcome::Skipped;
    }

    ObserveRuntime(); // before the availability is read: a selection without a model must show at once
    if (Refresh() != Availability::Available || !EnsureInit(req.context))
        return RunOutcome::NotAvailable;

    int index = m.table.Bind(key, req.width, req.height, req.now, req.entry);
    if (index == kNoStream && g_windowSet.On())
    {
        // The table is full: the least recently used stream that is not live and whose GPU work is finished makes room, so a window
        // that is closed does not block a new one (only with the budget on: off, the linger rule alone decides).
        const int victim = m.table.Evictable(req.now, [](uint64_t done) { return GpuDone(done); });
        if (victim != kNoStream)
        {
            LOG_INFO("CharaView NR: stream {} (output entry {}) released to make room for another preview window (its preview was not drawn lately)",
                     victim, m.table.At(victim).entry);
            ReleaseStreamObjects(m.streams[victim], true);
            m.table.Free(victim);
            index = m.table.Bind(key, req.width, req.height, req.now, req.entry);
        }
    }
    if (index == kNoStream)
    {
        ++m.skipTableFull;
        if (!m.tableFullLogged)
        {
            m.tableFullLogged = true;
            LOG_INFO("CharaView NR: all {} preview NR streams are in use (open windows and windows still lingering); further previews keep the "
                     "native image until one is released",
                     kMaxStreams);
        }
        return RunOutcome::Skipped;
    }
    if (m.table.At(index).attempts == 0)
        m.table.At(index).attempts = 1; // a stream Bind just created

    // A stream whose model instance was built from another NR runtime (the card selected a different model file) is rebuilt from the
    // current one, once the GPU is finished with it. The old instance is released through the forwarder, which routes the release to the
    // module that created it; no model file is ever unloaded.
    if (m.streams[index].feature != nullptr && m.streamRuntime[index] != m.backend.revision)
    {
        if (!GpuDone(m.table.At(index).lastSubmitted))
            return RunOutcome::Skipped;
        LOG_INFO("CharaView NR: stream {} rebuilt for NR runtime revision {} (was {})", index, m.backend.revision, m.streamRuntime[index]);
        ReleaseStreamObjects(m.streams[index], true);
        m.table.Free(index);
        index = m.table.Bind(key, req.width, req.height, req.now, req.entry);
        if (index == kNoStream)
        {
            ++m.skipTableFull;
            return RunOutcome::Skipped;
        }
        m.table.At(index).attempts = 1;
    }

    // What the stream has to be built for. A change (size, the model preset or style, the hand-off variant) must hold still for a
    // moment before the stream is rebuilt: dragging a window to a new size reports a new size every frame, and each one would
    // otherwise be a new model.
    const DlssNr::PreviewTuning tuning = DlssNr::CurrentPreviewTuning();
    // PreviewNRLate applies to the bring-up only (ShapeFor): the alpha pass needs this render's colour, and the SR evaluate reads the pre-SR result
    // right after the run.
    const StreamShape wanted = ShapeFor(placement, req.width, req.height, tuning.preset, tuning.style, Late(),
                                        production != nullptr ? production->guides.width : 0, production != nullptr ? production->guides.height : 0);
    m.table.At(index).lastLive = req.now;
    switch (DecideRebuild(m.table.At(index), wanted, req.now))
    {
    case RebuildAction::Settle: UpdateHolds(); ++m.skipSettling; return RunOutcome::Skipped;
    case RebuildAction::Rebuild:
    {
        // The old stream goes once the GPU is finished with it.
        if (!GpuDone(m.table.At(index).lastSubmitted))
            return RunOutcome::Skipped;
        const StreamShape old = m.table.At(index).built;
        LOG_INFO("CharaView NR: stream {} rebuilt: {} {}x{} -> {} {}x{}, preset {} -> {}, style {} -> {}, {} -> {}", index, PlacementName(PlacementOf(old)),
                 old.width, old.height, PlacementName(PlacementOf(wanted)), wanted.width, wanted.height, old.preset, wanted.preset, old.style,
                 wanted.style, old.late ? "late" : "same frame", wanted.late ? "late" : "same frame");
        ReleaseStreamObjects(m.streams[index], true);
        m.table.Free(index);
        index = m.table.Bind(key, req.width, req.height, req.now, req.entry);
        if (index == kNoStream)
        {
            ++m.skipTableFull;
            return RunOutcome::Skipped;
        }
        m.table.At(index).attempts = 1;
        m.table.At(index).lastLive = req.now;
        break;
    }
    case RebuildAction::Keep: break;
    }
    Stream& s = m.streams[index];
    StreamState& st = m.table.At(index);
    UpdateHolds();

    // A production run copies three images into the stream: their sizes and formats are the stream's, or the copy is not allowed. They are
    // fixed for a slot's life (a slot rebuild changes the sizes and so the wanted shape), so this only trips on a programming error.
    if (production != nullptr &&
        !CopiedInputsFit(wanted, InfoOf(production->colour), InfoOf(production->depth), InfoOf(production->motion)))
    {
        Latch("the images handed to NR (colour, depth, motion) do not match the stream's sizes and formats");
        return RunOutcome::Failed;
    }
    if (!s.gpuObjects)
    {
        D3D11_TEXTURE2D_DESC nd {};
        req.native->GetDesc(&nd);
        const bool formatOk = production != nullptr || nd.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        s.vramBefore = VramUsedBytes(); // before the images, so the figure covers the images as well as the model
        if (!formatOk || !CreateStreamGpu(s, wanted, nd.Format))
        {
            Latch(!formatOk ? "the preview image is not B8G8R8A8_UNORM" : "the shared images could not be created");
            return RunOutcome::Failed;
        }
        st.built = wanted;
        st.hasBuilt = true;
    }
    if (s.feature == nullptr)
        return CreateFeatureStep(index, s, st);
    if (!s.featureReady)
    {
        if (!GpuDone(s.createDone))
            return RunOutcome::Preparing;
        s.featureReady = true;
        NoteFeatureReady(s, index);
        return RunOutcome::Preparing; // the first evaluation is on the next render
    }
    const RunOutcome outcome = Run(index, s, req, production, placement);
    UpdateHolds();
    return outcome;
}

// The one place a call of either entry ends: a call that produced no result is counted as a render that kept the image, once (the steps
// above pass their outcome up without counting).
static RunOutcome RunCore(const RunRequest& req, const ProductionRequest* production, Placement placement)
{
    return CountedCall(M().fallbacks, placement, [&] { return RunCoreSteps(req, production, placement); },
                       [](RunOutcome outcome) { return !CountsAsFallback(outcome); });
}

// One call of either entry. A call that ends without a run breaks the stream's chain (the late variant must not show a result from before
// this render; the attempt itself was counted when the call reached the stream, see RunCore).
static RunOutcome RunBringUpImpl(const RunRequest& req)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    const RunOutcome outcome = RunCore(req, nullptr, Placement::BringUp);
    NoteBringUpEnded(m.table, reinterpret_cast<uintptr_t>(req.native), outcome == RunOutcome::Ran);
    return outcome;
}

// The production entry (NR on the super-resolution output of this render) and the pre-SR entry (NR on the render-size colour the SR evaluate is
// about to read) share this body. On a run, the result image is handed back with its own reference (the alpha pass samples it, or the SR evaluate
// reads it, right away, on this thread; the inline D3D11 wait was queued by the hand-off). The pre-SR entry also gets the texture, the handle the
// evaluate takes. Any other outcome returns no view and the caller keeps the plain image of its step.
static ProductionResult RunCopiedImpl(const ProductionRequest& request, Placement placement)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    ProductionResult result;
    const uintptr_t key = reinterpret_cast<uintptr_t>(request.base.native);
    result.outcome = RunCore(request.base, &request, placement);
    NoteBringUpEnded(m.table, key, result.outcome == RunOutcome::Ran);
    if (result.outcome != RunOutcome::Ran)
        return result; // RunCore counted it (unless it was a focus pause: not a fallback, see CountsAsFallback)
    const int index = m.table.Find(key);
    if (index == kNoStream || PlacementFrom(m.streams[index].production, m.streams[index].preSr) != placement || m.table.At(index).runs == 0)
    {
        result.outcome = RunOutcome::Failed; // cannot happen: a run just finished on this stream
        m.fallbacks.NoteFor(placement); // the run was counted as a run; this render still keeps the plain image of its step
        return result;
    }
    const Stream& s = m.streams[index];
    const int buffer = ResultBufferForRun(m.table.At(index).runs - 1); // the run that just finished wrote this image
    result.view = s.result[buffer].srv;
    result.serial = s.serial[buffer];
    if (placement == Placement::PreSr)
        result.texture = s.result[buffer].t11(); // ComPtr: takes its own reference
    return result;
}

static void InvalidateImpl(uintptr_t key)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    (void) InvalidateResult(m.table, key);
}

// The consumer's side of the hand-off: the late variant waits (GPU side) for the run that wrote the image it is about to read.
struct ConsumerOps
{
    Module& m;
    bool WaitResult(uint64_t done) { return SUCCEEDED(m.ctx4->Wait(m.doneFence11.Get(), done)); }
};

// Queues the D3D11 wait a consumer needs before it reads result buffer `buffer` of stream `index` (the late variant; the default variant
// waited inline after the run). False: the wait could not be queued, preview NR is latched.
static bool WaitForResult(Module& m, const Stream& s, int buffer)
{
    ConsumerOps ops { m };
    if (ConsumerWait(ops, s.late, s.doneValue[buffer]))
        return true;
    Latch("the D3D11 wait on the previous result failed");
    return false;
}

static ResultView ResultForImpl(uintptr_t key, uint64_t now)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (!m.inited || m.availability != Availability::Available)
        return {};
    const int index = m.table.Find(key);
    if (index == kNoStream)
        return {};
    const StreamState& st = m.table.At(index);
    // ResultForDecision: only the result of the newest call, only a bring-up result (a production stream holds the raw output of the
    // super-resolution image, which only the alpha pass of that path reads), and only while it is recent.
    Stream& s = m.streams[index];
    const int buffer = ResultForDecision(st, s.late, now);
    if (buffer < 0 || !WaitForResult(m, s, buffer))
        return {};
    ResultView view;
    view.view = s.result[buffer].srv; // a reference of its own: the view outlives the lock (see CharaNr.h)
    view.serial = s.serial[buffer];
    return view;
}

static ComPtr<ID3D11Texture2D> ResultTextureImpl(uint64_t now, bool waitForResult, ResultKind* kind)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (kind != nullptr)
        *kind = ResultKind::None;
    if (!m.inited || m.availability != Availability::Available)
        return nullptr;
    int best = kNoStream;
    for (size_t i = 0; i < kMaxStreams; ++i)
    {
        const StreamState& st = m.table.At(static_cast<int>(i));
        if (!st.Used() || !ResultTextureServes(ResultKindOf(st)) || now < st.lastRun || now - st.lastRun > kLiveMs)
            continue; // no current result, or a pre-SR one (an input of the SR evaluate: the dump has no label for it)
        if (best == kNoStream || st.lastRun > m.table.At(best).lastRun)
            best = static_cast<int>(i);
    }
    if (best == kNoStream)
        return nullptr;
    const StreamState& st = m.table.At(best);
    const Stream& s = m.streams[best];
    const int buffer = ConsumerBuffer(s.late, st.runs, st.chain);
    if (buffer < 0 || (waitForResult && !WaitForResult(m, s, buffer)))
        return nullptr;
    if (kind != nullptr)
        *kind = ResultKindOf(st);
    return s.result[buffer].t11(); // ComPtr: takes its own reference
}

static void MaintainImpl(uint64_t now)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (now >= m.lastMaintain && now - m.lastMaintain < 100)
        return;
    m.lastMaintain = now;

    // A done signal that failed behind a list that ran leaves the done fence below the value of that list. Streams and the final release
    // below decide from the fence alone, so they would wait for it forever: repair it first (cheap; it does nothing unless it is broken).
    RepairDone();

    // The objects of a previous bridge device that the GPU was still using when it changed: released as soon as it is done.
    if (m.releasePending && ReleaseAll(false))
        LOG_INFO("CharaView NR: the objects of the previous graphics device were released");

    ObserveRuntime(); // the draws are not reached while a model failure or a missing model closes the gate: look at the selection here
    const Availability a = Refresh(); // also with nothing created: the dispatcher reads the cached result (Available())
    if (!m.inited && m.table.UsedCount() == 0)
        return;
    const bool disabled = a != Availability::Available;
    // Several windows: while the processed set is full and an image waits for a place, a closed window's model goes after the live window
    // (a quarter of a second) instead of its full linger time, so the place arrives quickly. Only with the budget on.
    g_windowSet.Configure(ext::Cfg().ffxiv.chara.previewBudget.value_or_default(),
                          static_cast<uint32_t>(ext::Cfg().ffxiv.chara.previewMaxWindows.value_or_default()));
    const bool pressure = g_windowSet.On() && g_windowSet.HasWaitingCandidate(now);
    const uint64_t linger = pressure ? 0 : LingerMs(ext::Cfg().ffxiv.chara.previewNrLingerSeconds.value_or_default());
    for (size_t i = 0; i < kMaxStreams; ++i)
    {
        const int index = static_cast<int>(i);
        if (!m.table.At(index).Used())
            continue;
        Stream& s = m.streams[i];
        if (s.feature != nullptr && !s.featureReady && GpuDone(s.createDone))
        {
            s.featureReady = true;
            NoteFeatureReady(s, index);
        }
        const StreamState& st = m.table.At(index);
        const StreamFacts f = FactsOf(st, disabled, now, linger, GpuDone(st.lastSubmitted));
        if (DecideStream(f) == StreamAction::Release)
        {
            LOG_INFO("CharaView NR: stream {} released ({})", index, disabled ? "preview NR is not available" : "its preview window closed");
            ReleaseStreamObjects(s, true);
            m.table.Free(index);
        }
    }
    m.liveCount.store(static_cast<uint32_t>(m.table.LiveCount(now)), std::memory_order_relaxed);
    if (m.table.UsedCount() == 0 && m.inited && disabled && GpuDone(m.done.last))
        ReleaseGlobal(); // nothing left: the compute pass objects and fences go too
    UpdateHolds();
}

void OnFrameEnd() {}

static void OnBridgeShutdownImpl()
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    m.holdOff = true;
    if (m.inited || m.table.UsedCount() != 0)
        LOG_INFO("CharaView NR: the graphics bridge is shutting down; preview NR objects are released");
    if (!ReleaseAll(true))
        m.releasePending = true; // the GPU is still using them: the module is not usable until Maintain has released them
    UpdateHolds();
}

static void OnBridgeInitImpl()
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    m.holdOff = false;
}

static void DetachImpl()
{
    // Draw thread, device valid, the hooks are about to go. No wait here (the earlier version waited up to five seconds on a fence event
    // with the module locked, which is unsafe if this ever ran under the loader lock): the D3D11 context is flushed so that signals the
    // D3D12 queue may be waiting for are on their way, and whatever the GPU has finished is released. What it is still using stays; the
    // bridge shutdown (OnBridgeShutdown, which drains) or the device teardown takes it. The super-resolution path does no GPU wait in
    // its Detach either.
    Module& m = M();
    std::lock_guard lock(m.mutex);
    // The module is closed from here on, like at the bridge shutdown: a finisher that arrives before the hooks are removed (or when their
    // removal is skipped) must not rebuild fences, allocators and D3D11 interface pointers on a device that is going away. The hold-off is
    // cleared when the hooks are installed again (OnBridgeInit, called by the dispatcher).
    m.holdOff = true;
    if (m.ctx4 != nullptr)
        m.ctx4->Flush();
    if (!ReleaseAll(false))
    {
        m.releasePending = true;
        UpdateHolds();
        LOG_INFO("CharaView NR: the GPU was still using preview NR objects when the hooks were removed; they are released later");
    }
}

static Status GetStatusImpl()
{
    Module& m = M();
    Status s;
    {
        std::lock_guard lock(m.mutex);
        const Availability a = Evaluate(Facts());
        s.availability = a;
        s.reason = ReasonText(a);
        for (size_t i = 0; i < kMaxStreams; ++i)
        {
            const StreamState& st = m.table.At(static_cast<int>(i));
            s.late = s.late || (st.Used() && st.hasBuilt && LateVariantInUse(st.built));
            s.streamEntries[i] = st.Used() ? st.entry : -1;
            s.streamRuns[i] = st.Used() ? st.runs : 0;
        }
        s.skipTableFull = m.skipTableFull;
        s.skipSettling = m.skipSettling;
        s.skipNoList = m.skipNoList;
        s.streamsPeak = m.streamsPeak;
    }
    s.streams = m.streamCount.load(std::memory_order_relaxed);
    s.liveStreams = m.liveCount.load(std::memory_order_relaxed);
    s.runs = m.runs.load(std::memory_order_relaxed);
    s.fallbacks = m.fallbacks.all.load(std::memory_order_relaxed);
    s.lastGpuMs = static_cast<double>(m.lastGpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.lastCpuMs = static_cast<double>(m.lastCpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.productionRuns = m.productionRuns.load(std::memory_order_relaxed);
    s.productionFallbacks = m.fallbacks.production.load(std::memory_order_relaxed);
    s.productionGpuMs = static_cast<double>(m.lastProductionGpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.productionCpuMs = static_cast<double>(m.lastProductionCpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.preSrRuns = m.preSrRuns.load(std::memory_order_relaxed);
    s.preSrFallbacks = m.fallbacks.preSr.load(std::memory_order_relaxed);
    s.preSrGpuMs = static_cast<double>(m.lastPreSrGpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.preSrCpuMs = static_cast<double>(m.lastPreSrCpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.preSrLatched = m.latches.PreSrLatched();
    s.videoMemoryMb = m.vram.Total();
    return s;
}

// An exception inside a teardown entry leaves objects behind: mark the release pending (a plain store, it cannot throw), so Maintain retries it
// and EnsureInit builds nothing new on top of them. The latch log line is written once, by the entry's handler.
static void MarkReleasePending() noexcept { M().releasePending = true; }

// ---- The public entry points: the boundary towards the game --------------------------------------------------------------------
// Each one runs its body through Guarded (CharaNrRules.h): an exception (an out-of-memory, a failing library call, a path that cannot be
// converted) latches the module with one fixed log line and gives the answer "no NR" instead of leaving the game's Draw hook. The latch
// text is a literal on purpose: the handler must not do anything that can throw itself.
RunOutcome RunBringUp(const RunRequest& req)
{
    return Guarded([&] { return RunBringUpImpl(req); },
                   []
                   {
                       M().fallbacks.Note(false); // counted before the latch line, which can throw
                       Latch("an unexpected exception stopped the bring-up");
                       return RunOutcome::Failed;
                   });
}

ProductionResult RunProduction(const ProductionRequest& request)
{
    return Guarded([&] { return RunCopiedImpl(request, Placement::PostSr); },
                   []
                   {
                       M().fallbacks.Note(true); // counted before the latch line, which can throw
                       Latch("an unexpected exception stopped the production run");
                       ProductionResult failed;
                       failed.outcome = RunOutcome::Failed;
                       return failed;
                   });
}

ProductionResult RunPreSr(const ProductionRequest& request)
{
    return Guarded([&] { return RunCopiedImpl(request, Placement::PreSr); },
                   []
                   {
                       M().fallbacks.NoteFor(Placement::PreSr); // counted before the latch line, which can throw
                       Latch("an unexpected exception stopped the pre-SR run");
                       ProductionResult failed;
                       failed.outcome = RunOutcome::Failed;
                       return failed;
                   });
}

void NoteProductionDeclined()
{
    M().fallbacks.Note(true);
}

void NotePreSrDeclined()
{
    M().fallbacks.NoteFor(Placement::PreSr);
}

ResultView ResultFor(ID3D11DeviceContext*, uintptr_t key, uint64_t now)
{
    return Guarded([&] { return ResultForImpl(key, now); },
                   []() -> ResultView
                   {
                       Latch("an unexpected exception while looking up a result");
                       return {};
                   });
}

void Invalidate(uintptr_t key)
{
    Guarded([&] { InvalidateImpl(key); }, [] { Latch("an unexpected exception while withdrawing a result"); });
}

ComPtr<ID3D11Texture2D> ResultTexture(uint64_t now, bool waitForResult, ResultKind* kind)
{
    return Guarded([&] { return ResultTextureImpl(now, waitForResult, kind); },
                   []() -> ComPtr<ID3D11Texture2D>
                   {
                       Latch("an unexpected exception while looking up the result texture");
                       return nullptr;
                   });
}

void Maintain(uint64_t now)
{
    GuardedTeardown([&] { MaintainImpl(now); }, MarkReleasePending, [] { Latch("an unexpected exception in the maintenance"); });
}

void OnBridgeShutdown()
{
    GuardedTeardown([&] { OnBridgeShutdownImpl(); }, MarkReleasePending, [] { Latch("an unexpected exception at the bridge shutdown"); });
}

void OnBridgeInit()
{
    Guarded([&] { OnBridgeInitImpl(); }, [] { Latch("an unexpected exception at the bridge start"); });
}

void Detach()
{
    GuardedTeardown([&] { DetachImpl(); }, MarkReleasePending, [] { Latch("an unexpected exception while detaching"); });
}

Status GetStatus()
{
    return Guarded([&] { return GetStatusImpl(); },
                   []
                   {
                       Latch("an unexpected exception while reading the status");
                       Status s;
                       s.availability = Availability::Latched;
                       s.reason = ReasonText(Availability::Latched);
                       return s;
                   });
}
} // namespace ext::ffxiv::chara::nr
