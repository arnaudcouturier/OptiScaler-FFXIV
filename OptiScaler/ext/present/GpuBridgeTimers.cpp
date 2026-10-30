#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/dx11bridge/BridgeAccess.h"
#include "ext/nr/DlssNr.h"
#include "ext/present/GpuTimerStats.h"
#include "ext/reshade/ReShadeBoundaryCore.h"
#include "ext/reshade/ReShadeD3D11.h"
#include "ext/reshade/ReShadeProbe.h"

#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <State.h>

#include <framegen/IFGFeature_Dx12.h>

#include <d3d11_1.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

// GPU stage timers ([FramePacing] GpuTimers; seams ext/present/PresentSeams.h, rules GpuTimerStats.h).
//
// What runs where (all on the render thread; the menu thread only reads the published on/off flag and the published last samples):
//   GpuTimersBeginFrame / GpuTimersOnPresent: the bridge PresentScope (BridgeHdr.cpp).
//   Begin/EndCopyIn: BeforeSharedCopy / AfterSharedCopy around the present copy-in.
//   Begin/End/CancelInteropCopy: ConvertCopySource / PrepareScreenshot around the D3D12 interop copy (a failed
//     conversion closes its list without executing it, so the slot is forgotten instead of closed).
//   begin/finish-effects handlers below: ReShade's own end-of-frame pass (the fork's own render_effects calls, the
//     diagnostic probe's and the live in-place preset pass, are left out by their call guards).
// The on/off decision is published to an atomic at Present exit; the row functions read that atomic and never run
// the trace decision (which can log) from the menu thread. Logging (decision announcements, one-warning creation
// failures, 10 s lines) always happens without TimerMutex held. Named ranges (D3D11 annotations, D3D12 list markers)
// ride the same gate, so captures stay clean unless the timers sample.
namespace ext::present
{
namespace
{
using namespace gputimers;
using Microsoft::WRL::ComPtr;

std::mutex& TimerMutex()
{
    static std::mutex m;
    return m;
}

std::atomic<uint64_t>& FrameCounter()
{
    static std::atomic<uint64_t> f { 0 };
    return f;
}

// The on/off decision of the last Present exit. The render thread publishes it, the menu rows read it with a plain
// load, so the menu never runs the trace decision (which announces changes to the log) while holding TimerMutex.
std::atomic<bool>& PublishedOn()
{
    static std::atomic<bool> on { false };
    return on;
}

// The last sample of each logged stage, published at Present exit. The render thread stores them under
// TimerMutex; the menu rows load them with plain atomic loads, so the menu never takes TimerMutex.
// A negative value means no sample yet (durations are never negative).
std::atomic<double>& PublishedCopyInMs()
{
    static std::atomic<double> ms { -1.0 };
    return ms;
}
std::atomic<double>& PublishedEffectsMs()
{
    static std::atomic<double> ms { -1.0 };
    return ms;
}
std::atomic<double>& PublishedInteropMs()
{
    static std::atomic<double> ms { -1.0 };
    return ms;
}

bool IsFfxivExe()
{
    const std::string& exe = State::Instance().gameExe;
    return !exe.empty() && _stricmp(exe.c_str(), "ffxiv_dx11.exe") == 0;
}

int64_t NowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The CPU side of the clock calibration: ID3D12CommandQueue::GetClockCalibration reports its CPU timestamp on the
// QueryPerformanceCounter clock, so the two can be subtracted. The frequency is read once (it never changes).
std::uint64_t QpcTicks()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return static_cast<std::uint64_t>(v.QuadPart);
}

std::uint64_t QpcFrequency()
{
    static const std::uint64_t frequency = []
    {
        LARGE_INTEGER v {};
        QueryPerformanceFrequency(&v);
        return static_cast<std::uint64_t>(v.QuadPart);
    }();
    return frequency;
}

bool TimersOn() { return IsFfxivExe() && ResolveGpuTimers(ext::Cfg().framePacing.GpuTimersKey(), TraceIsOn()); }

// ---- D3D11 timestamp stage (present copy-in, ReShade effects): disjoint + start/end queries, read at Present ----
struct D3D11Stage
{
    static constexpr unsigned kRing = 4; // slot = frame % 16 while sampling, so frames 0..3 of the cycle
    ComPtr<ID3D11Query> disjoint[kRing], begin[kRing], end[kRing];
    ComPtr<ID3D11Device> device;
    ComPtr<ID3DUserDefinedAnnotation> annotation;
    ID3D11DeviceContext* annotationCtx = nullptr;
    ID3D11DeviceContext* createCtx = nullptr; // the context the queries and the annotation were created on
    ID3D11DeviceContext* lastCtx = nullptr;
    SlotBook book; // one pending pair per slot: the first writer of a frame wins, a later frame reuses the slot
    bool open = false;
    unsigned openSlot = 0;
    ID3D11DeviceContext* openCtx = nullptr;
    uint64_t openRuntime = 0; // effects only: the runtime whose span is open (a second runtime keeps out)
    bool dead = false;
    WarnLatch warn;
    std::optional<double> lastMs;
    GpuWindow window; // the 10 s reporting window
};

static_assert(D3D11Stage::kRing == kSampledSlots, "the D3D11 ring holds exactly the sampled frames of a cycle");

struct D3D12Copy
{
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    const UINT64* mapped = nullptr;
    UINT64 frequency = 0;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence; // the bridge copy fence: completion is read, never waited on
    Dx11wDx12SC* bridge = nullptr; // the bridge of this Present (same object from the copy calls to Present exit)
    SlotBook book; // per-slot pending state: a slot reports once its resolve retired, then exactly once
    // The GPU/CPU clock pairing of the samples (GetClockCalibration, resampled once per reporting window) and, per timer
    // slot, the CPU read taken when that slot's pair was submitted: together they place a GPU begin stamp on the CPU clock.
    ClockCalibration cal {};
    std::optional<int64_t> calUs {};
    std::array<std::uint64_t, SlotBook::kSlots> armCpuTicks {};
    bool dead = false;
    WarnLatch warn;
    bool open = false;
    unsigned openSlot = 0;
    unsigned openCopySlot = 0; // the copy list the open pair is recorded into
    std::optional<double> lastMs;
    GpuWindow window; // the 10 s reporting window
};

D3D11Stage& CopyInStage()
{
    static D3D11Stage s;
    return s;
}
D3D11Stage& EffectsStage()
{
    static D3D11Stage s;
    return s;
}
D3D12Copy& InteropStage()
{
    static D3D12Copy c;
    return c;
}

// Release every device-bound object of a D3D11 stage: a new device restarts clean (with its own warning on failure).
void DropD3D11(D3D11Stage& t)
{
    for (unsigned i = 0; i < D3D11Stage::kRing; ++i)
    {
        t.disjoint[i].Reset();
        t.begin[i].Reset();
        t.end[i].Reset();
    }
    t.annotation.Reset();
    t.annotationCtx = nullptr;
    t.createCtx = nullptr;
    t.lastCtx = nullptr;
    t.openCtx = nullptr;
    t.device.Reset();
    t.open = false;
    t.openSlot = 0;
    t.openRuntime = 0;
    t.book.Clear();
    t.lastMs.reset();
    t.window.Reset();
    t.dead = false;
    t.warn.Reset();
}

// Release every device-bound object of the interop stage, ending the persistent readback map. A new device, queue,
// fence or bridge restarts clean (with its own warning on failure).
void DropD3D12(D3D12Copy& t)
{
    if (t.mapped)
    {
        t.readback->Unmap(0, nullptr);
        t.mapped = nullptr;
    }
    t.heap.Reset();
    t.readback.Reset();
    t.frequency = 0;
    t.device.Reset();
    t.queue.Reset();
    t.fence.Reset();
    t.bridge = nullptr;
    t.cal = {};
    t.calUs.reset();
    t.armCpuTicks = {};
    t.open = false;
    t.openSlot = 0;
    t.openCopySlot = 0;
    t.book.Clear();
    t.lastMs.reset();
    t.window.Reset();
    t.dead = false;
    t.warn.Reset();
}

// True with working queries. Every creation failure latches the stage off and hands out one warning (returned, so
// the caller logs it without holding TimerMutex). A new device drops the stage first and retries with a fresh latch.
bool EnsureD3D11(D3D11Stage& t, ID3D11DeviceContext* ctx, const char* what, std::string& warn)
{
    warn.clear();
    if (ctx == nullptr)
    {
        t.dead = true;
        if (t.warn.Claim())
            warn = std::string("Frame pacing GPU timers: no device context for ") + what + ", that stage stays off";
        return false;
    }
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    if (!device)
    {
        t.dead = true;
        if (t.warn.Claim())
            warn = std::string("Frame pacing GPU timers: no device behind the context for ") + what + ", that stage stays off";
        return false;
    }
    if (t.device && t.device.Get() != device.Get())
        DropD3D11(t);
    if (t.dead)
        return false;
    if (t.device)
    {
        // Same device, another context: the queries stay valid, only the capture annotation follows the context.
        if (ctx != t.createCtx)
        {
            t.annotation.Reset();
            t.annotationCtx = nullptr;
            ComPtr<ID3DUserDefinedAnnotation> annotation;
            if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&annotation))) && annotation)
            {
                t.annotation = annotation;
                t.annotationCtx = ctx;
            }
            t.createCtx = ctx;
        }
        return true;
    }
    D3D11_QUERY_DESC disjointDesc {};
    disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC stampDesc {};
    stampDesc.Query = D3D11_QUERY_TIMESTAMP;
    for (unsigned i = 0; i < D3D11Stage::kRing; ++i)
    {
        if (FAILED(device->CreateQuery(&disjointDesc, &t.disjoint[i])) ||
            FAILED(device->CreateQuery(&stampDesc, &t.begin[i])) ||
            FAILED(device->CreateQuery(&stampDesc, &t.end[i])))
        {
            for (unsigned j = 0; j < D3D11Stage::kRing; ++j)
            {
                t.disjoint[j].Reset();
                t.begin[j].Reset();
                t.end[j].Reset();
            }
            t.dead = true;
            if (t.warn.Claim())
                warn = std::string("Frame pacing GPU timers: ") + what + " timestamp queries failed, that stage stays off";
            return false;
        }
    }
    // Best effort: named ranges for captures. No warning when the context offers none.
    ComPtr<ID3DUserDefinedAnnotation> annotation;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&annotation))) && annotation)
    {
        t.annotation = annotation;
        t.annotationCtx = ctx;
    }
    t.createCtx = ctx;
    t.device = device;
    return true;
}

