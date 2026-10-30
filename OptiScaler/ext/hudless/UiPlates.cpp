#include "pch.h"

#include "ext/hudless/UiPlates.h"

#include "ext/hudless/HudlessAccess.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"
#include "ext/hudless/HudlessLogic.h"
#include "ext/hudless/OutputPhase.h"
#include "ext/hudless/PlateSync.h"
#include "ext/hudless/SharedSlot.h"
#include "ext/hudless/UiGpuCommon.h"
#include "ext/fg/dlssg/PresetStatus.h"

#include "ext/ExtFeatures.h"

#include "UiPlateVs_Shader.h"
#include "UiPlatePs_Shader.h"

#include <State.h>

// The check build's clock (tests/ext/hudless/plate_stubs): the engine reads PLATES_NOW() everywhere, so a scenario can
// own the two freshness windows. Production never defines PLATES_TEST_CLOCK and never includes this header.
#ifdef PLATES_TEST_CLOCK
#include "plate_test_clock.h"
#endif

#include <d3d11.h>
#include <d3d12.h>

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// Engine side of the native nameplates. Three plate-only image slots shared with D3D12, one record per real present,
// and an output pass that draws the newest hidden record over every output of the frame generator. The game side draws
// the plate triangles straight into a reserved slot on its own D3D11 device; the produce fence signalled here on the
// frame-generation queue orders that write against the presenting queue's read, so nothing copies and nothing waits on
// the CPU.
//
// Threads: the game's render thread owns every slot state transition, the open frame and the drawable record; the
// presenting threads take the pass mutex and only pin, unpin and add readers; the menu thread writes the settings
// snapshot. Lock order: pass mutex, then the leaf mutex. The leaf covers slot bookkeeping, ID3D12Fence::GetCompletedValue
// and plain copies only: no logging, no D3D call that can block, no call back into the plate code.

// The settings snapshot lives inline in PlateMode.h: the config that writes it and the seams that read it link without
// this translation unit.

namespace ext::hudless::plates
{
namespace
{
using Microsoft::WRL::ComPtr;
using sync::Lock;
using namespace hudless::detail; // TypedFormat, CreateRootSignature, Transition, WriteTextureSrv, ...

// A record older than this is not drawn: the game's own plates are back by then.
#ifndef PLATES_RECORD_FRESH_MS
#define PLATES_RECORD_FRESH_MS 250
#endif
// The pass proved alive within this window: the hide gate needs it so a plate is never hidden without being drawn.
#ifndef PLATES_ALIVE_FRESH_MS
#define PLATES_ALIVE_FRESH_MS 100
#endif
constexpr ULONGLONG kRecordFreshMs = PLATES_RECORD_FRESH_MS;
constexpr ULONGLONG kAliveFreshMs = PLATES_ALIVE_FRESH_MS;
// Consecutive promoted plate images with one size and format before anything may hide.
constexpr unsigned kStablePromotes = 3;
// Consecutive hidden records the output pass sat out before the hide gate is held closed for a while: a pass that fails
// only on the frames it hides would otherwise lose the plates of every second frame for as long as the fault lasts.
constexpr unsigned kHiddenSkipLimit = 6;
constexpr ULONGLONG kHiddenSkipHoldMs = 1000;

// The clock every window above is measured on. The check build replaces it with its own clock so a scenario can drive
// the two freshness windows at their real values; the product always reads the system tick counter.
#ifndef PLATES_NOW
#define PLATES_NOW ::GetTickCount64
#endif
inline ULONGLONG NowMs() { return static_cast<ULONGLONG>(PLATES_NOW()); }
// True when `tick` lies within `window` of `now`; a tick from the future (another thread's clock reading, or a clock
// that went backwards) never counts as fresh, which fails open.
inline bool WithinWindow(ULONGLONG now, ULONGLONG tick, ULONGLONG window)
{
    return tick != 0 && now >= tick && now - tick <= window;
}

enum class RecordKind : uint8_t
{
    Native, // no plate image: the frame was drawn natively (or had no plates, or was declined)
    Shadow, // image kept, frame drawn natively: the pass proves itself with zero instances
    Hidden, // only the image was drawn: the pass draws it back
};

struct SlotImage
{
    SharedTexture shared;                // the D3D11 texture the game draws into and its D3D12 view
    ComPtr<ID3D11RenderTargetView> target; // the game's view of the same texture (created with it)
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct Record
{
    RecordKind kind = RecordKind::Native;
    int slot = -1;
    uint64_t serial = 0; // 1-based, one per promoted record
    // The present generation the record was published in: the clock the slot ring's stale rule runs on, so the pass
    // stamps its reader with the same counter the ring compares against.
    uint64_t present = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned draws = 0; // plate draws that reached the slot
    float viewportScale = 1.0f;
    unsigned moverCount = 0;
    Mover movers[kMaxMovers] {};
    uint64_t produceValue = 0; // the produce fence value signalled for this image (0 = none)
    ULONGLONG publishedTick = 0;
    bool armed = false;  // the hide gate held for this frame (static conditions and three stable images)
    bool wanted = false; // the static conditions held when the frame was published
    bool dlssg = false;  // the output was DLSS-G when the frame was published
    bool hdr = false;    // ... and SDR
    DXGI_FORMAT imageFormat = DXGI_FORMAT_UNKNOWN;

    bool HasImage() const { return kind != RecordKind::Native && slot >= 0; }
};

// The frame between its first accepted plate draw and the frame finish. Render thread only, no lock.
struct OpenFrame
{
    bool open = false;
    FrameMode mode = FrameMode::Declined;
    int slot = -1;
    unsigned width = 0;
    unsigned height = 0;
    unsigned draws = 0;
    ULONGLONG startTick = 0;
    uint64_t present = 0; // the present generation this reservation was taken in
};

// What the outputs of this present can consume: DLSS-G output, SDR, a known multiplier of at least 2 and, under
// recomposition, a preset that does not take the plates away.
struct Consumable
{
    bool dlssg = false;
    bool hdr = false;
    bool paused = false;
    bool wanted = false;
    unsigned multiplier = 0;
};

struct Retired
{
    std::vector<ComPtr<IUnknown>> objects;
    std::vector<std::pair<ComPtr<ID3D12Fence>, uint64_t>> readers;
    uint64_t frame = 0; // the present generation the slot was retired in (the stale drop)
};

class Plates : private FenceReader
{
  public:
    // Never destroyed: no destructor may run under the loader lock, and the GPU may outlive process teardown.
    static Plates& Get()
    {
        static Plates* plates = new Plates();
        return *plates;
    }

    FrameReservation Reserve(unsigned width, unsigned height, unsigned format);
    void NoteDraw();
    bool FrameHidden() const
    {
        return open_.open && open_.present == presentTick_ && open_.mode == FrameMode::Hidden;
    }
    void OnPresentBegin();
    void OnPost(Dx11wDx12SC& sc);
    void OnOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue);
    void OnRelease();
    // The game side's frame identity: changes at every present the bridge starts, so a frame whose present never
    // reached the frame finish cannot be mistaken for the next one.
    uint64_t FrameSerialValue() const { return presentTick_; }
    void SetBitSink(void (*sink)(bool)) { bitSink_.store(sink, std::memory_order_relaxed); }

    bool HideAllowed() const
    {
        const ULONGLONG now = NowMs();
        return now >= gateHoldUntil_.load(std::memory_order_relaxed) &&
               gateArmed_.load(std::memory_order_relaxed) &&
               WithinWindow(now, gateRecordTick_.load(std::memory_order_relaxed), kRecordFreshMs) &&
               WithinWindow(now, passAliveTick_.load(std::memory_order_relaxed), kAliveFreshMs);
    }
    bool CaptureWanted() const { return captureWanted_.load(std::memory_order_relaxed); }
    bool ShiftingActive() const { return lastShifted_.load(std::memory_order_relaxed); }
    std::string LastSkip() const
    {
        Lock lock(mutex_);
        return skipMessage_;
    }
    Counters Snapshot() const
    {
        Lock lock(mutex_);
        return counters_;
    }

