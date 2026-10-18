#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaNr.h"
#include "ext/ffxiv/chara/CharaNrRecipe.h"
#include "ext/gpu/Gpu.h"
#include "ext/nr/DlssNr.h"
#include "ext/nr/native/DlssNrNative.h"
#include "ext/nr/pass/DlssNr_Dx12.h"

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
constexpr size_t kAllocators = 4;
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

struct Stream
{
    uint32_t width = 0, height = 0;
    bool late = false;            // the hand-off variant, decided when the stream is created (a config change rebuilds the stream)
    Shared colour[2];             // the late variant uses both (the run before may still read one while the copy writes the other)
    Shared result[2], depth, motion;
    ComPtr<ID3D12Resource> proxy, original, modelOut;
    void* feature = nullptr;
    bool featureReady = false;
    uint64_t createDone = 0;      // completion value of the list that created the feature
    uint64_t doneValue[2] = {};   // done-fence value after which result[i] (and colour[i] in the late variant) is no longer in use
    bool guidesInit = false;
    bool gpuObjects = false;
    uint64_t vramBefore = 0;
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
    bool releasePending = false;         // a device change found the GPU busy: the old objects are kept and released in Maintain

    DlssNr::PreviewBackend backend;
    std::unique_ptr<DlssNr_Dx12> recorder;
    std::unique_ptr<GpuTime_Dx12> timer;
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
    Availability availability = Availability::ConfigOff;
    std::atomic<uint8_t> availabilitySnapshot { static_cast<uint8_t>(Availability::Available) }; // readable without the mutex
    uint64_t lastMaintain = 0;