const wchar_t* StageRange(GpuStage stage)
{
    switch (stage)
    {
    case GpuStage::PresentCopyIn:
        return L"present copy-in";
    case GpuStage::ReshadeEffects:
        return L"ReShade effects";
    case GpuStage::InteropCopy:
        return L"interop copy";
    case GpuStage::Count:
        break;
    }
    return L"?";
}

void AnnotateBegin(ID3D11DeviceContext* ctx, D3D11Stage& t, GpuStage stage)
{
    if (t.annotation && ctx == t.annotationCtx)
        t.annotation->BeginEvent(StageRange(stage));
}

void AnnotateEnd(ID3D11DeviceContext* ctx, D3D11Stage& t)
{
    if (t.annotation && ctx == t.annotationCtx)
        t.annotation->EndEvent();
}

// True with a query heap and a mapped readback. Same latch-every-failure rule as the D3D11 stages; a changed device,
// queue or fence drops the stage first and retries with a fresh latch. A new bridge is detected through those three:
// the stage holds its own references to them, so a new swapchain can never reuse their addresses. The one-time map
// at creation is the only Map of the slice: the frame path only reads the persistently mapped memory.
bool EnsureD3D12(Dx11wDx12SC& sc, std::string& warn)
{
    D3D12Copy& t = InteropStage();
    warn.clear();
    auto* device = Access::Bridge::Dx12Device(sc);
    auto* queue = Access::Bridge::CopyQueue(sc);
    auto* fence = Access::Bridge::CopyFence(sc);
    if (t.device && (t.device.Get() != device || t.queue.Get() != queue || t.fence.Get() != fence))
        DropD3D12(t);
    if (t.dead)
        return false;
    if (!device || !queue || !fence)
    {
        t.dead = true;
        if (t.warn.Claim())
            warn = "Frame pacing GPU timers: the interop copy has no device, queue or fence, that stage stays off";
        return false;
    }
    if (t.device && t.mapped && t.frequency)
        return true;
    UINT64 frequency = 0;
    if (FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency)
    {
        t.dead = true;
        if (t.warn.Claim())
            warn = "Frame pacing GPU timers: the interop copy timestamp frequency failed, that stage stays off";
        return false;
    }
    D3D12_QUERY_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kTimerSlots * 2;
    ComPtr<ID3D12QueryHeap> heap;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap))) || !heap)
    {
        t.dead = true;
        if (t.warn.Claim())
            warn = "Frame pacing GPU timers: interop copy query heap failed, that stage stays off";
        return false;
    }
    D3D12_RESOURCE_DESC bufDesc {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kTimerSlots * 2 * sizeof(UINT64);
    bufDesc.Height = bufDesc.DepthOrArraySize = bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heapProps {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    void* mapped = nullptr;
    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback))) ||
        !readback || FAILED(readback->Map(0, nullptr, &mapped)) || !mapped)
    {
        t.dead = true;
        if (t.warn.Claim())
            warn = "Frame pacing GPU timers: interop copy readback failed, that stage stays off";
        return false;
    }
    std::memset(mapped, 0, kTimerSlots * 2 * sizeof(UINT64)); // fresh resource, no GPU work references it yet
    t.heap = heap;
    t.readback = readback;
    t.mapped = static_cast<const UINT64*>(mapped);
    t.frequency = frequency;
    t.device = device;
    t.queue = queue;
    t.fence = fence;
    t.bridge = &sc;
    return true;
}

