#pragma once
// Rig for the native nameplate engine protocol test: a real D3D11 WARP device as the game, a real D3D12 WARP device as
// the bridge, a fake swapchain with a render-target backbuffer, and a driver that plays the game side and the
// presentation side one step at a time, so a scenario can present an output late or early.
//
// A "plate draw" here is a write into the reserved slot's render target view (and, for a frame the game also draws
// natively, into its own colour target) on the game's immediate context: the transport the engine protocol depends on.
// The game's real draw state and the shader path are proved by the render-target slot test.
//
// The backbuffer is refilled with a known background before every output, so one pixel read after the pass says which
// frame's plate image the output received, or that it received none.

#include "pch.h"

#include "ui_fake_swapchain.h"
#include "ui_harness.h"

#include <State.h>

#include "ext/hudless/HudlessAccess.h"
#include "ext/hudless/PlateMode.h"
#include "ext/hudless/PlateSync.h"
#include "ext/hudless/UiPlates.h"

// The product's present seams (ext/hudless/HudlessSeams.h) for the scenarios that route their presents through the
// wiring instead of the engine's entry points. Only the recipes that define PLATES_SEAM_ROUTING compile it, and those
// provide the HUD-less engine's own bodies as stubs.
#ifdef PLATES_SEAM_ROUTING
#include "ext/hudless/HudlessSeams.h"
#endif

#include "plate_test_clock.h"

// The end-to-end scenario test compiles the product's game side in (PlateNative.cpp + the real engine) and drives the
// frames through the classifier instead of reserving by hand: the plate draws then carry the real state and the redirect
// runs the real routine. The engine protocol test leaves this undefined.
#ifdef PLATES_GAME_SIDE
#include "ext/ffxiv/plates/PlateNative.h"
#include "plate_game_rig.h"
#endif

#include <d3d11.h>
#include <d3d12.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace plate_test
{
using Microsoft::WRL::ComPtr;
using ui_test::Check;
using ui_test::FakeSwapChain;
using ui_test::Rig;

// The "generated frame" every output starts from: the pass must leave it alone outside the plate image.
inline constexpr uint32_t kBackground = 0xFF0B8007u; // (7, 128, 11, 255)
// Frame n's plate colour: opaque, unique per frame, never the background.
inline uint32_t PlateColour(unsigned n) { return (n * 7u + 3u) | ((n * 13u + 11u) << 8) | ((n * 29u + 5u) << 16) | 0xFF000000u; }
// Which frame's colour this is, 0 when it is not a plate colour.
inline unsigned FrameOf(uint32_t pixel)
{
    for (unsigned n = 1; n < 128; ++n)
        if (PlateColour(n) == pixel)
            return n;
    return 0;
}

// The feature bit sink: the engine clears the bit when the last hidden record is gone and the key is off, and sets it
// again when the key came back on in between. The test owns the bit and can act inside the clear (a menu write at that
// instant).
inline std::vector<bool>& BitCalls()
{
    static std::vector<bool> calls;
    return calls;
}
// What the test does at the instant the engine clears (or sets) the bit: a menu write landing between the engine's
// clear and its re-read of the snapshot.
inline void (*BitHook)(bool on) = nullptr;

inline void RecordBit(bool on)
{
    BitCalls().push_back(on);
    if (BitHook != nullptr)
        BitHook(on);
}

struct FrameEntry
{
    unsigned n = 0;
    ext::hudless::plates::FrameMode mode = ext::hudless::plates::FrameMode::Declined;
    int slot = -1;
    unsigned nativeDraws = 0;
    uint64_t promoted = 0; // the promoted record count after the frame's present
    bool open = false;     // reserved but not posted yet
    bool dropped = false;  // reserved, and its present ended without the frame finish
};

struct OutputEntry
{
    unsigned batch = 0;     // the frame the script says this output belongs to
    unsigned drawn = 0;     // the frame whose plate image it received, 0 = nothing
    uint32_t pixel = 0;
    bool pending = false;   // its GPU work has not completed yet (FinishDeferred reads it)
    ext::hudless::plates::Counters counters {};
};

// A pause at one of the engine's test points (PLATES_TEST_POINT, check build only): a scenario lets the presenting side
// run into the point and stop there while the game side keeps working, which is the only way to hold a pin across a
// reservation or to keep GPU work outstanding.
class PauseGate
{
  public:
    explicit PauseGate(const char* point) : point_(point)
    {
        Current() = this;
        ext::hudless::plates::sync::SetTestPoint(&PauseGate::Hook);
    }
    ~PauseGate()
    {
        Release();
        ext::hudless::plates::sync::SetTestPoint(nullptr);
        Current() = nullptr;
    }
    PauseGate(const PauseGate&) = delete;
    PauseGate& operator=(const PauseGate&) = delete;

    // Waits until an engine call reached the point. False when it did not arrive in time (the scenario then fails).
    bool WaitReached(unsigned timeoutMs = 30000)
    {
        std::unique_lock lock(mutex_);
        return reached_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return arrived_; });
    }

    void Release()
    {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        wake_.notify_all();
    }

    bool Passed() const { return passed_.load(); }

  private:
    static PauseGate*& Current()
    {
        static PauseGate* current = nullptr;
        return current;
    }

    static void Hook(const char* point)
    {
        PauseGate* gate = Current();
        if (gate != nullptr && gate->point_ != nullptr && std::strcmp(point, gate->point_) == 0)
        {
            gate->passed_.store(true);
            std::unique_lock lock(gate->mutex_);
            gate->arrived_ = true;
            gate->reached_.notify_all();
            gate->wake_.wait_for(lock, std::chrono::seconds(60), [gate] { return gate->released_; });
        }
    }

    const char* point_ = nullptr;
    std::mutex mutex_;
    std::condition_variable reached_;
    std::condition_variable wake_;
    bool arrived_ = false;
    bool released_ = false;
    std::atomic<bool> passed_ { false };
};