  private:
    static constexpr size_t kSlots = SlotTable::kSlots;
    // The pass keeps this many allocator / list / upload sets, so a presenting queue that runs behind never blocks.
    static constexpr size_t kPassRing = 16;
    static constexpr UINT kMoversUploadBytes = 64 * 32; // rects + offsets, one float4 each

    uint64_t Completed(const void* fence) override
    {
        return static_cast<ID3D12Fence*>(const_cast<void*>(fence))->GetCompletedValue();
    }

    // A counter reached from a path that holds no lock. Never call while mutex_ is held (the checked mutex fails it).
    void Bump(uint64_t Counters::*field)
    {
        Lock lock(mutex_);
        ++(counters_.*field);
    }

    // ---- the frame's reservation (render thread) ----
    void AbandonOpenFrame(const char* why);
    // A1: every engine call that touches the open frame or a slot transition runs on the drawing thread. Latching once
    // keeps the engine open (native plates) for the rest of the session.
    bool CheckThread(const char* what);
    // ---- device / lifecycle ----
    void RebindDevice(ID3D12Device* device);
    void RetireAllLocked();
    void RetireSlotLocked(int slot);
    void RetireFreeSlotsLocked();
    void PollRetiredLocked();
    // ---- the frame finish ----
    Consumable CheckConsumable(Dx11wDx12SC& sc);
    void MaybeCreateSlot(Dx11wDx12SC& sc);
    void Drain();
    void PublishNative(Record& pending);
    // A hidden record the pass sat out: counted, and after kHiddenSkipLimit in a row the hide gate is held closed for a
    // second so the frames in between are cheap shadow frames instead of an alternating hide/unhide. Caller holds the leaf.
    void NoteHiddenSkipLocked(ULONGLONG now)
    {
        ++counters_.hiddenSkipped;
        if (++hiddenSkipRun_ < kHiddenSkipLimit)
            return;
        hiddenSkipRun_ = 0;
        gateHoldUntil_.store(now + kHiddenSkipHoldMs, std::memory_order_relaxed);
    }
    // A frame that reserved and could not be published: the slot goes back, and the drawable record either stays (a
    // hidden frame whose older hidden record is still fresh) or becomes Native (nothing to draw). A null reason is a
    // drop that was already reported on the device it belongs to: nothing is logged for it.
    void DropUnpublished(Record& pending, bool hiddenFrame, const char* why);
    // Drops the pass's pin: caller holds the leaf.
    void DropPinLocked(SlotTable::Pin& pin, bool& pinned)
    {
        if (!pinned)
            return;
        slots_.DropPin(pin);
        if (counters_.pinsOutstanding > 0)
            --counters_.pinsOutstanding;
        pinned = false;
    }
    bool SignalProduce(Dx11wDx12SC& sc, uint64_t& value, ID3D12CommandQueue*& queue);
    // ---- the pass ----
    void ParkPass();
    void PollParked();
    bool EnsurePass(ID3D12Device* device, ID3D12CommandQueue* presentQueue);
    ID3D12PipelineState* EnsurePassPso(ID3D12Device* device, DXGI_FORMAT rtvFormat);