// Samples the CPU and GPU timestamp counters together with one GetClockCalibration call, at most once per reporting
// window (10 s; the pairing is stable over that time, and resampling it per frame would buy nothing). A refused or
// invalid sample keeps the last good pairing instead of clearing it, and never warns: the stage still logs its
// durations, only the CPU-time offset field is left out of the line.
bool RefreshCalibration(D3D12Copy& t, int64_t nowUs)
{
    if (!CalibrationDue(nowUs, t.calUs))
        return t.cal.gpuFrequency != 0;
    t.calUs = nowUs;
    if (!t.queue)
        return false;
    ClockCalibration sample;
    sample.gpuFrequency = t.frequency;
    sample.cpuFrequency = QpcFrequency();
    if (FAILED(t.queue->GetClockCalibration(&sample.gpuTimestamp, &sample.cpuTimestamp)) || !CalibrationValid(sample))
        return t.cal.gpuFrequency != 0;
    t.cal = sample;
    return true;
}

// DONOTFLUSH reads of one stage. A query the GPU has not reached stays pending for the next Present: no wait,
// no flush. A disjoint window reads empty once (frequency lost) and is dropped.
void ReadD3D11(D3D11Stage& t, int64_t nowUs)
{
    if (!t.device || !t.lastCtx)
        return;
    for (unsigned i = 0; i < D3D11Stage::kRing; ++i)
    {
        if (!t.book.Pending(i))
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
        if (t.lastCtx->GetData(t.disjoint[i].Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        if (disjoint.Disjoint || !disjoint.Frequency)
        {
            t.book.Consume(i);
            continue;
        }
        UINT64 b = 0, e = 0;
        if (t.lastCtx->GetData(t.begin[i].Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            t.lastCtx->GetData(t.end[i].Get(), &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || e < b)
            continue; // the stamps are not readable yet: the pair stays pending, the sample is kept
        t.book.Consume(i);
        const double ms = static_cast<double>(e - b) * 1000.0 / static_cast<double>(disjoint.Frequency);
        t.lastMs = ms;
        t.window.Add(ms, nowUs);
    }
}

std::optional<double> InteropMs(UINT64 b, UINT64 e, UINT64 frequency)
{
    if (!b || e < b || !frequency)
        return std::nullopt;
    return static_cast<double>(e - b) * 1000.0 / static_cast<double>(frequency);
}

// One slot reports when the fence proves its resolve finished and retired: the covering value (read from the recorded
// list, so a later execution on the same list only delays, never invents) exceeds the recorded one, and the fence
// completed value has passed the covering one. A half-written pair (resolve still in flight) stays pending; every
// value is consumed on report, so each reports exactly once. No wait, no event, no flush: GetCompletedValue is a load.
void ReadD3D12(int64_t nowUs)
{
    D3D12Copy& t = InteropStage();
    if (!t.device || !t.mapped || !t.fence || !t.frequency || t.bridge == nullptr)
        return;
    if (Access::Bridge::Dx12Device(*t.bridge) != t.device.Get() || Access::Bridge::CopyFence(*t.bridge) != t.fence.Get())
    {
        DropD3D12(t); // interop was torn down under the stage: forget it, a later Begin recreates
        return;
    }
    const UINT64 completed = t.fence->GetCompletedValue();
    for (unsigned s = 0; s < SlotBook::kSlots; ++s)
    {
        if (!t.book.Pending(s))
            continue;
        if (!t.book.Ready(s, completed, Access::Bridge::CopyAllocatorFenceValue(*t.bridge, t.book.Tag(s))))
            continue;
        const unsigned q = s * 2;
        const auto ms = InteropMs(t.mapped[q], t.mapped[q + 1], t.frequency);
        // The pair's place on the CPU clock, against the CPU read taken when its list was closed. Without a
        // calibration this is empty and the line simply has no offset field.
        const auto offset = CpuOffsetMs(t.cal, t.mapped[q], t.armCpuTicks[s]);
        t.book.Consume(s);
        if (!ms.has_value())
            continue;
        t.lastMs = ms;
        t.window.Add(*ms, nowUs, offset);
    }
}

// Forget one D3D11 stage on the way off: no pending slot reports, no window reopens, no row shows after this.
void ClearD3D11(D3D11Stage& t)
{
    t.book.Clear();
    t.open = false;
    t.openSlot = 0;
    t.openRuntime = 0;
    t.openCtx = nullptr;
    t.lastMs.reset();
    t.window.Reset();
}

void ClearD3D12()
{
    D3D12Copy& t = InteropStage();
    t.book.Clear();
    t.cal = {}; // the pairing belongs to the device and queue that were dropped, and no offset outlives the timers
    t.calUs.reset();
    t.armCpuTicks = {};
    t.open = false;
    t.openSlot = 0;
    t.openCopySlot = 0;
    t.lastMs.reset();
    t.window.Reset();
}

} // namespace

void GpuTimersBeginFrame()
{
    FrameCounter().fetch_add(1, std::memory_order_relaxed);
}

void GpuTimersOnPresent()
{
    const int64_t nowUs = NowUs();
    const bool on = TimersOn(); // the decision may announce (log) on change: no lock held here
    PublishedOn().store(on, std::memory_order_relaxed);
    struct DueLine
    {
        GpuStage stage;
        GpuSummary summary;
    };
    DueLine due[static_cast<unsigned>(GpuStage::Count)];
    size_t dueCount = 0;
    {
        std::scoped_lock lock(TimerMutex());
        if (!on)
        {
            ClearD3D11(CopyInStage());
            ClearD3D11(EffectsStage());
            ClearD3D12();
            // the menu rows read the published samples without the lock: a negative value means empty, so a cleared stage must not keep its last number.
            PublishedCopyInMs().store(-1.0, std::memory_order_relaxed);
            PublishedEffectsMs().store(-1.0, std::memory_order_relaxed);
            PublishedInteropMs().store(-1.0, std::memory_order_relaxed);
            return;
        }
        ReadD3D11(CopyInStage(), nowUs);
        ReadD3D11(EffectsStage(), nowUs);
        ReadD3D12(nowUs);
        PublishedCopyInMs().store(CopyInStage().lastMs.value_or(-1.0), std::memory_order_relaxed);
        PublishedEffectsMs().store(EffectsStage().lastMs.value_or(-1.0), std::memory_order_relaxed);
        PublishedInteropMs().store(InteropStage().lastMs.value_or(-1.0), std::memory_order_relaxed);
        // One 10 s log line per closed window that holds samples (a stage with no samples, like ReShade effects
        // without ReShade, stays silent instead of logging zeros every 10 s). Collected here, logged below without
        // the mutex: the present path never waits on log file I/O while holding it.
        const auto collect = [&](GpuStage stage, GpuWindow& window)
        {
            GpuSummary summary;
            if (window.CloseIfDue(nowUs, summary) && summary.samples > 0 &&
                dueCount < static_cast<unsigned>(GpuStage::Count))
                due[dueCount++] = { stage, summary };
        };
        collect(GpuStage::PresentCopyIn, CopyInStage().window);
        collect(GpuStage::ReshadeEffects, EffectsStage().window);
        collect(GpuStage::InteropCopy, InteropStage().window);
        // The bridge pointer is only valid for the Present that set it: a swapchain can be destroyed between two
        // presents, and a later Present that exits before the copy seams must not read through a stale pointer.
        InteropStage().bridge = nullptr;
    }
    for (size_t i = 0; i < dueCount; ++i)
        LOG_INFO("{}", FormatGpuLine(GpuStageName(due[i].stage), due[i].summary));
}

void GpuTimersBeginCopyIn(Dx11wDx12SC& sc)
{
    const uint64_t frame = FrameCounter().load(std::memory_order_relaxed);
    if (!ShouldSample(frame) || !TimersOn()) // the decision may log on change: no lock held
        return;
    auto* ctx = Access::Bridge::Dx11Context(sc);
    std::string warn;
    {
        std::unique_lock lock(TimerMutex());
        D3D11Stage& t = CopyInStage();
        const unsigned slot = static_cast<unsigned>(frame % kTimerSlots);
        if (EnsureD3D11(t, ctx, "present copy-in", warn) &&
            t.book.TryBegin(slot, frame, 0, 0, false) != SlotBook::Verdict::KeepOut)
        {
            t.book.Consume(slot); // a Drop verdict forgets the unfinished sample, then reuses the slot
            t.open = true;
            t.openSlot = slot;
            t.openCtx = ctx;
            t.lastCtx = ctx;
            t.book.Arm(slot, frame, 0, 0);
            AnnotateBegin(ctx, t, GpuStage::PresentCopyIn);
            ctx->Begin(t.disjoint[slot].Get());
            ctx->End(t.begin[slot].Get());
        }
    }
    if (!warn.empty())
        LOG_WARN("{}", warn);
}

void GpuTimersEndCopyIn(ID3D11DeviceContext* /*context*/)
{
    std::scoped_lock lock(TimerMutex());
    D3D11Stage& t = CopyInStage();
    if (!t.open)
        return;
    t.open = false;
    // A pair always ends on the context it was opened on; the seam argument only says that a copy happened.
    ID3D11DeviceContext* ctx = t.openCtx;
    if (!ctx || !t.device)
        return;
    AnnotateEnd(ctx, t);
    ctx->End(t.end[t.openSlot].Get());
    ctx->End(t.disjoint[t.openSlot].Get());
}

void GpuTimersBeginInteropCopy(Dx11wDx12SC& sc, unsigned slot)
{
    const uint64_t frame = FrameCounter().load(std::memory_order_relaxed);
    const bool on = TimersOn(); // the decision may log on change: no lock held
    const int64_t nowUs = NowUs();
    std::string warn;
    {
        std::unique_lock lock(TimerMutex());
        D3D12Copy& t = InteropStage();
        t.bridge = &sc; // the Present exit reads the fence through this; same Present, same bridge
        if (!ShouldSample(frame) || !on)
            return;
        auto* list = Access::Bridge::CopyList(sc, slot);
        if (list == nullptr || !EnsureD3D12(sc, warn))
            return;
        const unsigned s = static_cast<unsigned>(frame % kTimerSlots);
        RefreshCalibration(t, nowUs); // once per reporting window; a refusal keeps the last pairing
        // Reusing a slot whose old sample finished reports it first (recycle); an unfinished one is forgotten.
        if (t.book.TryBegin(s, frame, t.fence->GetCompletedValue(),
                            Access::Bridge::CopyAllocatorFenceValue(sc, t.book.Pending(s) ? t.book.Tag(s) : slot),
                            true) == SlotBook::Verdict::Recycle)
        {
            const unsigned q = s * 2;
            const auto ms = InteropMs(t.mapped[q], t.mapped[q + 1], t.frequency);
            if (ms.has_value())
            {
                t.lastMs = ms;
                t.window.Add(*ms, nowUs, CpuOffsetMs(t.cal, t.mapped[q], t.armCpuTicks[s]));
            }
        }
        t.book.Consume(s);
        t.open = true;
        t.openSlot = s;
        t.openCopySlot = slot;
        const unsigned q = s * 2;
        list->BeginEvent(0, L"interop copy", sizeof(L"interop copy"));
        list->EndQuery(t.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
    }
    if (!warn.empty())
        LOG_WARN("{}", warn);
}

void GpuTimersEndInteropCopy(Dx11wDx12SC& sc, unsigned slot)
{
    std::scoped_lock lock(TimerMutex());
    D3D12Copy& t = InteropStage();
    t.bridge = &sc;
    if (!t.open || !t.device)
        return;
    if (slot != t.openCopySlot)
    {
        // A different list than the pair was opened on: ending here would pair stamps from two executions.
        t.book.Cancel(t.openSlot);
        t.open = false;
        return;
    }
    auto* list = Access::Bridge::CopyList(sc, slot);
    if (list == nullptr)
    {
        t.book.Cancel(t.openSlot);
        t.open = false;
        return;
    }
    t.open = false;
    const unsigned q = t.openSlot * 2;
    list->EndQuery(t.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
    list->ResolveQueryData(t.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 2, t.readback.Get(), q * sizeof(UINT64));
    list->EndEvent();
    // The CPU read this pair is placed against: the submission of the copy the stamps belong to, taken here so the
    // offset never mixes two frames.
    t.armCpuTicks[t.openSlot] = QpcTicks();
    // The fence value already covering this list: only a later value proves the resolve finished.
    t.book.Arm(t.openSlot, FrameCounter().load(std::memory_order_relaxed),
               Access::Bridge::CopyAllocatorFenceValue(sc, slot), slot);
}

void GpuTimersCancelInteropCopy(Dx11wDx12SC& sc)
{
    std::scoped_lock lock(TimerMutex());
    D3D12Copy& t = InteropStage();
    t.bridge = &sc;
    if (!t.open)
        return;
    // The failed conversion closes its list without executing it: the recorded begin stamp never runs, so the slot
    // is forgotten instead of closed. A resolve recorded for it would read back a previous cycle as current.
    t.book.Cancel(t.openSlot);
    t.open = false;
}

namespace
{
// ---- ReShade begin/finish-effects handlers: ReShade's own end-of-frame pass on the native D3D11 context ----
using namespace ::reshade::api;

struct NativeJob
{
    effect_runtime* runtime;
    command_list* list;
    int api; // -1 until the thunk runs
    uint64_t native;
};

void NativeThunk(void* p)
{
    auto& j = *static_cast<NativeJob*>(p);
    j.api = -1;
    j.native = 0;
    if (j.runtime == nullptr || j.list == nullptr)
        return;
    device* dev = j.runtime->get_device();
    if (dev == nullptr)
        return;
    j.api = static_cast<int>(dev->get_api());
    j.native = j.list->get_native();
}

// The native immediate context, or null (a non-D3D11 runtime, or a ReShade call that faulted and was contained).
bool NativeD3D11Context(effect_runtime* runtime, command_list* cmd, ID3D11DeviceContext*& ctx)
{
    ctx = nullptr;
    NativeJob job { runtime, cmd, -1, 0 };
    if (ext::reshade::d3d11::GuardedCall(&NativeThunk, &job) != 0)
        return false;
    if (job.api != static_cast<int>(device_api::d3d11) || !job.native)
        return false;
    ctx = reinterpret_cast<ID3D11DeviceContext*>(job.native);
    return true;
}

void TimerBeginEffects(effect_runtime* runtime, command_list* cmd, resource_view, resource_view) noexcept
{
    try
    {
        const uint64_t frame = FrameCounter().load(std::memory_order_relaxed);
        if (!ShouldSample(frame) || !TimersOn())
            return;
        if (ext::reshade::probe::InProbeCall())
            return; // the diagnostic probe's own render_effects call: not ReShade's end-of-frame work
        if (ext::reshade::boundary::InLiveEffectsCall())
            return; // the live in-place preset pass: not ReShade's end-of-frame work either
        ID3D11DeviceContext* ctx = nullptr;
        if (!NativeD3D11Context(runtime, cmd, ctx) || !ctx)
            return;
        std::string warn;
        {
            std::unique_lock lock(TimerMutex());
            D3D11Stage& t = EffectsStage();
            const uint64_t id = reinterpret_cast<uint64_t>(runtime);
            if (t.open)
            {
                // A span of this frame is already open: the first runtime wins, this begin is not timed. A span
                // of an earlier frame is ended where it was opened (so the queries stay balanced) and its slot is
                // disarmed, then the new begin proceeds: one missed finish never blocks later frames.
                if (DecideEffectsBegin(true, t.book.Frame(t.openSlot), frame) == EffectsBegin::KeepOut)
                    return;
                if (ID3D11DeviceContext* oc = t.openCtx; oc && t.device)
                {
                    AnnotateEnd(oc, t);
                    oc->End(t.end[t.openSlot].Get());
                    oc->End(t.disjoint[t.openSlot].Get());
                }
                t.book.Cancel(t.openSlot);
                t.open = false;
                t.openRuntime = 0;
                t.openCtx = nullptr;
            }
            if (!EnsureD3D11(t, ctx, "ReShade effects", warn))
                return;
            const unsigned slot = static_cast<unsigned>(frame % kTimerSlots);
            if (t.book.TryBegin(slot, frame, 0, 0, false) == SlotBook::Verdict::KeepOut)
                return; // an unread sample of this frame waits: the first runtime wins, this one is not timed
            t.book.Consume(slot); // a Drop verdict forgets the unfinished sample, then reuses the slot
            t.open = true;
            t.openSlot = slot;
            t.openCtx = ctx;
            t.openRuntime = id;
            t.lastCtx = ctx;
            t.book.Arm(slot, frame, 0, 0);
            AnnotateBegin(ctx, t, GpuStage::ReshadeEffects);
            ctx->Begin(t.disjoint[slot].Get());
            ctx->End(t.begin[slot].Get());
        }
        if (!warn.empty())
            LOG_WARN("{}", warn);
    }
    catch (...)
    {
    }
}

void TimerEndEffects(effect_runtime* runtime, command_list*, resource_view, resource_view) noexcept
{
    try
    {
        std::scoped_lock lock(TimerMutex());
        D3D11Stage& t = EffectsStage();
        if (!t.open)
            return;
        if (t.openRuntime != reinterpret_cast<uint64_t>(runtime))
            return; // not the span we opened (its begin kept out while another runtime was open)
        t.open = false;
        // A pair always ends on the context it was opened on (a second device's finish event must not steal it).
        ID3D11DeviceContext* ctx = t.openCtx;
        if (!ctx || !t.device)
            return;
        AnnotateEnd(ctx, t);
        ctx->End(t.end[t.openSlot].Get());
        ctx->End(t.disjoint[t.openSlot].Get());
    }
    catch (...)
    {
    }
}
} // namespace

bool InstallGpuTimerEvents()
{
    try
    {
        using ::reshade::addon_event;
        ::reshade::register_event<addon_event::reshade_begin_effects>(&TimerBeginEffects);
        ::reshade::register_event<addon_event::reshade_finish_effects>(&TimerEndEffects);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

namespace
{
// ---- Menu rows ("Per shader GPU times", with frame generation active): the last sample of each timed stage, ----
// plus the derived "Bridge excl. NR" row computed in its row function ----
// Empty while the timers are off, so the default menu is exactly todays. The upstream "Interop" row is untouched
// (it lives in an upstream file); "Bridge excl. NR" sits beside it with NR own time removed. The rows read the
// decision published at the last Present exit: no trace call (and its possible log line) from the menu thread.
std::optional<double> RowCopyIn()
{
    if (!PublishedOn().load(std::memory_order_relaxed))
        return std::nullopt;
    const double ms = PublishedCopyInMs().load(std::memory_order_relaxed);
    return ms < 0.0 ? std::nullopt : std::optional<double>(ms);
}

std::optional<double> RowEffects()
{
    if (!PublishedOn().load(std::memory_order_relaxed))
        return std::nullopt;
    const double ms = PublishedEffectsMs().load(std::memory_order_relaxed);
    return ms < 0.0 ? std::nullopt : std::optional<double>(ms);
}

std::optional<double> RowInterop()
{
    if (!PublishedOn().load(std::memory_order_relaxed))
        return std::nullopt;
    const double ms = PublishedInteropMs().load(std::memory_order_relaxed);
    return ms < 0.0 ? std::nullopt : std::optional<double>(ms);
}

// The corrected row, computed here on the menu thread (never snapshotted on the present path): upstream "Interop"
// minus NR's own time, latest values read at the same present. Empty unless the timers run, frame generation is
// really producing frames, and NR reports running (a failed or idle NR must not lend its last time to this row).
// Holds no TimerMutex: like the "Neural Rendering" row beside it, this reads NR's latest time straight from the
// same call site, so the slice adds no new place that takes NR's lock. A menu row only: it has no log line.
std::optional<double> RowExcl()
{
    if (!PublishedOn().load(std::memory_order_relaxed))
        return std::nullopt;
    auto* fg = State::Instance().currentFG;
    if (!fg || !fg->IsActive() || fg->IsPaused())
        return std::nullopt;
    if (!DlssNr::IsRunning())
        return std::nullopt;
    std::optional<double> interop;
    for (const auto& d : State::Instance().detailedGpuTimes)
    {
        if (d.name != "Interop")
            continue;
        interop = d.time;
        break;
    }
    if (!interop.has_value())
        return std::nullopt; // upstream wrote no Interop: show nothing rather than a stale row
    return BridgeExclNr(interop, DlssNr::LastGpuTime());
}

struct GpuTimerRowRegistration
{
    GpuTimerRowRegistration()
    {
        ext::menu::RegisterTimeRow({ "present copy-in", RowCopyIn });
        ext::menu::RegisterTimeRow({ "ReShade effects", RowEffects });
        ext::menu::RegisterTimeRow({ "interop copy", RowInterop });
        ext::menu::RegisterTimeRow({ "Bridge excl. NR", RowExcl });
    }
} g_gpuTimerRows;
} // namespace
} // namespace ext::present