class Driver
{
  public:
    Driver(UINT width, UINT height, unsigned multiplier) : width_(width), height_(height)
    {
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d3d11_,
                                nullptr, &context_),
              "D3D11 WARP device");
        primary_ = std::make_unique<Rig>();
        rig_ = primary_.get();
        swapChain_.buffer = MakeBackbuffer();
        fg_.queue = rig_->queue.Get();
        bridgeExt_.hdrOutput = false;
        sc_._dx11Device = d3d11_.Get();
        sc_._dx11Context = context_.Get();
        sc_._dx12Device = rig_->device.Get();
        sc_._fg = &fg_;
        sc_._ext = &bridgeExt_;
        sc_._bufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        MakeGameTargets();
        ext::hudless::PublishPlateSettings({ ext::hudless::PlateInterp::Endpoint, 1, 24 });
        ext::hudless::plates::SetPlateFeatureBitSink(&RecordBit);
        ext::hudless::plates::NoteMultiplier(multiplier);
        State::Instance().activeFgOutput = FGOutput::DLSSG;
#ifdef PLATES_GAME_SIDE
        Self() = this;
        // After the device exists: the game-state rig creates its own targets and states on it.
        state_ = std::make_unique<plate_game_test::DrawStateRig>(d3d11_.Get(), context_.Get(), width_, height_);