    // ---- guarded by mutex_ (the leaf) ----
    mutable sync::Mutex mutex_ { true };
    SlotTable slots_;
    std::array<SlotImage, kSlots> images_ {};
    Record record_;                                     // the drawable record (the pass copies it out)
    std::vector<Retired> retired_;                      // resources waiting for their readers' fences
    ID3D12Device* device_ = nullptr;                    // the bridge's D3D12 device the slots were opened on
    ID3D12Device* failedDevice_ = nullptr;              // ... whose produce fence could not be created (no repeating log)
    ComPtr<ID3D12Fence> produceFence_;                  // signalled on the frame-generation queue
    uint64_t produceValue_ = 0;
    unsigned stableW_ = 0;                              // size and format identity of the promoted images
    unsigned stableH_ = 0;
    DXGI_FORMAT stableF_ = DXGI_FORMAT_UNKNOWN;
    unsigned stableFrames_ = 0;
    unsigned hiddenSkipRun_ = 0;                        // hidden records the pass sat out in a row (the circuit breaker)
    Counters counters_ {};
    std::string skipMessage_;                           // the last reason the pass sat an output out
    // ---- render thread only (no lock) ----
    ID3D12CommandQueue* produceQueue_ = nullptr;        // the queue the produce fence belongs to
    uint64_t serial_ = 0;                               // promoted records (the record identity)
    uint64_t presentTick_ = 0;                          // present generations: one per present the bridge starts
    size_t nextSlot_ = 0;                               // round robin hint for the reservation
    OpenFrame open_;
    // ---- atomics (relaxed; one frame of staleness is allowed) ----
    std::atomic<DWORD> drawTid_ { 0 };
    std::atomic<bool> affinityBroken_ { false };
    std::atomic<bool> gateArmed_ { false };
    std::atomic<ULONGLONG> gateHoldUntil_ { 0 };        // the hide gate stays closed until this tick (the skip breaker)
    std::atomic<ULONGLONG> gateRecordTick_ { 0 };
    std::atomic<ULONGLONG> passAliveTick_ { 0 };
    std::atomic<bool> captureWanted_ { false };
    std::atomic<bool> produceReady_ { false };
    std::atomic<bool> lastShifted_ { false };
    std::atomic<void (*)(bool)> bitSink_ { nullptr };
    // ---- output pass (guarded by passMutex_ then mutex_) ----
    sync::Mutex passMutex_ { false };
    OutputPhase phase_;
    uint64_t lastRecordSerial_ = 0;  // the record the previous output drew (late / early outputs)
    struct PassPso
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        ComPtr<ID3D12PipelineState> pso;
    };
    struct ParkedPass
    {
        ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
        ComPtr<ID3D12RootSignature> root;
        std::vector<ComPtr<ID3D12PipelineState>> psos;
        std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
        std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
        std::vector<ComPtr<ID3D12Resource>> uploads;
        ComPtr<ID3D12DescriptorHeap> srvHeap;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
    };
    ID3D12Device* passDevice_ = nullptr;
    ComPtr<ID3D12RootSignature> passRoot_;
    std::vector<PassPso> passPsos_;
    std::array<ComPtr<ID3D12CommandAllocator>, kPassRing> passAllocators_ {};
    // One list per ring entry: a list must not be reset while the GPU is still executing it, and the presenting queue
    // may lag several outputs behind (a frame generator that runs behind the game does).
    std::array<ComPtr<ID3D12GraphicsCommandList>, kPassRing> passLists_ {};
    std::array<ComPtr<ID3D12Resource>, kPassRing> passUpload_ {};
    std::array<void*, kPassRing> passMapped_ {};
    ComPtr<ID3D12DescriptorHeap> passSrvHeap_;
    ComPtr<ID3D12DescriptorHeap> passRtvHeap_;
    UINT passSrvIncrement_ = 0;
    UINT passRtvIncrement_ = 0;
    ComPtr<ID3D12Fence> passFence_;
    uint64_t passFenceValue_ = 0;
    std::array<uint64_t, kPassRing> passAllocatorValues_ {};
    ID3D12CommandQueue* passQueue_ = nullptr;
    bool passFailed_ = false;
    size_t passRing_ = 0;
    std::vector<ParkedPass> parked_;
};

Plates& P() { return Plates::Get(); }

FrameReservation Plates::Reserve(unsigned width, unsigned height, unsigned format)
{
    FrameReservation out;
    if (State::Instance().isShuttingDown || width == 0 || height == 0)
        return out;
    // The key: one relaxed load while it is off, and nothing else happens.
    if (hudless::CurrentPlates().interp == hudless::PlateInterp::Off)
        return out;
    // Every draw of a frame runs on one thread (the bridge's immediate context). Fail open, once.
    if (!CheckThread("the plate draw"))
        return out;

    const ULONGLONG now = NowMs();
    // A reservation belongs to the frame of the present it was taken in. A present that started since then ended that
    // frame (whether or not its finish seam ran), so its reservation is dropped here rather than lent to this frame.
    if (open_.open && open_.present != presentTick_)
        AbandonOpenFrame("the present that reserved it ended");
    // The time bound is the backstop for a frame whose present never started at all (the game stopped presenting).
    if (open_.open && now >= open_.startTick && now - open_.startTick >= SlotTable::kReclaimMs)
        AbandonOpenFrame("the frame never finished");
    if (open_.open)
        return out; // one reservation per frame; a second ask in the same frame is refused
    if (!captureWanted_.load(std::memory_order_relaxed))
    {
        Bump(&Counters::declined); // the key is on, but this frame's image could not be consumed
        return out;
    }

    // Hide only when the produce path is proven and the pass is alive; otherwise the frame is a shadow frame.
    const bool canHide = HideAllowed() && produceReady_.load(std::memory_order_relaxed);
    {
        Lock lock(mutex_);
        // A slot that finished draining since the last present is usable again now, not one frame later.
        slots_.FreeDrained(presentTick_, *this);
        // A free slot of the frame's own size and format. A slot that is still being (re)created for the frame size is not
        // handed out: the next present creates one of them, and until then the frame stays native.
        int slot = -1;
        for (int attempt = 0; attempt < static_cast<int>(kSlots); ++attempt)
        {
            const int candidate = slots_.Reserve(nextSlot_, now, presentTick_, *this);
            if (candidate < 0)
                break;
            const SlotImage& image = images_[static_cast<size_t>(candidate)];
            if (image.shared.texture != nullptr && image.target != nullptr && image.width == width &&
                image.height == height && static_cast<unsigned>(image.format) == format)
            {
                slot = candidate;
                break;
            }
            slots_.Release(candidate);
            nextSlot_ = (static_cast<size_t>(candidate) + 1) % kSlots;
        }
        if (slot < 0)
        {
            ++counters_.declined;
            return out;
        }
        nextSlot_ = (static_cast<size_t>(slot) + 1) % kSlots;
        // A test-only report (nothing is compiled in for the product): the slot must have no pass read pinned on it.
        PLATES_TEST_RESERVE(slot, slots_.Pins(slot));
        open_.open = true;
        open_.mode = canHide ? FrameMode::Hidden : FrameMode::Shadow;
        open_.slot = slot;
        open_.width = width;
        open_.height = height;
        open_.draws = 0;
        open_.startTick = now;
        open_.present = presentTick_;
        out.mode = open_.mode;
        out.slot = slot;
        out.width = width;
        out.height = height;
        out.format = static_cast<unsigned>(images_[static_cast<size_t>(slot)].format);
        out.target = images_[static_cast<size_t>(slot)].target.Get();
    }
    return out;
}

bool Plates::CheckThread(const char* what)
{
    const DWORD self = GetCurrentThreadId();
    const DWORD owner = drawTid_.load(std::memory_order_relaxed);
    if (owner == 0)
    {
        drawTid_.store(self, std::memory_order_relaxed);
        return true;
    }
    if (owner == self)
        return true;
    if (!affinityBroken_.load(std::memory_order_relaxed))
    {
        affinityBroken_.store(true, std::memory_order_relaxed);
        gateArmed_.store(false, std::memory_order_relaxed);
        Bump(&Counters::affinityBroken);
        LOG_WARN("Plates: {} ran on another thread than the plate draws; the engine stays off", what);
    }
    return false;
}

void Plates::OnPresentBegin()
{
    if (State::Instance().isShuttingDown)
        return;
    // Nothing to bound while the key is off and no frame is waiting for its present.
    if (!open_.open && hudless::CurrentPlates().interp == hudless::PlateInterp::Off)
        return;
    if (!CheckThread("the present"))
        return;
    // The frame boundary. A reservation taken before it belongs to an earlier frame; the next reservation drops it.
    ++presentTick_;
}

void Plates::NoteDraw()
{
    if (open_.open && open_.draws < 1000000)
        ++open_.draws;
}

void Plates::AbandonOpenFrame(const char* why)
{
    if (!open_.open)
        return;
    {
        Lock lock(mutex_);
        if (open_.slot >= 0)
            slots_.Release(open_.slot);
        ++counters_.reclaimed;
    }
    if (open_.mode == FrameMode::Hidden)
    {
        Bump(&Counters::releasedHidden);
        LOG_WARN("Plates: a hidden frame was dropped without a present ({})", why);
    }
    open_ = OpenFrame {};
}
void Plates::RebindDevice(ID3D12Device* device)
{
    ComPtr<ID3D12Fence> fence;
    // A check build can fail the fence on a software adapter, where no creation call can be made to fail.
    const bool ok = !PLATES_TEST_FAIL_PRODUCE_FENCE() && device != nullptr &&
                    SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    {
        // The pass may hold a copied-out resource of a slot that is about to be retired: the pass mutex is the same
        // order as every other retirement (pass mutex, then the leaf) and its readers are already recorded here.
        Lock passLock(passMutex_);
        Lock lock(mutex_);
        RetireAllLocked();
        device_ = ok ? device : nullptr;
        produceFence_ = fence;
        produceValue_ = 0;
    }
    produceQueue_ = nullptr;
    produceReady_.store(false, std::memory_order_relaxed);
    gateArmed_.store(false, std::memory_order_relaxed);
    passAliveTick_.store(0, std::memory_order_relaxed);
    if (open_.open)
    {
        if (open_.mode == FrameMode::Hidden)
            Bump(&Counters::releasedHidden);
        open_ = OpenFrame {};
    }
    if (ok)
        LOG_INFO("Plates: slot ring bound to a new D3D12 device");
    else
        LOG_ERROR("Plates: the plate produce fence could not be created for the new device");
    // A device whose fence could not be created is not retried on every present: the failure is latched per device, so
    // one broken device costs one log line and no plates keep being asked for. A successful rebind, or the release that
    // ends the session on this device, clears the latch: a new device (or the same one rebuilt) gets its own attempt.
    failedDevice_ = ok ? nullptr : device;
}

void Plates::RetireSlotLocked(int slot)
{
    if (slot < 0 || static_cast<size_t>(slot) >= kSlots)
        return;
    SlotImage& image = images_[static_cast<size_t>(slot)];
    Retired retired;
    const void* fence = nullptr;
    uint64_t value = 0;
    for (size_t i = 0; slots_.Reader(slot, i, fence, value); ++i)
        retired.readers.emplace_back(ComPtr<ID3D12Fence>(static_cast<ID3D12Fence*>(const_cast<void*>(fence))), value);
    slots_.DropReaders(slot);
    auto keep = [&](auto& pointer)
    {
        if (pointer == nullptr)
            return;
        retired.objects.emplace_back();
        retired.objects.back().Attach(pointer.Detach());
    };
    keep(image.shared.resource);
    keep(image.shared.texture);
    keep(image.target);
    if (image.shared.handle != nullptr)
    {
        CloseHandle(image.shared.handle);
        image.shared.handle = nullptr;
    }
    image.width = image.height = 0;
    image.format = DXGI_FORMAT_UNKNOWN;
    if (!retired.objects.empty())
    {
        retired.frame = presentTick_;
        retired_.push_back(std::move(retired));
        ++counters_.slotsRetired;
    }
}

void Plates::RetireFreeSlotsLocked()
{
    // Nothing is drawable any more (the key is off and the last record is gone, or the engine is released): the slot
    // textures go back now rather than at the next device change. Their readers keep them alive until the GPU is done.
    for (size_t i = 0; i < kSlots; ++i)
    {
        if (slots_.State(static_cast<int>(i)) == SlotState::Free)
            RetireSlotLocked(static_cast<int>(i));
    }
    PollRetiredLocked();
}

void Plates::RetireAllLocked()
{
    for (size_t i = 0; i < kSlots; ++i)
        RetireSlotLocked(static_cast<int>(i));
    slots_.Reset();
    record_ = Record {};
    nextSlot_ = 0;
    stableW_ = stableH_ = 0;
    stableF_ = DXGI_FORMAT_UNKNOWN;
    stableFrames_ = 0;
    skipMessage_.clear();
    hiddenSkipRun_ = 0;
    gateHoldUntil_.store(0, std::memory_order_relaxed);
    PollRetiredLocked();
}

void Plates::PollRetiredLocked()
{
    for (size_t i = 0; i < retired_.size();)
    {
        bool done = true;
        for (const auto& [fence, value] : retired_[i].readers)
        {
            const uint64_t completed = fence->GetCompletedValue();
            if (completed != UINT64_MAX && completed < value)
            {
                done = false;
                break;
            }
        }
        // A retired slot whose readers never complete (a pass that was dropped before its signal) is released after
        // the same number of presents the slot ring uses, instead of holding its image for the session.
        const bool stale = !done && presentTick_ >= retired_[i].frame &&
                           presentTick_ - retired_[i].frame >= SlotRing<SlotTable::kSlots>::kStaleFrames;
        if (stale)
            ++counters_.retiredStale;
        if (done || stale)
            retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
}

Consumable Plates::CheckConsumable(Dx11wDx12SC& sc)
{
    Consumable out;
    out.dlssg = State::Instance().activeFgOutput == FGOutput::DLSSG;
    out.hdr = Access::Hudless::HdrOutput(sc);
    out.multiplier = ExpectedMultiplier();
    // A preset that takes the recomposition away takes the plates with it; only under recomposition.
    const bool recomp = hudless::CurrentMode() == hudless::Mode::Recomposition;
    out.paused = recomp && out.dlssg &&
                 fg::preset::CurrentUiRecompositionDecision(hudless::QueryRecomposition().enabled).droppedByPreset;
    out.wanted = out.dlssg && !out.hdr && out.multiplier >= 2 && !out.paused;
    return out;
}

bool Plates::SignalProduce(Dx11wDx12SC& sc, uint64_t& value, ID3D12CommandQueue*& queue)
{
    auto* fg = Access::Hudless::Fg(sc);
    queue = fg != nullptr ? fg->GetCommandQueue() : nullptr;
    if (queue == nullptr || produceFence_ == nullptr)
        return false;
    if (queue != produceQueue_)
    {
        // One fence per queue: values signalled from two queues could complete out of order.
        if (produceQueue_ != nullptr && produceValue_ > produceFence_->GetCompletedValue())
            return false;
        produceQueue_ = queue;
    }
    if (FAILED(queue->Signal(produceFence_.Get(), produceValue_ + 1)))
        return false;
    value = produceValue_ + 1;
    return true;
}

void Plates::PublishNative(Record& pending)
{
    {
        Lock lock(mutex_);
        if (record_.HasImage())
            slots_.Retire(record_.slot);
        // The stable counter is unchanged by a native record (nothing was captured); the gate is recomputed from it.
        pending.armed = pending.wanted && stableFrames_ >= kStablePromotes &&
                        !affinityBroken_.load(std::memory_order_relaxed);
        record_ = pending;
        ++counters_.promoted;
        ++counters_.native;
        slots_.FreeDrained(presentTick_, *this);
        PollRetiredLocked();
        gateArmed_.store(record_.armed, std::memory_order_relaxed);
        gateRecordTick_.store(record_.publishedTick, std::memory_order_relaxed);
    }
    // A frame the game drew itself proves nothing about the pass. Clearing this closes the native -> hidden path: the
    // hide gate then needs a shadow frame first, so no older hidden image can be drawn over a native frame's outputs.
    passAliveTick_.store(0, std::memory_order_relaxed);
    lastShifted_.store(false, std::memory_order_relaxed);
}

void Plates::DropUnpublished(Record& pending, bool hiddenFrame, const char* why)
{
    bool keep = false;
    {
        Lock lock(mutex_);
        if (pending.slot >= 0)
            slots_.Release(pending.slot);
        if (hiddenFrame)
        {
            ++counters_.promoteFailedHidden;
            ++counters_.releasedHidden;
            // This frame's own draws were hidden, so the game drew no plates for it: its outputs may keep the older
            // hidden image for one frame. Only while that image is itself hidden and fresh.
            keep = record_.kind == RecordKind::Hidden && record_.serial != 0 &&
                   WithinWindow(NowMs(), record_.publishedTick, kRecordFreshMs);
        }
        gateArmed_.store(false, std::memory_order_relaxed);
        gateRecordTick_.store(0, std::memory_order_relaxed);
        stableFrames_ = 0;
    }
    produceReady_.store(false, std::memory_order_relaxed);
    if (!keep)
    {
        // The frame drew its own plates (a shadow frame), or nothing drawable is left: an older hidden image must not
        // be drawn over this frame's outputs.
        pending.kind = RecordKind::Native;
        pending.slot = -1;
        pending.draws = 0;
        PublishNative(pending);
    }
    if (why != nullptr)
        LOG_WARN("Plates: {}", why);
}

void Plates::OnPost(Dx11wDx12SC& sc)
{
    if (State::Instance().isShuttingDown)
        return;
    // The frame finish runs on the drawing thread; the frame boundary moves with it too, so a bridge that reaches this
    // seam without the present entry still ends the frame it publishes.
    const bool threadOk = CheckThread("the frame finish");
    ++presentTick_;

    const ULONGLONG now = NowMs();
    const Consumable consumable = CheckConsumable(sc);
    const hudless::PlateSnapshot settings = hudless::CurrentPlates();
    // The multiplier arrives with the frame generator's dispatch, which can run after this batch opened: amend the open
    // batch while none of its outputs has presented yet (a batch opened with the previous dispatch's value would
    // mismatch once per change).
    phase_.OnPost(ExpectedMultiplier());
    captureWanted_.store(consumable.wanted && settings.interp != hudless::PlateInterp::Off && threadOk &&
                             !affinityBroken_.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);

    // Consume the open frame before anything below can return: a present that cannot publish must not leave the frame's
    // reservation, and its "hidden" answer, current for the next frame.
    OpenFrame frame = open_;
    open_ = OpenFrame {};
    const bool usable = frame.open && frame.mode != FrameMode::Declined && now >= frame.startTick &&
                        now - frame.startTick < SlotTable::kReclaimMs;
    PlateFrame finish {};
    if (usable)
    {
        if (const PlateFinishHook hook = PlateFinish(); hook != nullptr)
            hook(finish, PlateFinishUser());
    }

    Record pending;
    pending.kind = usable ? (frame.mode == FrameMode::Hidden ? RecordKind::Hidden : RecordKind::Shadow)
                          : RecordKind::Native;
    // A hidden frame whose image received no draw has nothing to draw, while its slot still holds whatever an older
    // frame left in it: publish Native (nothing) rather than the recycled slot. The clear belongs to the first
    // redirected draw, and NotePlateDraw is the proof that one ran.
    if (pending.kind == RecordKind::Hidden && frame.draws == 0)
    {
        pending.kind = RecordKind::Native;
        Bump(&Counters::empty);
    }
    pending.serial = ++serial_;
    pending.present = presentTick_;
    pending.publishedTick = now;
    pending.dlssg = consumable.dlssg;
    pending.hdr = consumable.hdr;
    pending.wanted = consumable.wanted;
    pending.draws = usable ? frame.draws : 0;
    if (pending.kind != RecordKind::Native)
    {
        pending.slot = frame.slot;
        pending.width = frame.width;
        pending.height = frame.height;
        pending.viewportScale = finish.viewportScale;
        pending.moverCount = finish.moverCount <= kMaxMovers ? finish.moverCount : static_cast<unsigned>(kMaxMovers);
        for (unsigned i = 0; i < pending.moverCount; ++i)
            pending.movers[i] = finish.movers[i];
        pending.imageFormat = images_[static_cast<size_t>(pending.slot)].format;
    }
    else if (frame.slot >= 0)
    {
        // Nothing of this frame is drawable: its slot goes back at once, so its image can never be drawn again.
        Lock lock(mutex_);
        slots_.Release(frame.slot);
    }

    // A different D3D12 device (bridge rebuild): everything opened on the old one is retired behind its fences.
    ID3D12Device* device12 = Access::Hudless::Dx12Device(sc);
    if (device12 == nullptr || device12 != device_)
    {
        // A device whose produce fence could not be created stays latched: the rebind is not retried on every present, and
        // neither is its warning (the failure and its first drop were reported once).
        const bool latched = device12 != nullptr && device12 == failedDevice_;
        if (device12 != nullptr && !latched)
            RebindDevice(device12); // the slots, the record and the produce fence go with the old device
        // Nothing of this frame may outlive the present it was reserved for.
        DropUnpublished(pending, usable && frame.mode == FrameMode::Hidden && frame.draws != 0,
                        latched ? nullptr
                                : (device12 == nullptr ? "the bridge had no device; the frame's image was dropped"
                                                       : "the device changed; the frame's image was dropped"));
        if (device12 != nullptr)
            MaybeCreateSlot(sc);
        Drain();
        return;
    }

    if (pending.kind == RecordKind::Native)
    {
        PublishNative(pending);
        MaybeCreateSlot(sc);
        Drain();
        return;
    }

    // Signal before the promote: until the signal succeeded the record must not become drawable (R1).
    uint64_t value = 0;
    ID3D12CommandQueue* queue = nullptr;
    if (!SignalProduce(sc, value, queue))
    {
        // The image cannot be proven complete. A shadow frame drew its own plates, so nothing may be drawn for it; a
        // hidden frame may only keep an older hidden image that is itself fresh.
        DropUnpublished(pending, pending.kind == RecordKind::Hidden,
                        "the plate produce fence could not be signalled; the frame's image was dropped");
        MaybeCreateSlot(sc);
        Drain();
        return;
    }
    // The image is complete and signalled; a test can hold the frame here, between its signal and its promote.
    PLATES_TEST_POINT("post.signalled");

    bool promoted = true;
    bool kindChanged = false;
    bool firstPromote = false;
    unsigned promotedWidth = 0;
    unsigned promotedHeight = 0;
    int retiredSlot = -1;
    {
        Lock lock(mutex_);
        produceValue_ = value;
        produceQueue_ = queue;
        // The frame finished: Reserved -> Staged -> Drawable, and the produce fence becomes a reader of the slot, so
        // the game side cannot write it again before the presenting queue is done with it.
        if (!slots_.Stage(pending.slot) || !slots_.Promote(pending.slot))
            promoted = false;
        else if (!slots_.AddReader(pending.slot, produceFence_.Get(), value, presentTick_))
            promoted = false;
        if (!promoted)
            slots_.Release(pending.slot);
        if (promoted)
        {
            // Three consecutive promoted images of one size and format before anything may hide.
            if (pending.width == stableW_ && pending.height == stableH_ && pending.imageFormat == stableF_)
            {
                if (stableFrames_ < 1000)
                    ++stableFrames_;
            }
            else
            {
                stableW_ = pending.width;
                stableH_ = pending.height;
                stableF_ = pending.imageFormat;
                stableFrames_ = 1;
            }
            pending.armed = pending.wanted && stableFrames_ >= kStablePromotes;
            pending.produceValue = value;
            kindChanged = record_.kind != pending.kind;
            retiredSlot = record_.kind == RecordKind::Native ? -1 : record_.slot;
            record_ = pending;
            ++counters_.promoted;
            firstPromote = counters_.promoted == 1;
            promotedWidth = pending.width;
            promotedHeight = pending.height;
            if (pending.kind == RecordKind::Hidden)
                ++counters_.hidden;
            else
                ++counters_.shadow;
            if (kindChanged && record_.serial > 1)
                ++counters_.transitions;
        }
        else
        {
            // The failure path below drops the image: the counters, the gate and the stable counter go with it.
        }
    }
    if (!promoted)
    {
        DropUnpublished(pending, pending.kind == RecordKind::Hidden,
                        "the plate slot could not be tracked; the frame's image was dropped");
        MaybeCreateSlot(sc);
        Drain();
        return;
    }
    produceReady_.store(true, std::memory_order_relaxed);
    {
        Lock lock(mutex_);
        if (retiredSlot >= 0)
            slots_.Retire(retiredSlot);
        slots_.FreeDrained(presentTick_, *this);
        PollRetiredLocked();
        gateArmed_.store(record_.armed, std::memory_order_relaxed);
        gateRecordTick_.store(record_.publishedTick, std::memory_order_relaxed);
    }
    static std::atomic<bool> loggedFirstPromote { false };
    if (firstPromote && !loggedFirstPromote.exchange(true))
        LOG_INFO("Plates: first plate frame promoted ({}x{})", promotedWidth, promotedHeight);
    MaybeCreateSlot(sc);
    Drain();
}

void Plates::Drain()
{
    // The engine owns the feature bit once the key is off: the menu and the config own the desired state, and the last
    // hidden record keeps being drawn until it is replaced or too old. Then the bit goes, and a key that was turned
    // back on in between gets it back (the snapshot-first write order makes that the correct final state).
    const hudless::PlateSnapshot settings = hudless::CurrentPlates();
    if (settings.interp != hudless::PlateInterp::Off)
        return;
    bool handedBack = false;
    {
        Lock lock(mutex_);
        const bool drawable = record_.kind == RecordKind::Hidden && record_.serial != 0 &&
                              WithinWindow(NowMs(), record_.publishedTick, kRecordFreshMs);
        if (drawable)
            return;
        // Nothing drawable is left: the slot textures have no further use while the key is off, so they are handed back
        // now (behind their readers' fences) instead of at the next device change. A slot that is still being read, or a
        // retired image whose fence has not completed, keeps the bit: this runs on every present, so the hand-back
        // finishes once the fences do and no texture is left behind by the key going off.
        RetireFreeSlotsLocked();
        handedBack = retired_.empty();
        for (size_t i = 0; handedBack && i < kSlots; ++i)
            handedBack = slots_.State(static_cast<int>(i)) == SlotState::Free;
    }
    if (!handedBack)
        return;
    const auto sink = bitSink_.load(std::memory_order_relaxed);
    if (sink == nullptr)
        return;
    sink(false);
    if (hudless::CurrentPlates().interp != hudless::PlateInterp::Off)
        sink(true); // the user turned the key back on while the bit was being cleared
}

void Plates::MaybeCreateSlot(Dx11wDx12SC& sc)
{
    // Slots exist only for a capture that can be consumed: the key is on, this present's output is DLSS-G and SDR with
    // a multiplier of 2 or more, and nothing paused the plates. An HDR output is never captured in this build, so no
    // slot is created for it (an FP16 slot is 66 MB at 4K, three of them 199 MB, against 100 MB for three 8-bit ones).
    if (!captureWanted_.load(std::memory_order_relaxed) ||
        hudless::CurrentPlates().interp == hudless::PlateInterp::Off)
        return;
    ID3D11Device* device11 = Access::Hudless::Dx11Device(sc);
    ID3D11Texture2D* backbuffer = Access::Hudless::SharedBackBuffer(sc, Access::Hudless::FakeIndex(sc));
    if (device11 == nullptr || backbuffer == nullptr)
        return;
    D3D11_TEXTURE2D_DESC target {};
    backbuffer->GetDesc(&target);
    const DXGI_FORMAT format = TypedFormat(target.Format);
    // The game side only classifies an 8-bit SDR target, so any other format would build an image nothing can fill.
    if (target.Width == 0 || target.Height == 0 || !IsColorFormat(format) ||
        (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM))
        return;

    int index = -1;
    ID3D12Device* createDevice = nullptr;
    {
        Lock lock(mutex_);
        if (device_ == nullptr)
            return;
        for (size_t i = 0; i < kSlots; ++i)
        {
            if (slots_.State(static_cast<int>(i)) != SlotState::Free)
                continue;
            const SlotImage& image = images_[i];
            const bool usable = image.shared.texture != nullptr && image.target != nullptr &&
                                image.width == target.Width && image.height == target.Height &&
                                image.format == format;
            if (usable)
                continue;
            index = static_cast<int>(i);
            break;
        }
        if (index < 0)
            return;
        createDevice = device_;
    }

    // The creation touches both devices and the descriptor heaps: outside the leaf. Publishing re-checks the device and
    // the slot's state, so a release or a second creation in between never installs a stale texture.
    SharedTexture fresh;
    ComPtr<ID3D11RenderTargetView> view;
    const HRESULT created = CreateSharedTexture(device11, createDevice, target.Width, target.Height, format, fresh, true);
    if (FAILED(created) || FAILED(device11->CreateRenderTargetView(fresh.texture.Get(), nullptr, &view)))
    {
        fresh.Reset();
        LOG_ERROR("Plates: could not create the plate slot ({}x{}, format {})", target.Width, target.Height,
                  static_cast<UINT>(format));
        return;
    }
    bool firstSlot = false;
    {
        Lock lock(mutex_);
        if (device_ != createDevice || slots_.State(index) != SlotState::Free)
        {
            fresh.Reset(); // a release or another creation won: drop it (nothing reads a free slot)
            return;
        }
        RetireSlotLocked(index);
        SlotImage& image = images_[static_cast<size_t>(index)];
        image.shared.texture.Attach(fresh.texture.Detach());
        image.shared.resource.Attach(fresh.resource.Detach());
        image.shared.handle = fresh.handle;
        fresh.handle = nullptr;
        image.target = view;
        image.width = target.Width;
        image.height = target.Height;
        image.format = format;
        ++counters_.slotsCreated;
        firstSlot = counters_.slotsCreated == 1;
    }
    if (firstSlot)
        LOG_INFO("Plates: first plate slot created ({}x{}, format {})", target.Width, target.Height,
                 static_cast<UINT>(format));
}

void Plates::OnRelease()
{
    // A release from the DLL detach path runs under the loader lock and the presenting threads are gone: nothing here may
    // take a lock or wait for the GPU.
    if (State::Instance().isShuttingDown)
        return;
    // The release runs on the render thread like the draws and the frame finish, but it does not judge it: a resize or a
    // device rebuild may hand the draws to another thread legitimately, so this only re-latches the drawing thread (the
    // next draw latches the one it runs on). Latching the engine off here would fail it open for the session.
    Lock passLock(passMutex_);
    ParkPass();
    {
        // Retire the slots before the parked set may be dropped: a slot's reader fences are the raw pointers the parked
        // pass objects own, and they must outlive the walk that turns them into their own references.
        Lock lock(mutex_);
        RetireAllLocked();
        device_ = nullptr;
        failedDevice_ = nullptr; // the latch belongs to that device's session: a rebuilt one gets its own attempt
        produceFence_.Reset();
        produceValue_ = 0;
    }
    PollParked();
    produceQueue_ = nullptr;
    produceReady_.store(false, std::memory_order_relaxed);
    gateArmed_.store(false, std::memory_order_relaxed);
    gateRecordTick_.store(0, std::memory_order_relaxed);
    passAliveTick_.store(0, std::memory_order_relaxed);
    captureWanted_.store(false, std::memory_order_relaxed);
    lastShifted_.store(false, std::memory_order_relaxed);
    if (open_.open)
    {
        if (open_.mode == FrameMode::Hidden)
            Bump(&Counters::releasedHidden);
        open_ = OpenFrame {};
    }
    // A resize may move the game's render thread (a device rebuild does): the next engine call latches the new one
    // instead of failing the session off. A real two-thread break is already latched in affinityBroken_.
    drawTid_.store(0, std::memory_order_relaxed);
}

void Plates::ParkPass()
{
    // A parked pass set is gone: the next build starts from no failure. The failure handler parks its own half-built set
    // before it latches the failure, so the latch survives exactly the failure it belongs to, and a release (a resize, a
    // device rebuild, a shutdown) gives the next build its chance.
    passFailed_ = false;
    if (!passRoot_ && !passLists_[0])
        return;
    ParkedPass parked;
    parked.fence = passFence_;
    parked.value = passFenceValue_;
    parked.root = passRoot_;
    for (auto& entry : passPsos_)
        parked.psos.push_back(entry.pso);
    for (auto& allocator : passAllocators_)
        parked.allocators.push_back(allocator);
    for (auto& list : passLists_)
        parked.lists.push_back(list);
    for (auto& upload : passUpload_)
        parked.uploads.push_back(upload);
    parked.srvHeap = passSrvHeap_;
    parked.rtvHeap = passRtvHeap_;
    parked_.push_back(std::move(parked));
    passRoot_.Reset();
    passPsos_.clear();
    for (auto& allocator : passAllocators_)
        allocator.Reset();
    for (auto& list : passLists_)
        list.Reset();
    for (auto& upload : passUpload_)
        upload.Reset();
    passMapped_.fill(nullptr);
    passSrvHeap_.Reset();
    passRtvHeap_.Reset();
    passFence_.Reset();
    passFenceValue_ = 0;
    passAllocatorValues_.fill(0);
    passQueue_ = nullptr;
    passDevice_ = nullptr;
}

void Plates::PollParked()
{
    for (size_t i = 0; i < parked_.size();)
    {
        const uint64_t done = parked_[i].fence ? parked_[i].fence->GetCompletedValue() : UINT64_MAX;
        if (done != UINT64_MAX && done >= parked_[i].value)
            parked_.erase(parked_.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
}

bool Plates::EnsurePass(ID3D12Device* device, ID3D12CommandQueue* presentQueue)
{
    if (passFailed_)
        return false;
    if (device != passDevice_)
    {
        ParkPass();
        passDevice_ = device;
        D3D12_DESCRIPTOR_RANGE range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER params[3] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = { 0, 0, 4 };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor = { 1, 0 };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = { 1, &range };
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc { 3, params, 0, nullptr,
                                            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        passRoot_ = CreateRootSignature(device, rootDesc, "Plate pass");
        // A check build can fail the build on a software adapter, where no creation call can be made to fail.
        bool ok = !PLATES_TEST_FAIL_PASS_BUILD() && passRoot_ != nullptr &&
                  SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&passFence_)));
        for (size_t i = 0; ok && i < kPassRing; ++i)
            ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          IID_PPV_ARGS(&passAllocators_[i])));
        for (size_t i = 0; ok && i < kPassRing; ++i)
        {
            ok = SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, passAllocators_[i].Get(),
                                                     nullptr, IID_PPV_ARGS(&passLists_[i])));
            if (ok)
                passLists_[i]->Close();
        }
        if (ok)
        {
            const auto heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
            const auto desc = BufferDesc(kMoversUploadBytes, D3D12_RESOURCE_FLAG_NONE);
            for (size_t i = 0; ok && i < kPassRing; ++i)
            {
                ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&passUpload_[i])));
                if (ok)
                    ok = SUCCEEDED(passUpload_[i]->Map(0, nullptr, &passMapped_[i])) && passMapped_[i] != nullptr;
            }
        }
        if (ok)
        {
            passSrvHeap_ = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, static_cast<UINT>(kPassRing), true);
            passRtvHeap_ = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, static_cast<UINT>(kPassRing), false);
            ok = passSrvHeap_ && passRtvHeap_;
        }
        if (ok)
        {
            passSrvIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            passRtvIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        if (!ok)
        {
            LOG_ERROR("Plates: creating the plate pass objects failed");
            ParkPass(); // nothing was ever submitted through the half-built set
            passFailed_ = true;
            return false;
        }
        LOG_INFO("Plates: plate pass objects created");
    }
    if (presentQueue != passQueue_)
    {
        // One fence per queue: rebind after the old queue's passes finished.
        if (passQueue_ != nullptr && passFenceValue_ > passFence_->GetCompletedValue())
            return false;
        passQueue_ = presentQueue;
    }
    return true;
}