    std::atomic<bool> holds { false };
    std::atomic<uint64_t> runs { 0 }, fallbacks { 0 };
    std::atomic<uint32_t> streamCount { 0 }, liveCount { 0 };
    std::atomic<uint64_t> lastGpuUs { 0 }, lastCpuUs { 0 };
    std::atomic<uint64_t> vramAddedMb { 0 };
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

bool Late() { return ext::Cfg().ffxiv.chara.previewNrLate.value_or_default(); }

GateFacts Facts()
{
    Module& m = M();
    GateFacts f;
    f.configOn = Wanted();
    f.latched = m.latches.NrLatched();
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
    const uint64_t value = m.done.Next();
    if (SUCCEEDED(m.queue->Signal(m.doneFence12.Get(), value)))
        m.doneBroken = false;
}

// Resets the shared list on an allocator whose earlier work finished. False: none is free (not an error).
bool BeginList()
{
    Module& m = M();
    for (size_t i = 0; i < kAllocators; ++i)
    {
        const size_t index = (m.allocatorNext + i) % kAllocators;
        if (!GpuDone(m.allocatorValue[index]))
            continue;
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
    value = m.done.Next();
    if (SUCCEEDED(m.queue->Signal(m.doneFence12.Get(), value)))
        return true;
    m.doneBroken = true;
    return false;
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

bool CreateStreamGpu(Stream& s, uint32_t w, uint32_t h, DXGI_FORMAT nativeFormat, bool late)
{
    s.width = w;
    s.height = h;
    s.late = late; // decided here, for the life of the stream
    if (!CreateShared("CharaNr colour", w, h, nativeFormat, D3D11_BIND_SHADER_RESOURCE, false, false, s.colour[0]) ||
        (late && !CreateShared("CharaNr colour 1", w, h, nativeFormat, D3D11_BIND_SHADER_RESOURCE, false, false, s.colour[1])) ||
        !CreateShared("CharaNr result 0", w, h, kResultFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, true, false, s.result[0]) ||
        !CreateShared("CharaNr result 1", w, h, kResultFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, true, false, s.result[1]) ||
        !CreateShared("CharaNr depth", w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, false, true, s.depth) ||
        !CreateShared("CharaNr motion", w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, false, false, s.motion) ||
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
    s = Stream {};
}

void UpdateHolds()
{
    Module& m = M();
    m.holds.store(m.inited || m.table.UsedCount() != 0, std::memory_order_relaxed);
    m.streamCount.store(static_cast<uint32_t>(m.table.UsedCount()), std::memory_order_relaxed);
}

void ReleaseGlobal()
{
    Module& m = M();
    m.recorder.reset();
    m.timer.reset();
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
    m.vramAddedMb.store(0, std::memory_order_relaxed);
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
    if (!lists || m.recorder == nullptr || !m.recorder->IsInit())
    {
        Latch("the command lists or the compute pass could not be created");
        ReleaseGlobal();
        return false;
    }
    m.inited = true;
    UpdateHolds();
    LOG_INFO("CharaView NR: ready on the bridge's D3D12 device; model {}; own fence and {} command allocators", std::filesystem::path(m.backend.modelPath).string(), kAllocators);
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
            _m.timer->Start(_list);
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
    FramePlan plan;
    int buffer = 0;
    double cpuMs = 0.0;

    // The late variant overwrites a colour image the run before last read: the D3D11 queue first waits for that run (done fence).
    bool WaitColourFree(uint64_t v) { return SUCCEEDED(m.ctx4->Wait(m.doneFence11.Get(), v)); }
    // The extra copy of the bring-up: native image -> shareable colour image.
    void CopyColour() { ctx->CopyResource(s.colour[plan.colour == Tex::Colour1 ? 1 : 0].t11(), native); }
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
        return RunOutcome::Skipped;
    const DlssNr::PreviewBackend& b = m.backend;
    const DlssNr::PreviewTuning t = DlssNr::CurrentPreviewTuning();
    s.vramBefore = VramUsedBytes();
    const auto start = std::chrono::steady_clock::now();
    {
        DlssNr::PreviewEvaluateScope serialised;
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
        Latch("the model would not initialise");
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
    const uint64_t after = VramUsedBytes();
    LOG_INFO("CharaView NR: model instance {} created for {}x{} in {:.1f} ms (preset {}, style {}); video memory in use {} MB -> {} MB",
             index, s.width, s.height, createMs, t.preset, t.style, s.vramBefore >> 20, after >> 20);
    return RunOutcome::Preparing;
}

void NoteFeatureReady(Stream& s, int index)
{
    if (s.readyLogged)
        return;
    s.readyLogged = true;
    Module& m = M();
    const uint64_t used = VramUsedBytes();
    const uint64_t added = used > s.vramBefore ? (used - s.vramBefore) >> 20 : 0;
    m.vramAddedMb.fetch_add(added, std::memory_order_relaxed);
    LOG_INFO("CharaView NR: model instance {} ready after {:.0f} ms; video memory in use {} MB ({} MB more than before it was created)", index,
             MsSince(s.created), used >> 20, added);
}

void LogCostIfDue(uint64_t now)
{
    Module& m = M();
    if (!m.cpuCost.Due(now))
        return;
    const auto cpu = m.cpuCost.Take(now);
    const auto gpu = m.gpuCost.Take(now);
    if (gpu.count != 0)
        LOG_INFO("CharaView NR cost: {} runs, GPU {:.2f} ms average ({:.2f} max), CPU recording {:.2f} ms average ({:.2f} max); {} stream(s)", cpu.count,
                 gpu.average, gpu.max, cpu.average, cpu.max, m.table.UsedCount());
    else
        LOG_INFO("CharaView NR cost: {} runs, CPU recording {:.2f} ms average ({:.2f} max); GPU time not measured yet", cpu.count, cpu.average, cpu.max);
}

RunOutcome Fallback(RunOutcome why)
{
    M().fallbacks.fetch_add(1, std::memory_order_relaxed);
    return why;
}

RunOutcome Run(int index, Stream& s, const RunRequest& req)
{
    Module& m = M();
    ReplayScope own; // the copy and clear below go through the hooked context: they are not the game's work
    StreamState& st = m.table.At(index);

    const bool late = s.late; // decided when the stream was created: the run and its consumer always agree
    FramePlan plan;
    plan.width = s.width;
    plan.height = s.height;
    plan.initGuides = !s.guidesInit;
    const int resultBuffer = ResultBufferForRun(st.runs);
    plan.result = resultBuffer == 0 ? Tex::Result0 : Tex::Result1;                 // the result image this run writes
    plan.colour = (late && resultBuffer == 1) ? Tex::Colour1 : Tex::Colour0;       // the late variant alternates the colour images too
    const ResetReason reset = DecideReset(st.everRan, st.lastRun, req.now);
    plan.reset = reset != ResetReason::None;
    plan.settings = SettingsFromConfig();
    plan.tuning = TuningFromBackend();

    // A free command allocator is needed before anything is queued: none free (many windows open, a slow GPU) is not an error, the
    // native image stays for this render.
    if (!BeginList())
        return Fallback(RunOutcome::Skipped);

    ID3D11DeviceContext* ctx = req.context;
    if (plan.initGuides)
    {
        const FLOAT farPlane[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // the bring-up depth: everything at the far plane, not inverted
        ctx->ClearRenderTargetView(s.depth.rtv.Get(), farPlane);
    }

    const int buffer = resultBuffer;
    HandoffOps ops { m, s, st, ctx, req.native, plan, buffer };
    const HandoffOutcome out = RunFrame(ops, m.ready, late, s.doneValue[buffer]);
    if (!out.ok)
    {
        if (out.failedAt == HandoffStep::ReuseWait || out.failedAt == HandoffStep::Signal11 || out.failedAt == HandoffStep::Wait12)
            m.list->Close(); // the list was reset for this run but never recorded
        char text[96];
        std::snprintf(text, sizeof(text), "a run failed at the %s step", HandoffStepName(out.failedAt));
        Latch(text);
        return Fallback(RunOutcome::Failed);
    }

    s.doneValue[buffer] = out.done;
    s.guidesInit = true;
    st.everRan = true;
    st.lastRun = req.now;
    st.ranAttempt = st.attempts; // this bring-up call produced the result that is there now
    ++st.runs;
    st.chain = reset != ResetReason::None ? 1 : st.chain + 1; // the late variant shows the run before the newest only inside one chain
    m.runs.fetch_add(1, std::memory_order_relaxed);
    m.lastCpuUs.store(static_cast<uint64_t>(ops.cpuMs * 1000.0), std::memory_order_relaxed);
    m.cpuCost.Add(ops.cpuMs);
    if (m.timer != nullptr)
    {
        if (const auto gpuMs = m.timer->ReadGpuTime(m.queue); gpuMs.has_value())
        {
            m.lastGpuUs.store(static_cast<uint64_t>(*gpuMs * 1000.0), std::memory_order_relaxed);
            m.gpuCost.Add(*gpuMs);
        }
    }
    if (st.runs == 1)
        LOG_INFO("CharaView NR: stream {} first run {}x{} ({}, history {})", index, s.width, s.height, late ? "late variant" : "same frame",
                 ResetReasonText(reset));
    LogCostIfDue(req.now);
    return RunOutcome::Ran;
}
} // namespace

bool Wanted() { return ext::Cfg().ffxiv.chara.previewNr.value_or_default(); }
bool Latched() { return M().latches.NrLatched(); }
bool Available() { return static_cast<Availability>(M().availabilitySnapshot.load(std::memory_order_relaxed)) == Availability::Available; }
bool HoldsResources() { return M().holds.load(std::memory_order_relaxed); }
Latches& SharedLatches() { return M().latches; }

RunOutcome RunBringUp(const RunRequest& req)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (req.context == nullptr || req.native == nullptr || req.width == 0 || req.height == 0)
        return Fallback(RunOutcome::Skipped);

    // Every call that reaches a preview that has a stream is an attempt. The result in the stream is current only while the newest
    // attempt produced it: a render that was skipped, still preparing or failed leaves the older result stale, and a consumer draw must
    // not show it for this render (ResultFor).
    const uintptr_t key = reinterpret_cast<uintptr_t>(req.native);
    if (const int known = m.table.Find(key); known != kNoStream)
        ++m.table.At(known).attempts;

    if (Refresh() != Availability::Available || !EnsureInit(req.context))
        return Fallback(RunOutcome::NotAvailable);

    int index = m.table.Bind(key, req.width, req.height, req.now);
    if (index == kNoStream)
    {
        if (!m.tableFullLogged)
        {
            m.tableFullLogged = true;
            LOG_INFO("CharaView NR: all {} preview NR streams are in use (open windows and windows still lingering); further previews keep the "
                     "native image until one is released",
                     kMaxStreams);
        }
        return Fallback(RunOutcome::Skipped);
    }
    if (m.table.At(index).attempts == 0)
        m.table.At(index).attempts = 1; // a stream Bind just created

    // What the stream has to be built for. A change (size, the model preset or style, the hand-off variant) must hold still for a
    // moment before the stream is rebuilt: dragging a window to a new size reports a new size every frame, and each one would
    // otherwise be a new model.
    const DlssNr::PreviewTuning tuning = DlssNr::CurrentPreviewTuning();
    StreamShape wanted;
    wanted.width = req.width;
    wanted.height = req.height;
    wanted.preset = tuning.preset;
    wanted.style = tuning.style;
    wanted.late = Late();
    m.table.At(index).lastLive = req.now;
    switch (DecideRebuild(m.table.At(index), wanted, req.now))
    {
    case RebuildAction::Settle: UpdateHolds(); return Fallback(RunOutcome::Skipped);
    case RebuildAction::Rebuild:
    {
        // The old stream goes once the GPU is finished with it.
        if (!GpuDone(m.table.At(index).lastSubmitted))
            return Fallback(RunOutcome::Skipped);
        const StreamShape old = m.table.At(index).built;
        LOG_INFO("CharaView NR: stream {} rebuilt: {}x{} -> {}x{}, preset {} -> {}, style {} -> {}, {} -> {}", index, old.width, old.height,
                 wanted.width, wanted.height, old.preset, wanted.preset, old.style, wanted.style, old.late ? "late" : "same frame",
                 wanted.late ? "late" : "same frame");
        ReleaseStreamObjects(m.streams[index], true);
        m.table.Free(index);
        index = m.table.Bind(key, req.width, req.height, req.now);
        if (index == kNoStream)
            return Fallback(RunOutcome::Skipped);
        m.table.At(index).attempts = 1;
        m.table.At(index).lastLive = req.now;
        break;
    }
    case RebuildAction::Keep: break;
    }
    Stream& s = m.streams[index];
    StreamState& st = m.table.At(index);
    UpdateHolds();

    if (!s.gpuObjects)
    {
        D3D11_TEXTURE2D_DESC nd {};
        req.native->GetDesc(&nd);
        if (nd.Format != DXGI_FORMAT_B8G8R8A8_UNORM || !CreateStreamGpu(s, req.width, req.height, nd.Format, wanted.late))
        {
            Latch(nd.Format != DXGI_FORMAT_B8G8R8A8_UNORM ? "the preview image is not B8G8R8A8_UNORM" : "the shared images could not be created");
            return Fallback(RunOutcome::Failed);
        }
        st.built = wanted;
        st.hasBuilt = true;
    }
    if (s.feature == nullptr)
        return Fallback(CreateFeatureStep(index, s, st));
    if (!s.featureReady)
    {
        if (!GpuDone(s.createDone))
            return Fallback(RunOutcome::Preparing);
        s.featureReady = true;
        NoteFeatureReady(s, index);
        return Fallback(RunOutcome::Preparing); // the first evaluation is on the next render
    }
    const RunOutcome outcome = Run(index, s, req);
    UpdateHolds();
    return outcome == RunOutcome::Ran ? outcome : Fallback(outcome);
}

RunOutcome RunProduction(const RunRequest&)
{
    // Not implemented yet. Planned position: between the super-resolution evaluate and the alpha pass (CharaDlss.cpp RunSlot), with the
    // SR output as the colour image and the real depth and motion guides, writing the result image that the alpha pass reads.
    return RunOutcome::NotImplemented;
}

// The consumer's side of the hand-off: the late variant waits (GPU side) for the run that wrote the image it is about to read.
struct ConsumerOps
{
    Module& m;
    bool WaitResult(uint64_t done) { return SUCCEEDED(m.ctx4->Wait(m.doneFence11.Get(), done)); }
};

ID3D11ShaderResourceView* ResultFor(ID3D11DeviceContext* /*context*/, uintptr_t key, uint64_t now)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (!m.inited || m.availability != Availability::Available)
        return nullptr;
    const int index = m.table.Find(key);
    if (index == kNoStream)
        return nullptr;
    const StreamState& st = m.table.At(index);
    // Only the result of the newest bring-up call, and only while it is recent: a skipped, preparing or failed render has no result.
    if (!st.ResultIsCurrent() || now < st.lastRun || now - st.lastRun > kLiveMs)
        return nullptr;
    Stream& s = m.streams[index];
    const int buffer = ConsumerBuffer(s.late, st.runs, st.chain);
    if (buffer < 0)
        return nullptr;
    ConsumerOps ops { m };
    if (!ConsumerWait(ops, s.late, s.doneValue[buffer]))
    {
        Latch("the D3D11 wait on the previous result failed");
        return nullptr;
    }
    return s.result[buffer].srv.Get();
}

ID3D11Texture2D* ResultTexture(uint64_t now)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (!m.inited || m.availability != Availability::Available)
        return nullptr;
    int best = kNoStream;
    for (size_t i = 0; i < kMaxStreams; ++i)
    {
        const StreamState& st = m.table.At(static_cast<int>(i));
        if (!st.Used() || !st.ResultIsCurrent() || now < st.lastRun || now - st.lastRun > kLiveMs)
            continue;
        if (best == kNoStream || st.lastRun > m.table.At(best).lastRun)
            best = static_cast<int>(i);
    }
    if (best == kNoStream)
        return nullptr;
    const StreamState& st = m.table.At(best);
    const int buffer = ConsumerBuffer(m.streams[best].late, st.runs, st.chain);
    return buffer < 0 ? nullptr : m.streams[best].result[buffer].t11();
}

void Maintain(uint64_t now)
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (now >= m.lastMaintain && now - m.lastMaintain < 100)
        return;
    m.lastMaintain = now;

    // The objects of a previous bridge device that the GPU was still using when it changed: released as soon as it is done.
    if (m.releasePending && ReleaseAll(false))
        LOG_INFO("CharaView NR: the objects of the previous graphics device were released");

    const Availability a = Refresh(); // also with nothing created: the dispatcher reads the cached result (Available())
    if (!m.inited && m.table.UsedCount() == 0)
        return;
    const bool disabled = a != Availability::Available;
    const uint64_t linger = LingerMs(ext::Cfg().ffxiv.chara.previewNrLingerSeconds.value_or_default());
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

void OnBridgeShutdown()
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

void OnBridgeInit()
{
    Module& m = M();
    std::lock_guard lock(m.mutex);
    m.holdOff = false;
}

void Detach()
{
    // Draw thread, device valid, the hooks are about to go. No wait here (the earlier version waited up to five seconds on a fence event
    // with the module locked, which is unsafe if this ever ran under the loader lock): the D3D11 context is flushed so that signals the
    // D3D12 queue may be waiting for are on their way, and whatever the GPU has finished is released. What it is still using stays; the
    // bridge shutdown (OnBridgeShutdown, which drains) or the device teardown takes it. The super-resolution path does no GPU wait in
    // its Detach either.
    Module& m = M();
    std::lock_guard lock(m.mutex);
    if (m.ctx4 != nullptr)
        m.ctx4->Flush();
    if (!ReleaseAll(false))
    {
        m.releasePending = true;
        UpdateHolds();
        LOG_INFO("CharaView NR: the GPU was still using preview NR objects when the hooks were removed; they are released later");
    }
}

Status GetStatus()
{
    Module& m = M();
    Status s;
    {
        std::lock_guard lock(m.mutex);
        const Availability a = Evaluate(Facts());
        s.availability = a;
        s.reason = ReasonText(a);
    }
    s.streams = m.streamCount.load(std::memory_order_relaxed);
    s.liveStreams = m.liveCount.load(std::memory_order_relaxed);
    s.runs = m.runs.load(std::memory_order_relaxed);
    s.fallbacks = m.fallbacks.load(std::memory_order_relaxed);
    s.lastGpuMs = static_cast<double>(m.lastGpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.lastCpuMs = static_cast<double>(m.lastCpuUs.load(std::memory_order_relaxed)) / 1000.0;
    s.videoMemoryMb = m.vramAddedMb.load(std::memory_order_relaxed);
    s.late = Late();
    return s;
}
} // namespace ext::ffxiv::chara::nr