#endif
    }

    ~Driver()
    {
        ext::hudless::plates::OnRelease();
        ext::hudless::plates::SetPlateFeatureBitSink(nullptr);
    }

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    // ---- the game side -----------------------------------------------------------------------------------------------
#ifdef PLATES_GAME_SIDE
    // One real frame through the product's own draw path: the plate state is real, the first plate draw asks the engine,
    // and the redirect draws through a stand-in for the game's DrawIndexed that paints the bound target with the frame's
    // plate colour.
    // `first` breaks the named condition of the frame's first draw (a UI draw where a plate draw was expected); `second`
    // breaks a further draw of the same frame (a UI draw after the plate draw).
    void Frame(unsigned n, bool withPlate = true, const char* first = nullptr, const char* second = nullptr)
    {
        plate_test_clock::Advance();
        FrameEntry entry;
        entry.n = n;
        gameFrame_ = n;
        nativeDraws_ = 0;
        slotDraws_ = 0;
        state_->BindPlateDraw(first);
        if (withPlate)
        {
            if (!DrawOne())
                GameOriginal(context_.Get(), 3, 0, 0); // the game's own draw (the hook's normal path)
            // The frame's mode, from the first draw: the engine says whether the frame is hidden, and a draw that reached
            // the image says whether it is a shadow frame.
            const bool hidden = ext::hudless::plates::FrameHidden();
            const bool imaged = slotDraws_ > 0;
            entry.mode = hidden ? ext::hudless::plates::FrameMode::Hidden
                                : (imaged ? ext::hudless::plates::FrameMode::Shadow
                                          : ext::hudless::plates::FrameMode::Declined);
            entry.slot = imaged ? 1 : -1;
            if (second != nullptr)
            {
                state_->BindPlateDraw(second);
                if (!DrawOne())
                    GameOriginal(context_.Get(), 3, 0, 0);
            }
        }
        entry.nativeDraws = nativeDraws_;
        entry.open = true;
        frames_.push_back(entry);
        FlushGame();
    }

    // One game draw through the product's own path. True = the draw was consumed (the frame is hidden).
    bool DrawOne()
    {
        return ext::ffxiv::plates::InterceptIndexed(context_.Get(), 3, 0, 0,
                                                    reinterpret_cast<void*>(&Driver::GameOriginal));
    }

    static void STDMETHODCALLTYPE GameOriginal(ID3D11DeviceContext* context, UINT, UINT, INT)
    {
        Driver& self = *Self();
        ID3D11RenderTargetView* target = nullptr;
        context->OMGetRenderTargets(1, &target, nullptr);
        if (target != nullptr)
        {
            const float colour[4] = { ((PlateColour(self.gameFrame_) >> 0) & 0xFF) / 255.0f,
                                      ((PlateColour(self.gameFrame_) >> 8) & 0xFF) / 255.0f,
                                      ((PlateColour(self.gameFrame_) >> 16) & 0xFF) / 255.0f, 1.0f };
            context->ClearRenderTargetView(target, colour);
            // The game's own colour target, or the plate image the engine reserved (which lives on the engine side).
            if (target == self.state_->TargetView())
                ++self.nativeDraws_; // the game's own colour target
            else
                ++self.slotDraws_;   // the plate image the engine reserved for this frame
            target->Release();
        }
    }

    static Driver*& Self()
    {
        static Driver* self = nullptr;
        return self;
    }