ID3D12PipelineState* Plates::EnsurePassPso(ID3D12Device* device, DXGI_FORMAT rtvFormat)
{
    for (auto& entry : passPsos_)
        if (entry.format == rtvFormat)
            return entry.pso.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = passRoot_.Get();
    desc.VS = { UiPlateVs_cso, sizeof(UiPlateVs_cso) };
    desc.PS = { UiPlatePs_cso, sizeof(UiPlatePs_cso) };
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    // The plate image is premultiplied: add it over the frame, keep the frame's alpha.
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtvFormat;
    desc.SampleDesc = { 1, 0 };
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))))
    {
        LOG_ERROR("Plates: CreateGraphicsPipelineState failed for format {}", static_cast<UINT>(rtvFormat));
        return nullptr;
    }
    passPsos_.push_back({ rtvFormat, pso });
    return passPsos_.back().pso.Get();
}

void Plates::OnOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue)
{
    if (State::Instance().isShuttingDown || swapChain == nullptr || presentQueue == nullptr)
    {
        // Nothing is drawn: the game's own plates come back at once.
        passAliveTick_.store(0, std::memory_order_relaxed);
        return;
    }
    Lock passLock(passMutex_);
    PollParked();

    // The batch this output belongs to (the output phase has its own leaf mutex: never under ours). The multiplier may
    // have arrived after this batch opened: forwarding it amends the batch while none of its outputs presented yet.
    const hudless::PlateSnapshot settings = hudless::CurrentPlates();
    phase_.NoteMultiplier(ExpectedMultiplier());
    const auto phase = phase_.OnOutput(settings.phase);

    Record record;
    ComPtr<ID3D12Fence> produceFence;
    ComPtr<ID3D12Resource> image;
    ID3D12Device* bridge = nullptr;
    SlotTable::Pin pin {};
    bool pinned = false;
    {
        // One section: the record, everything needed to draw it, and the pin that keeps the slot out of the game side's
        // hands until this output's read is recorded.
        Lock lock(mutex_);
        record = record_;
        produceFence = produceFence_;
        bridge = device_;
        if (record.HasImage() && static_cast<size_t>(record.slot) < kSlots)
            image = images_[static_cast<size_t>(record.slot)].shared.resource;
        if (record.HasImage())
        {
            pin = slots_.TakePin(record.slot);
            pinned = static_cast<bool>(pin);
            if (pinned)
            {
                ++counters_.pinned;
                ++counters_.pinsOutstanding;
            }
        }
        const bool changed = record.serial != lastRecordSerial_;
        if (phase.m > 0 && phase.k >= phase.m)
        {
            if (changed)
                ++counters_.lateOutputs;
            else
                ++counters_.earlyOutputs;
        }
        lastRecordSerial_ = record.serial;
    }
    // Between the copy-out above and the reader add below the pass holds a pin: a test can hold it here, and the game
    // side must then find that slot out of its hands.
    PLATES_TEST_POINT("pass.pinned");
    struct PinGuard
    {
        Plates& owner;
        SlotTable::Pin& pin;
        bool& pinned;
        ~PinGuard()
        {
            if (pinned)
            {
                Lock lock(owner.mutex_);
                owner.DropPinLocked(pin, pinned);
            }
        }
    } pinGuard { *this, pin, pinned };

    const ULONGLONG now = NowMs();
    auto skip = [&](const char* why)
    {
        bool hidden = false;
        {
            Lock lock(mutex_);
            ++counters_.skipped;
            hidden = record.kind == RecordKind::Hidden;
            if (hidden)
                NoteHiddenSkipLocked(now);
            skipMessage_ = why;
            DropPinLocked(pin, pinned);
        }
        // The game's own plates come back at once: a pass that draws nothing is not alive.
        passAliveTick_.store(0, std::memory_order_relaxed);
        lastShifted_.store(false, std::memory_order_relaxed);
    };

    const bool fresh = WithinWindow(now, record.publishedTick, kRecordFreshMs);
    const bool native = record.kind == RecordKind::Native;
    const bool shadow = record.kind == RecordKind::Shadow;
    const bool slotReady = record.slot >= 0 && static_cast<size_t>(record.slot) < kSlots && image != nullptr;
    switch (PrelimPlateRecord(fresh, native, shadow, slotReady))
    {
    case PlatePrelim::Stale:
        skip("no fresh plate record");
        return;
    case PlatePrelim::Native:
        skip("the frame was drawn natively");
        return;
    case PlatePrelim::NoSlot:
        skip("plate slot not ready");
        return;
    case PlatePrelim::Shadow:
    case PlatePrelim::Ready:
        break;
    }
    if (!record.dlssg || record.hdr)
    {
        skip("provider changed or HDR output");
        return;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(presentQueue->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != bridge || bridge == nullptr)
    {
        skip("presenting queue is on another D3D12 device");
        return;
    }
    if (produceFence == nullptr || record.produceValue == 0)
    {
        skip("plate image not produced yet");
        return;
    }
    if (!EnsurePass(device.Get(), presentQueue))
    {
        skip("plate pass objects failed");
        return;
    }

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
    {
        skip("no current backbuffer index");
        return;
    }
    ComPtr<ID3D12Resource> backBuffer;
    const HRESULT backBufferHr = swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
    swapChain3.Reset();
    if (FAILED(backBufferHr) || !backBuffer)
    {
        skip("no presenting backbuffer");
        return;
    }
    // Do not keep the backbuffer past this call (it would make ResizeBuffers fail).
    struct BackBufferRelease
    {
        ComPtr<ID3D12Resource>& ref;
        ~BackBufferRelease() { ref.Reset(); }
    } backBufferRelease { backBuffer };

    const auto backDesc = backBuffer->GetDesc();
    if (backDesc.Width != record.width || backDesc.Height != record.height || backDesc.SampleDesc.Count != 1 ||
        (backDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
    {
        skip("backbuffer does not match the plate image");
        return;
    }
    const DXGI_FORMAT rtvFormat = TypedFormat(backDesc.Format);
    if (rtvFormat == DXGI_FORMAT_UNKNOWN)
    {
        skip("backbuffer format has no typed view");
        return;
    }
    ID3D12PipelineState* pso = EnsurePassPso(device.Get(), rtvFormat);
    if (pso == nullptr)
    {
        skip("plate pipeline failed");
        return;
    }

    const size_t ringIndex = (passRing_ + 1) % kPassRing;
    if (passAllocatorValues_[ringIndex] > passFence_->GetCompletedValue())
    {
        skip("plate pass objects busy");
        return;
    }
    passRing_ = ringIndex;
    ID3D12GraphicsCommandList* const list = passLists_[ringIndex].Get();
    if (FAILED(passAllocators_[ringIndex]->Reset()) ||
        FAILED(list->Reset(passAllocators_[ringIndex].Get(), nullptr)))
    {
        skip("plate command list reset failed");
        return;
    }
    auto abandon = [&]()
    {
        list->Close();
        bool hidden = false;
        {
            Lock lock(mutex_);
            ++counters_.skipped;
            hidden = record.kind == RecordKind::Hidden;
            if (hidden)
                NoteHiddenSkipLocked(now);
            skipMessage_ = "plate list abandoned";
            DropPinLocked(pin, pinned);
        }
        passAliveTick_.store(0, std::memory_order_relaxed);
        lastShifted_.store(false, std::memory_order_relaxed);
    };

    // A shadow record proves the pass end to end with zero instances; a hidden one draws its image: the whole image
    // unshifted at endpoint, or each mover shifted by this output's share of the frame-to-frame movement under 'on'.
    const bool shifting = settings.interp == hudless::PlateInterp::On && phase.shift && record.moverCount > 0 &&
                          record.armed && record.kind == RecordKind::Hidden;
    const float stepCap = static_cast<float>(settings.step);
    struct MoversUpload
    {
        float rect[64][4];
        float off[64][4];
    };
    auto* upload = static_cast<MoversUpload*>(passMapped_[ringIndex]);
    memset(upload, 0, sizeof(*upload));
    UINT instances = 0;
    bool shifted = false;
    if (record.kind == RecordKind::Hidden)
    {
        if (!shifting)
        {
            upload->rect[0][2] = static_cast<float>(record.width);
            upload->rect[0][3] = static_cast<float>(record.height);
            instances = 1;
        }
        else
        {
            const float width = static_cast<float>(record.width), height = static_cast<float>(record.height);
            for (unsigned i = 0; i < record.moverCount; ++i)
            {
                // The image holds the plates at this frame's position: move them back by the share of the movement that
                // belongs to this output. Integer offsets keep the glyphs sharp; the margin keeps their edges (the hole
                // the shader cuts in movers below is the exact rect, without this margin).
                float ox = record.movers[i].endpoint ? 0.0f : -record.movers[i].offX * (1.0f - phase.s);
                float oy = record.movers[i].endpoint ? 0.0f : -record.movers[i].offY * (1.0f - phase.s);
                if (std::fabs(ox) > stepCap || std::fabs(oy) > stepCap)
                {
                    ox = 0.0f;
                    oy = 0.0f;
                }
                ox = std::round(ox);
                oy = std::round(oy);
                if (ox != 0.0f || oy != 0.0f)
                    shifted = true;
                upload->rect[i][0] = std::max(0.0f, std::floor(record.movers[i].srcX0) - 2.0f);
                upload->rect[i][1] = std::max(0.0f, std::floor(record.movers[i].srcY0) - 2.0f);
                upload->rect[i][2] = std::min(width, std::ceil(record.movers[i].srcX1) + 2.0f);
                upload->rect[i][3] = std::min(height, std::ceil(record.movers[i].srcY1) + 2.0f);
                upload->off[i][0] = ox;
                upload->off[i][1] = oy;
            }
            instances = record.moverCount;
        }
    }
    // MoverCount is a real uint (the root constant is raw bits): a float 1.0 would read back as 1065353216.
    uint32_t params[4];
    params[0] = instances;
    params[1] = std::bit_cast<uint32_t>(static_cast<float>(record.width));
    params[2] = std::bit_cast<uint32_t>(static_cast<float>(record.height));
    params[3] = 0;

    WriteTextureSrv(device.Get(), image.Get(), record.imageFormat,
                    CpuHandle(passSrvHeap_.Get(), static_cast<UINT>(ringIndex), passSrvIncrement_));
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(backBuffer.Get(), &rtvDesc,
                                   CpuHandle(passRtvHeap_.Get(), static_cast<UINT>(ringIndex), passRtvIncrement_));

    Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->SetGraphicsRootSignature(passRoot_.Get());
    list->SetPipelineState(pso);
    list->SetGraphicsRoot32BitConstants(0, 4, params, 0);
    list->SetGraphicsRootConstantBufferView(1, passUpload_[ringIndex]->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[1] = { passSrvHeap_.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(2,
                                              GpuHandle(passSrvHeap_.Get(), static_cast<UINT>(ringIndex), passSrvIncrement_));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport { 0.0f, 0.0f, static_cast<FLOAT>(backDesc.Width),
                                    static_cast<FLOAT>(backDesc.Height), 0.0f, 1.0f };
    const D3D12_RECT scissor { 0, 0, static_cast<LONG>(backDesc.Width), static_cast<LONG>(backDesc.Height) };
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    const auto rtvHandle = CpuHandle(passRtvHeap_.Get(), static_cast<UINT>(ringIndex), passRtvIncrement_);
    list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    list->DrawInstanced(6, instances, 0, 0);
    Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    // The slot must still be readable when this list runs: record the read (and drop the pin) in one section, then close.
    const uint64_t readValue = passFenceValue_ + 1;
    bool readerAdded = false;
    {
        Lock lock(mutex_);
        readerAdded = slots_.AddReader(record.slot, passFence_.Get(), readValue, record.present);
        if (readerAdded)
            DropPinLocked(pin, pinned);
    }
    if (!readerAdded)
    {
        abandon();
        return;
    }
    if (FAILED(list->Close()))
    {
        abandon();
        return;
    }
    // The game side's draw completed before the interop fence this present waited on; the wait below orders the two
    // queues, and the read only completes when the presenting queue reaches the pass fence.
    presentQueue->Wait(produceFence.Get(), record.produceValue);
    ID3D12CommandList* lists[1] = { list };
    presentQueue->ExecuteCommandLists(1, lists);
    presentQueue->Signal(passFence_.Get(), readValue);
    passFenceValue_ = readValue;
    passAllocatorValues_[ringIndex] = readValue;
    bool report = false;
    Counters counters;
    std::string lastSkip;
    {
        Lock lock(mutex_);
        // The pass is alive for the frame it drew, but only while that frame's record is still the drawable one: a frame
        // the game drew natively publishes a Native record that clears the alive tick, and storing the tick after that
        // would re-open the hide gate over the native frame (the next frame would hide and its plates would be lost).
        if (record_.serial == record.serial)
        {
            passAliveTick_.store(now, std::memory_order_relaxed);
            lastShifted_.store(shifted, std::memory_order_relaxed);
        }
        // "Plated" means the image was drawn: a shadow record submits zero instances and is not a draw, and only a
        // plated hidden record says the fault that made the breaker trip is gone.
        if (record.kind == RecordKind::Hidden)
        {
            ++counters_.plated;
            hiddenSkipRun_ = 0;
        }
        report = record.kind == RecordKind::Hidden && counters_.plated % 600 == 0;
        if (report)
        {
            counters = counters_;
            lastSkip = skipMessage_;
        }
    }
    if (report)
        LOG_INFO("Plates: engine: {} records, {} outputs plated, {} skipped (last: {})", counters.promoted,
                 counters.plated, counters.skipped, lastSkip);
}
} // namespace

// ---- the game side's frame finish ---------------------------------------------------------------------------------------
namespace
{
std::atomic<PlateFinishHook> g_finishHook { nullptr };
std::atomic<void*> g_finishUser { nullptr };
} // namespace

void SetPlateFinishHook(PlateFinishHook hook, void* user)
{
    g_finishUser.store(user, std::memory_order_relaxed);
    g_finishHook.store(hook, std::memory_order_relaxed);
}

// The engine owns its feature bit: with no sink installed the bit is the ext bit itself, so nothing outside has to wire
// it. A test (or a host that wants to see the transitions) replaces the sink.
void DefaultBitSink(bool on) { ext::SetActive(ext::F::Plate, on); }
struct RegisterBitSink
{
    RegisterBitSink() { SetPlateFeatureBitSink(&DefaultBitSink); }
};
const RegisterBitSink g_registerBitSink;

PlateFinishHook PlateFinish() { return g_finishHook.load(std::memory_order_relaxed); }
void* PlateFinishUser() { return g_finishUser.load(std::memory_order_relaxed); }

// ---- the module's entry points -----------------------------------------------------------------------------------------
FrameReservation ReserveFrame(unsigned width, unsigned height, unsigned format)
{
    return P().Reserve(width, height, format);
}
void NotePlateDraw() { P().NoteDraw(); }
bool FrameHidden() { return P().FrameHidden(); }
uint64_t FrameSerial() { return P().FrameSerialValue(); }
void OnPresentBegin() { P().OnPresentBegin(); }
void OnPost(Dx11wDx12SC& sc) { P().OnPost(sc); }
void OnRelease() { P().OnRelease(); }
void OnOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue) { P().OnOverlay(swapChain, presentQueue); }
std::string LastSkip() { return P().LastSkip(); }
bool ShiftingActive() { return P().ShiftingActive(); }
bool HideAllowed() { return P().HideAllowed(); }
bool CaptureWanted() { return P().CaptureWanted(); }
void SetPlateFeatureBitSink(void (*sink)(bool)) { P().SetBitSink(sink); }
Counters Snapshot() { return P().Snapshot(); }
} // namespace ext::hudless::plates