#else
    // One real frame: the first plate draw asks the engine, then the frame's draws happen where the answer puts them.
    void Frame(unsigned n, bool withPlate = true)
    {
        plate_test_clock::Advance();
        FrameEntry entry;
        entry.n = n;
        context_->OMSetRenderTargets(1, &nativeRtv_, nativeDsv_.Get());
        if (withPlate)
        {
            const auto reservation = ext::hudless::plates::ReserveFrame(width_, height_, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
            entry.mode = reservation.mode;
            entry.slot = reservation.slot;
            if (reservation)
            {
                // The redirect: the plate triangle goes into the reserved slot instead of the game's own target.
                const float colour[4] = { ((PlateColour(n) >> 0) & 0xFF) / 255.0f, ((PlateColour(n) >> 8) & 0xFF) / 255.0f,
                                          ((PlateColour(n) >> 16) & 0xFF) / 255.0f, 1.0f };
                context_->OMSetRenderTargets(1, &reservation.target, nativeDsv_.Get());
                context_->ClearRenderTargetView(reservation.target, colour);
                ext::hudless::plates::NotePlateDraw();
            }
            if (reservation.mode != ext::hudless::plates::FrameMode::Hidden)
            {
                // The game draws the frame's plates itself (Declined) or both (Shadow).
                const float colour[4] = { ((PlateColour(n) >> 0) & 0xFF) / 255.0f,
                                          ((PlateColour(n) >> 8) & 0xFF) / 255.0f,
                                          ((PlateColour(n) >> 16) & 0xFF) / 255.0f, 1.0f };
                context_->OMSetRenderTargets(1, &nativeRtv_, nativeDsv_.Get());
                context_->ClearRenderTargetView(nativeRtv_.Get(), colour);
                ++entry.nativeDraws;
            }
        }
        entry.open = true;
        frames_.push_back(entry);
        FlushGame();
    }

    // A frame that reserved and then drew nothing into its slot (its first redirect failed after the reservation, or the
    // resize changed the target between the reservation and the draw). Nothing calls NotePlateDraw, so the image holds
    // whatever an earlier frame left in that slot.
    void FrameNoDraw(unsigned n)
    {
        plate_test_clock::Advance();
        FrameEntry entry;
        entry.n = n;
        context_->OMSetRenderTargets(1, &nativeRtv_, nativeDsv_.Get());
        const auto reservation =
            ext::hudless::plates::ReserveFrame(width_, height_, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
        entry.mode = reservation.mode;
        entry.slot = reservation.slot;
        if (reservation.mode != ext::hudless::plates::FrameMode::Hidden)
        {
            const float colour[4] = { ((PlateColour(n) >> 0) & 0xFF) / 255.0f, ((PlateColour(n) >> 8) & 0xFF) / 255.0f,
                                      ((PlateColour(n) >> 16) & 0xFF) / 255.0f, 1.0f };
            context_->OMSetRenderTargets(1, &nativeRtv_, nativeDsv_.Get());
            context_->ClearRenderTargetView(nativeRtv_.Get(), colour);
            ++entry.nativeDraws;
        }
        entry.open = true;
        frames_.push_back(entry);
        FlushGame();
    }
#endif

    // What the engine answers the game side's per-draw question right now.
    bool EngineHidden() const { return ext::hudless::plates::FrameHidden(); }

    // The game's immediate-context work reached the GPU: in the game the bridge's interop fence waits for exactly this
    // same-context order before the frame generator's queue reads the shared image.
    void FlushGame()
    {
        D3D11_QUERY_DESC query {};
        query.Query = D3D11_QUERY_EVENT;
        ComPtr<ID3D11Query> done;
        Check(d3d11_->CreateQuery(&query, &done), "event query");
        context_->End(done.Get());
        context_->Flush();
        BOOL signalled = FALSE;
        for (int i = 0; i < 20000 && !signalled; ++i)
        {
            if (context_->GetData(done.Get(), &signalled, sizeof(signalled), 0) == S_OK)
                break;
            Sleep(1);
        }
        if (!signalled)
            throw std::runtime_error("the game's work did not complete");
    }

    // The bridge reached Present (its present scope), before the backbuffer copy and before any of its early returns:
    // the engine's frame boundary. A present that never reaches the frame finish below still comes through here.
    void PresentBegin()
    {
        plate_test_clock::Advance();
#ifdef PLATES_SEAM_ROUTING
        if (seamRoute_)
        {
            ext::hudless::OnPresentBegin();
            return;
        }
#endif
        ext::hudless::plates::OnPresentBegin();
    }

    // Route the presents and outputs through the product's seam wrappers instead of the engine's entry points, so the
    // wiring between the bridge and the engine is what the scenario drives.
    void ThroughSeams(bool on) { seamRoute_ = on; }

    // The frame finish (the present seam), with the present entry that always precedes it in the bridge.
    void Post()
    {
        PresentBegin();
        if (queueMissing_)
            fg_.queue = nullptr;
#ifdef PLATES_SEAM_ROUTING
        if (seamRoute_)
            ext::hudless::PostInteropCopy(sc_);
        else
            ext::hudless::plates::OnPost(sc_);
#else
        ext::hudless::plates::OnPost(sc_);
#endif
        if (queueMissing_)
        {
            fg_.queue = rig_->queue.Get();
            queueMissing_ = false;
        }
        if (!frames_.empty())
        {
            frames_.back().open = false;
            frames_.back().promoted = ext::hudless::plates::Snapshot().promoted;
        }
    }

    // The present ended without the frame finish (the bridge gave up before the interop copy): the frame keeps its
    // reservation, and the next frame must not inherit it.
    void DropPresent()
    {
        PresentBegin();
        if (!frames_.empty())
        {
            frames_.back().open = false;
            frames_.back().dropped = true;
        }
    }

    unsigned NativeDraws(unsigned n) const
    {
        for (const auto& frame : frames_)
            if (frame.n == n)
                return frame.nativeDraws;
        return 0;
    }

    ext::hudless::plates::FrameMode Mode(unsigned n) const
    {
        for (const auto& frame : frames_)
            if (frame.n == n)
                return frame.mode;
        return ext::hudless::plates::FrameMode::Declined;
    }

    // ---- the presentation side ---------------------------------------------------------------------------------------
    // One output of the batch of frame `batch`: refill the generated frame, run the pass, read the pixel back.
    OutputEntry Output(unsigned batch)
    {
        plate_test_clock::Advance();
        FillBackbuffer();
        if (failOutput_)
        {
            // A backbuffer that cannot be fetched: the pass skips this output after it pinned the slot.
            failOutput_ = false;
            swapChain_.buffer.Reset();
            RunOverlay();
            rig_->WaitIdle();
            swapChain_.buffer = MakeBackbuffer();
            MakeRtvHeap();
            OutputEntry entry;
            entry.batch = batch;
            entry.drawn = 0;
            entry.counters = ext::hudless::plates::Snapshot();
            outputs_.push_back(entry);
            return entry;
        }
        RunOverlay();
        rig_->WaitIdle();
        const auto bytes = rig_->Download(swapChain_.buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, 4);
        const uint32_t pixel = *reinterpret_cast<const uint32_t*>(&bytes[(static_cast<size_t>(height_ / 2) * width_ + width_ / 2) * 4]);
        OutputEntry entry;
        entry.batch = batch;
        entry.pixel = pixel;
        entry.drawn = FrameOf(pixel);
        entry.counters = ext::hudless::plates::Snapshot();
        outputs_.push_back(entry);
        return entry;
    }

    // An output whose GPU work is not allowed to complete yet: the presenting queue is queued behind a fence the test
    // signals later, so the pass's reader fence stays open and the slot it read stays busy (the game side keeps
    // running). FinishDeferred drains the queue and reads the pixel back.
    OutputEntry OutputDeferred(unsigned batch)
    {
        plate_test_clock::Advance();
        FillBackbufferNoWait();
        BlockQueue();
        RunOverlay();
        OutputEntry entry;
        entry.batch = batch;
        entry.pending = true;
        entry.counters = ext::hudless::plates::Snapshot();
        outputs_.push_back(entry);
        return entry;
    }

    // The blocked GPU work is allowed to finish; every deferred output is read back (they share the presented backbuffer,
    // so only the last one's pixels are meaningful: the earlier ones assert on the counters instead).
    void FinishDeferred()
    {
        UnblockQueue();
        rig_->WaitIdle();
        for (auto& entry : outputs_)
        {
            if (!entry.pending)
                continue;
            const auto bytes = rig_->Download(swapChain_.buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, 4);
            entry.pixel = *reinterpret_cast<const uint32_t*>(&bytes[(static_cast<size_t>(height_ / 2) * width_ + width_ / 2) * 4]);
            entry.drawn = FrameOf(entry.pixel);
            entry.counters = ext::hudless::plates::Snapshot();
            entry.pending = false;
        }
    }

    // Queues the presenting queue behind a fence that is signalled only by UnblockQueue: nothing the pass submits
    // completes until then, which is what a frame generator running behind the game looks like to the engine.
    void BlockQueue()
    {
        if (!blocker_)
            Check(rig_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker_)), "blocker fence");
        if (!blockerQueue_)
        {
            // The signal has to come from somewhere that is not behind the wait: a signal queued on the blocked queue
            // itself would sit behind its own wait forever.
            D3D12_COMMAND_QUEUE_DESC desc {};
            desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            Check(rig_->device->CreateCommandQueue(&desc, IID_PPV_ARGS(&blockerQueue_)), "blocker queue");
        }
        ++blockerValue_;
        Check(rig_->queue->Wait(blocker_.Get(), blockerValue_), "blocker wait");
    }

    void UnblockQueue()
    {
        if (!blocker_ || !blockerQueue_ || blockerValue_ == 0)
            return;
        Check(blockerQueue_->Signal(blocker_.Get(), blockerValue_), "blocker signal");
    }

    // ---- lifecycle and settings --------------------------------------------------------------------------------------
    void Release() { ext::hudless::plates::OnRelease(); }
    void Multiplier(unsigned m) { ext::hudless::plates::NoteMultiplier(m); }
    void Interp(ext::hudless::PlateInterp interp, int step = 24)
    {
        ext::hudless::PublishPlateSettings({ interp, 1, step });
#ifdef PLATES_GAME_SIDE
        // The menu's order: the snapshot first, then the bit (the menu never clears it; the engine owns the clear).
        if (interp != ext::hudless::PlateInterp::Off)
            ext::SetActive(ext::F::Plate, true);
#endif
    }
    ext::hudless::plates::Counters Counters() const { return ext::hudless::plates::Snapshot(); }
    std::string LastSkip() const { return ext::hudless::plates::LastSkip(); }
    void FailNextOutput() { failOutput_ = true; }
    void QueueMissing() { queueMissing_ = true; }
    void Hdr(bool on) { bridgeExt_.hdrOutput = on; }
    void ShuttingDown(bool on) { State::Instance().isShuttingDown = on; }
    Dx11wDx12SC& Bridge() { return sc_; }

    // A new output size: the bridge's backbuffer copy and the presented backbuffer change with it.
    void Resize(UINT width, UINT height)
    {
        width_ = width;
        height_ = height;
        swapChain_.buffer.Reset();
        swapChain_.buffer = MakeBackbuffer();
        MakeRtvHeap();
        MakeGameTargets();
    }

    // The bridge has no D3D12 device for one present (a rebuild in progress), then gets it back. A second device cannot
    // be created on WARP: the runtime hands the same device object out again.
    void NoDevice() { sc_._dx12Device = nullptr; }
    void WithDevice() { sc_._dx12Device = rig_->device.Get(); }

    const std::vector<FrameEntry>& Frames() const { return frames_; }
    const std::vector<OutputEntry>& Outputs() const { return outputs_; }
    unsigned Width() const { return width_; }

  private:
    // The overlay seam, or the engine's entry point when the scenario asked for the wiring to be exercised.
    void RunOverlay()
    {
#ifdef PLATES_SEAM_ROUTING
        if (seamRoute_)
        {
            ext::present::BeforeOverlay(&swapChain_, rig_->queue.Get(), false);
            return;
        }
#endif
        ext::hudless::plates::OnOverlay(&swapChain_, rig_->queue.Get());
    }

    // The presented backbuffer: the "generated frame" the pass draws over, in PRESENT state like a real swapchain one.
    ComPtr<ID3D12Resource> MakeBackbuffer()
    {
        auto texture = rig_->Texture(width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                     D3D12_RESOURCE_STATE_PRESENT);
        return texture;
    }

    void MakeRtvHeap()
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        rtvHeap_.Reset();
        Check(rig_->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtvHeap_)), "backbuffer RTV heap");
        D3D12_RENDER_TARGET_VIEW_DESC view {};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rig_->device->CreateRenderTargetView(swapChain_.buffer.Get(), &view,
                                             rtvHeap_->GetCPUDescriptorHandleForHeapStart());
    }

    // The game's own colour target and depth buffer, and the bridge's D3D11 backbuffer copy (the slot's size source).
    void MakeGameTargets()
    {
        D3D11_TEXTURE2D_DESC colour {};
        colour.Width = width_;
        colour.Height = height_;
        colour.MipLevels = colour.ArraySize = 1;
        colour.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        colour.SampleDesc.Count = 1;
        colour.Usage = D3D11_USAGE_DEFAULT;
        colour.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        nativeTarget_.Reset();
        nativeRtv_.Reset();
        Check(d3d11_->CreateTexture2D(&colour, nullptr, &nativeTarget_), "native colour target");
        Check(d3d11_->CreateRenderTargetView(nativeTarget_.Get(), nullptr, &nativeRtv_), "native render target view");
        D3D11_TEXTURE2D_DESC depth = colour;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        nativeDepth_.Reset();
        nativeDsv_.Reset();
        Check(d3d11_->CreateTexture2D(&depth, nullptr, &nativeDepth_), "native depth target");
        Check(d3d11_->CreateDepthStencilView(nativeDepth_.Get(), nullptr, &nativeDsv_), "native depth-stencil view");
        // The bridge's shared D3D11 copy of the backbuffer: the engine sizes its slots from it.
        ComPtr<ID3D11Texture2D> backbuffer;
        Check(d3d11_->CreateTexture2D(&colour, nullptr, &backbuffer), "backbuffer copy");
        sc_._sharedDx11BackBufferCopies.assign(1, backbuffer.Get());
        backbufferCopy_ = backbuffer;
        if (swapChain_.buffer)
            MakeRtvHeap();
    }

    // Refill the generated frame with the background the pass must leave alone outside the plate image.
    void FillBackbuffer()
    {
        if (!swapChain_.buffer)
            return;
        rig_->Run(
            [&](ID3D12GraphicsCommandList* list)
            {
                RecordBackground(list);
            });
    }

    // The same fill, submitted without waiting for the GPU: used while the presenting queue is deliberately held, where
    // a wait would never return. The fill keeps its own allocators and lists so it never resets one that is in flight.
    void FillBackbufferNoWait()
    {
        if (!swapChain_.buffer)
            return;
        if (fillLists_.empty())
        {
            for (int i = 0; i < 8; ++i)
            {
                ComPtr<ID3D12CommandAllocator> allocator;
                ComPtr<ID3D12GraphicsCommandList> list;
                Check(rig_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                      "fill allocator");
                Check(rig_->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                      IID_PPV_ARGS(&list)),
                      "fill list");
                Check(list->Close(), "fill list close");
                fillLists_.push_back(FillSlot { allocator, list });
            }
        }
        auto& entry = fillLists_[fillRing_];
        fillRing_ = (fillRing_ + 1) % fillLists_.size();
        Check(entry.allocator->Reset(), "fill allocator reset");
        Check(entry.list->Reset(entry.allocator.Get(), nullptr), "fill list reset");
        RecordBackground(entry.list.Get());
        Check(entry.list->Close(), "fill list close");
        ID3D12CommandList* lists[] = { entry.list.Get() };
        rig_->queue->ExecuteCommandLists(1, lists);
    }

    void RecordBackground(ID3D12GraphicsCommandList* list)
    {
        Rig::Barrier(list, swapChain_.buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                     D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float colour[4] = { ((kBackground >> 0) & 0xFF) / 255.0f, ((kBackground >> 8) & 0xFF) / 255.0f,
                                  ((kBackground >> 16) & 0xFF) / 255.0f, 1.0f };
        list->ClearRenderTargetView(rtvHeap_->GetCPUDescriptorHandleForHeapStart(), colour, 0, nullptr);
        Rig::Barrier(list, swapChain_.buffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_PRESENT);
    }

    UINT width_;
    UINT height_;
    ComPtr<ID3D11Device> d3d11_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> nativeTarget_;
    ComPtr<ID3D11RenderTargetView> nativeRtv_;
    ComPtr<ID3D11Texture2D> nativeDepth_;
    ComPtr<ID3D11DepthStencilView> nativeDsv_;
    ComPtr<ID3D11Texture2D> backbufferCopy_;
    std::unique_ptr<Rig> primary_;
    Rig* rig_ = nullptr;
    FakeSwapChain swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    FakeFg fg_;
    FakeBridgeExt bridgeExt_;
    Dx11wDx12SC sc_;
    std::vector<FrameEntry> frames_;
    std::vector<OutputEntry> outputs_;
    bool failOutput_ = false;
    bool queueMissing_ = false;
    bool seamRoute_ = false;
    ComPtr<ID3D12Fence> blocker_;
    ComPtr<ID3D12CommandQueue> blockerQueue_;
    UINT64 blockerValue_ = 0;
    struct FillSlot
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
    };
    std::vector<FillSlot> fillLists_;
    size_t fillRing_ = 0;
#ifdef PLATES_GAME_SIDE
    std::unique_ptr<plate_game_test::DrawStateRig> state_;
    unsigned gameFrame_ = 0;
    int nativeDraws_ = 0;
    int slotDraws_ = 0;
#endif
  private:
};

// Every output that received frame n's image, in order.
inline std::vector<unsigned> DrawnFrames(const std::vector<OutputEntry>& outputs)
{
    std::vector<unsigned> drawn;
    for (const auto& output : outputs)
        drawn.push_back(output.drawn);
    return drawn;
}
} // namespace plate_test
