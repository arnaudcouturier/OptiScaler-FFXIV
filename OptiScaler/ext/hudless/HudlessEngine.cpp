#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/game/Game.h"
#include "ext/gpu/Gpu.h"
#include "ext/hdr/Hdr10.h"
#include "ext/hdr/screenshot/HdrScreenshot.h"
#include "ext/hudless/HudlessAccess.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/HudlessEngine.h"
#include "ext/hudless/HudlessGpu.h"
#include "ext/hudless/SharedSlot.h"
#include "ext/hudless/adapters/Adapters.h"

#include <Config.h>
#include <State.h>
#include <hooks/FG_Hooks.h>

#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <vector>

// S10 package B: the HUD-less engine (docs/HUDLESS.md). The decision logic is HudlessCore.h; this file is its host: the
// slot ring of shared D3D11 -> D3D12 textures with per-slot reader fences, the base swap, the tags, the paste producer
// and the seam bodies. Everything per present runs on the game's Present thread; only the menu (GetStatus) and the output
// presents (BeforeOverlay) come from other threads.

using Microsoft::WRL::ComPtr;

namespace ext::hudless
{
namespace
{
// The typed format of a capture / backbuffer: the copy between an sRGB or typeless view and the typed one is legal
// (same typeless family), so compare and size on this.
DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

Provider ProviderOf(FGOutput output)
{
    switch (output)
    {
    case FGOutput::DLSSG:
        return Provider::DlssG;
    case FGOutput::FSRFG:
        return Provider::FsrFg;
    case FGOutput::XeFG:
        return Provider::XeFg;
    default:
        return Provider::None;
    }
}

class Engine final : public IEngineHost, private FenceReader
{
  public:
    // Never destroyed: no destructor may run under the loader lock, and the GPU may outlive the process teardown.
    static Engine& Get()
    {
        static Engine* engine = new Engine();
        return *engine;
    }

    // ---- seam bodies --------------------------------------------------------------------------------------------------
    void Pre(Dx11wDx12SC& sc);
    void SwapBase(Dx11wDx12SC& sc, ID3D12Resource*& source, D3D12_RESOURCE_STATES& state);
    void Post(Dx11wDx12SC& sc);
    void Release(Dx11wDx12SC& sc);
    void Overlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);

    Status GetStatus()
    {
        std::lock_guard lock(_statusMutex);
        return _status;
    }
    Recomposition QueryRecomposition();

    // ---- IEngineHost --------------------------------------------------------------------------------------------------
    bool FrameGenActive() override;
    Provider FrameGenProvider() override { return ProviderOf(State::Instance().activeFgOutput); }
    bool HdrOutput() override { return Access::Hudless::HdrOutput(*_sc); }
    bool DebugCompareOn() override { return State::Instance().fgHudlessCompare; }
    float CoverageLimit() override { return ext::Cfg().hudless.CoverageLimit(); }
    GuardPolicy CoverageGuardPolicy() override { return ext::Cfg().hudless.GetGuardPolicy(); }
    int CoverageGuardReleaseHolds() override { return ext::Cfg().hudless.ReleaseHolds(); }
    bool CoverageGuardTripOnAnchorLater() override { return ext::Cfg().hudless.TripOnAnchorLater(); }
    IHudlessSource* Source() override { return ext::game::Current().hudless; }
    Reason CheckCapture(const FrameImage& image) override;
    Reason StageCapture(const FrameImage& image) override;
    bool PasteReady() override;
    bool SceneHdrOwnsFrame() override;
    bool PausedByPreset() override;
    void Log(const std::string& line) override { LOG_INFO("{}", line); }

  private:
    Engine() : _core(*this) {}

    // Completed value of a fence the ring tracks (FenceReader)
    uint64_t Completed(const void* fence) override
    {
        return static_cast<ID3D12Fence*>(const_cast<void*>(fence))->GetCompletedValue();
    }

    static constexpr size_t kSlots = BUFFER_COUNT;

    struct Slot
    {
        SharedTexture shared;            // the D3D11 texture (game device, written by the immediate context) and its D3D12 view
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
        // HDR10 output + ui_free: the PQ copy of the final image the paste samples, and the recording that makes it
        ComPtr<ID3D12Resource> hdrFinal;
        ComPtr<ID3D12CommandAllocator> hdrAllocator;
        ComPtr<ID3D12GraphicsCommandList> hdrList;
    };

    // Resources of a torn-down ring, kept alive until every recorded reader fence reached its value
    struct Retired
    {
        std::vector<ComPtr<IUnknown>> objects;
        std::vector<std::pair<ComPtr<ID3D12Fence>, uint64_t>> readers;
    };

    void Bind(Dx11wDx12SC& sc);
    bool EnsureSlot(size_t index, UINT width, UINT height, DXGI_FORMAT format);
    bool EnsureHdrStage(size_t index);
    bool EnsureHdrStageOnce(size_t index);
    ID3D12Resource* BuildHdrFinal(Dx11wDx12SC& sc, Slot& slot, ID3D12Resource* finalImage, ID3D12CommandQueue* queue);
    void AddReader(size_t index, ID3D12Fence* fence, uint64_t value);
    void PinFence(ID3D12Fence* fence);
    void RetireSlot(size_t index);
    void RetireAll();
    void PollRetired();
    void OnModeOff();
    void SyncPasteOptions();
    void Finish();
    bool ProducePaste(Dx11wDx12SC& sc, Slot& slot, ID3D12Resource* finalImage, IFGFeature_Dx12* fg);
    void NoteRefusal(const std::string& why);

    std::vector<std::string> _refusals; // texts NoteRefusal already logged
    HdrStageLatch _hdrLatch;            // HdrPaste = on: a failed creation of the HDR stage is not retried on every present
    Core _core;
    SlotRing<kSlots> _ring;
    std::array<Slot, kSlots> _slots;
    std::vector<ComPtr<ID3D12Fence>> _pinned; // keeps the fences the ring points at alive
    std::vector<Retired> _retired;
    ComPtr<ID3D12Fence> _produceFence;        // signalled on the FG queue right after UiPaste::Produce
    uint64_t _produceValue = 0;

    Dx11wDx12SC* _bridge = nullptr; // the bridge the ring belongs to
    ID3D12Device* _device12 = nullptr;
    Dx11wDx12SC* _sc = nullptr;     // the bridge of the present in progress (null between presents)
    uint64_t _frame = 0;
    size_t _next = 0;
    int _stagedSlot = -1;
    bool _baseSwapped = false;
    bool _everAllocated = false;
    bool _tintSynced = false;
    bool _tint = false;
    bool _steady = false;

    std::atomic<uint32_t> _lastFormat { 0 };   // typed format of the last staged capture
    std::atomic<uint32_t> _bridgeFormat { 0 }; // typed format of the bridge's backbuffer
    std::atomic<bool> _bridgeSeen { false };   // a bridge Present ran: its Pre owns the feature bit from then on

    std::mutex _statusMutex;
    Status _status;
};

// ---- the host side of one present -----------------------------------------------------------------------------------------
bool Engine::FrameGenActive()
{
    auto* fg = Access::Hudless::Fg(*_sc);
    return fg != nullptr && fg->IsActive() && !fg->IsPaused() && Config::Instance()->IsFGEnabled();
}

// The same rule as the DLSS-G option seam (ext/fg/dlssg/UiRecompositionRule.h): the preset takes the recomposition away. DLSS-G provider only,
// and only when recomposition was wanted in the first place (mode, a hud-less source for the game build, [DLSSG] UIRecomposition): otherwise
// there is nothing to take away and the engine reports its own reason.
bool Engine::PausedByPreset() { return FrameGenProvider() == Provider::DlssG && ext::fg::preset::CurrentUiRecompositionDecision(QueryRecomposition().enabled).droppedByPreset; }

// One log line per distinct refusal text (the texts are a handful of fixed strings): a refusal repeats every present and the
// summary only counts the reasons, so the cause of a CaptureRejected is written once, the first time it happens.
void Engine::NoteRefusal(const std::string& why)
{
    for (const auto& seen : _refusals)
        if (seen == why)
            return;
    if (_refusals.size() >= 16) // numbers in the text could otherwise grow the list on every resize
        return;
    _refusals.push_back(why);
    LOG_WARN("HudLess: the UI-free capture was refused: {}", why);
}

Reason Engine::CheckCapture(const FrameImage& image)
{
    if (image.texture == nullptr)
    {
        NoteRefusal("the live source delivered no texture");
        return Reason::CaptureRejected;
    }

    D3D11_TEXTURE2D_DESC desc {};
    image.texture->GetDesc(&desc); // the texture is the truth, not the FrameImage fields
    if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
        return Reason::MsaaOrArray;

    // The bridge copied this frame's backbuffer into its shared texture before the seam: size and format must match it.
    ID3D11Texture2D* backbuffer = Access::Hudless::SharedBackBuffer(*_sc, Access::Hudless::FakeIndex(*_sc));
    if (backbuffer == nullptr)
    {
        NoteRefusal("the bridge has no shared backbuffer copy for this frame");
        return Reason::CaptureRejected;
    }

    // Both textures name the device they live on. The bridge's own device pointer is not comparable with that: the game's
    // device may be a wrapper (ReShade proxies it), while a texture always reports the native device it was created on. Two
    // textures on one device is exactly what CopyResource needs.
    ComPtr<ID3D11Device> imageDevice;
    ComPtr<ID3D11Device> bridgeDevice;
    image.texture->GetDevice(&imageDevice);
    backbuffer->GetDevice(&bridgeDevice);
    if (imageDevice == nullptr || imageDevice.Get() != bridgeDevice.Get())
    {
        NoteRefusal("the captured image and the bridge's backbuffer copy live on different D3D11 devices");
        return Reason::CaptureRejected;
    }

    D3D11_TEXTURE2D_DESC target {};
    backbuffer->GetDesc(&target);

    if (desc.Width != target.Width || desc.Height != target.Height)
    {
        NoteRefusal(std::format("size mismatch: capture {}x{}, bridge backbuffer copy {}x{}", desc.Width, desc.Height,
                                target.Width, target.Height));
        return Reason::SizeMismatch;
    }
    if (TypedFormat(desc.Format) != TypedFormat(target.Format))
    {
        NoteRefusal(std::format("format mismatch: capture format {}, bridge backbuffer copy format {}",
                                static_cast<UINT>(desc.Format), static_cast<UINT>(target.Format)));
        return Reason::FormatUnsupported;
    }
    return Reason::None;
}

Reason Engine::StageCapture(const FrameImage& image)
{
    auto* context = Access::Hudless::Dx11Context(*_sc);
    if (context == nullptr || Access::Hudless::Dx11Device(*_sc) == nullptr || _device12 == nullptr)
    {
        NoteRefusal("the bridge has no D3D11 context or D3D12 device to stage the capture with");
        return Reason::CaptureRejected;
    }

    if (_produceFence == nullptr)
    {
        if (FAILED(_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_produceFence))))
        {
            NoteRefusal("the D3D12 producer fence could not be created");
            return Reason::CaptureRejected;
        }
        PinFence(_produceFence.Get());
        _produceValue = 0;
    }

    D3D11_TEXTURE2D_DESC desc {};
    image.texture->GetDesc(&desc);
    const DXGI_FORMAT format = TypedFormat(desc.Format);

    // Never wait: a slot whose readers have not all completed is skipped, and if every slot is busy this present has
    // no hud-less image.
    const int slot = _ring.Acquire(_next, _frame, *this);
    if (slot < 0)
        return Reason::SlotBusy;

    if (!EnsureSlot(static_cast<size_t>(slot), desc.Width, desc.Height, format))
    {
        NoteRefusal("the shared hud-less slot could not be created (see the error line above)");
        return Reason::CaptureRejected;
    }

    // Same D3D11 immediate context, before the bridge signals its fence: one signal covers both copies.
    context->CopyResource(_slots[slot].shared.texture.Get(), image.texture.Get());

    _stagedSlot = slot;
    _next = (static_cast<size_t>(slot) + 1) % kSlots;
    _lastFormat.store(static_cast<uint32_t>(format), std::memory_order_relaxed);
    return Reason::None;
}

bool Engine::PasteReady()
{
    if (_stagedSlot < 0)
        return false;

    auto& state = State::Instance();
    if (state.isShuttingDown || state.activeFgOutput != FGOutput::DLSSG)
        return false;

    // The paste is drawn by the frame generation swapchain's own present hook: a plain interop presenter never pastes, so
    // the UI must not be removed from the base on such a chain.
    IDXGISwapChain* chain = Access::Hudless::FgSwapChain(*_sc);
    if (state.currentFGSwapchain != chain || FGHooks::IsDx12InteropPresentSC(chain))
        return false;

    auto* fg = Access::Hudless::Fg(*_sc);
    if (fg == nullptr || fg->GetCommandQueue() == nullptr)
        return false;

    const Slot& slot = _slots[static_cast<size_t>(_stagedSlot)];
    if (!UiPaste::Ready(_device12, slot.width, slot.height))
        return false;

    // Every later step of the producer is prepared now: after the swap there is no way back for this present.
    return !HdrOutput() || EnsureHdrStage(static_cast<size_t>(_stagedSlot));
}

bool Engine::SceneHdrOwnsFrame()
{
    if (!HdrOutput())
        return false;

    // The HDR PNG of the frame is taken from the copy source and would lose the UI.
    if (ext::hdr::screenshot::Pending())
        return true;

    // Scene HDR (mode 1) keeps the frames it supplies a scene for. WouldSupplyScene is the side-effect-free twin of the
    // Open() the bridge makes for the same frame: it counts no request and caches nothing, so ui_free does not double the
    // scene-HDR statistics. Only asked when scene HDR is requested at all.
    auto* scene = ext::game::Current().hdr;
    if (scene == nullptr || ext::Cfg().hdr.mode.value_or_default() != 1)
        return false;

    const Slot& slot = _slots[static_cast<size_t>(_stagedSlot)];
    return scene->WouldSupplyScene(slot.width, slot.height);
}

// ---- ring ------------------------------------------------------------------------------------------------------------------
void Engine::Bind(Dx11wDx12SC& sc)
{
    ID3D12Device* device = Access::Hudless::Dx12Device(sc);
    if (_bridge == &sc && _device12 == device)
        return;

    // A different bridge or device: whatever the old one still had in flight is retired behind its own fences.
    if (_bridge != nullptr)
        RetireAll();
    _bridge = &sc;
    _device12 = device;
}

bool Engine::EnsureSlot(size_t index, UINT width, UINT height, DXGI_FORMAT format)
{
    Slot& slot = _slots[index];
    if (slot.shared.texture != nullptr && slot.shared.resource != nullptr && slot.width == width &&
        slot.height == height && slot.format == format)
        return true;

    // The slot is free (Acquire): nothing reads the old resources any more.
    RetireSlot(index);

    const HRESULT hr =
        CreateSharedTexture(Access::Hudless::Dx11Device(*_sc), _device12, width, height, format, slot.shared);
    if (FAILED(hr))
    {
        LOG_ERROR("HudLess: could not create the shared hud-less slot {} ({}x{}, format {}): {:X}", index, width, height,
                  static_cast<UINT>(format), static_cast<UINT>(hr));
        return false;
    }

    slot.shared.resource->SetName(std::format(L"HudLess slot [{}]", index).c_str());
    slot.format = format;
    slot.width = width;
    slot.height = height;
    _everAllocated = true;
    LOG_INFO("HudLess: slot {} created, {}x{} format {}", index, width, height, static_cast<UINT>(format));
    return true;
}

bool Engine::EnsureHdrStage(size_t index)
{
    Slot& slot = _slots[index];
    if (slot.hdrFinal != nullptr && slot.hdrFinal->GetDesc().Width == slot.width &&
        slot.hdrFinal->GetDesc().Height == slot.height && slot.hdrList != nullptr)
        return true;

    // The old PQ image / list are free (the slot is): drop them and rebuild at the new size.
    slot.hdrList.Reset();
    slot.hdrAllocator.Reset();
    slot.hdrFinal.Reset();

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = slot.width;
    desc.Height = slot.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM; // PQ, the format the paste stores in HDR (docs/HUDLESS.md 8.3)
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT hr = _device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                    nullptr, IID_PPV_ARGS(&slot.hdrFinal));
    if (SUCCEEDED(hr))
        hr = _device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.hdrAllocator));
    if (SUCCEEDED(hr))
        hr = _device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.hdrAllocator.Get(), nullptr,
                                          IID_PPV_ARGS(&slot.hdrList));
    if (SUCCEEDED(hr))
        hr = slot.hdrList->Close();

    if (FAILED(hr))
    {
        LOG_ERROR("HudLess: could not create the HDR paste stage of slot {}: {:X}", index, static_cast<UINT>(hr));
        slot.hdrList.Reset();
        slot.hdrAllocator.Reset();
        slot.hdrFinal.Reset();
        return false;
    }
    slot.hdrFinal->SetName(std::format(L"HudLess HDR final [{}]", index).c_str());
    return true;
}

// The producer's way to the stage: with HdrPaste off the plain EnsureHdrStage (retried whenever reached, as before); with on a failed
// creation is not tried again until the HDR output state or the setting changes, and says so once.
bool Engine::EnsureHdrStageOnce(size_t index)
{
    return EnsureStageLatched(_hdrLatch, HdrOutput(), ext::Cfg().hudless.HdrPasteOn(), [&] { return EnsureHdrStage(index); },
                              []
                              {
                                  LOG_WARN("HudLess: HdrPaste: the HDR paste stage could not be created, not trying again until the HDR "
                                           "output state or the setting changes");
                              });
}

// Converts the final image to PQ with the production OptiHDR converter (the same call the bridge makes on its copy source)
// on an engine-owned list that executes on the FG queue before UiPaste::Produce submits, and keeps the result in an
// engine-owned image (the converter's pooled image may be reused as soon as the list completes, before the producer ran).
ID3D12Resource* Engine::BuildHdrFinal(Dx11wDx12SC&, Slot& slot, ID3D12Resource* finalImage, ID3D12CommandQueue* queue)
{
    if (slot.hdrAllocator == nullptr || slot.hdrList == nullptr || slot.hdrFinal == nullptr ||
        FAILED(slot.hdrAllocator->Reset()))
        return nullptr;

    ext::gpu::Scope scope(slot.hdrList.Get(), slot.hdrAllocator.Get());
    if (!scope)
        return nullptr;

    // Same inputs as the base's conversion in a frame without a scene: empty SceneInput (SceneHdrOwnsFrame frames never
    // get here), so the paste and the base share one colour treatment.
    ID3D12Resource* converted = ext::hdr::Convert(_device12, slot.hdrList.Get(), finalImage, D3D12_RESOURCE_STATE_COMMON);
    if (converted == nullptr)
        return nullptr;

    Barrier(slot.hdrList.Get(), converted, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(slot.hdrList.Get(), slot.hdrFinal.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    slot.hdrList->CopyResource(slot.hdrFinal.Get(), converted);
    Barrier(slot.hdrList.Get(), slot.hdrFinal.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(slot.hdrList.Get(), converted, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (FAILED(scope.Submit(queue)))
        return nullptr;
    return slot.hdrFinal.Get();
}

void Engine::PinFence(ID3D12Fence* fence)
{
    if (fence == nullptr)
        return;
    for (const auto& pinned : _pinned)
        if (pinned.Get() == fence)
            return;
    _pinned.emplace_back(fence);
}

void Engine::AddReader(size_t index, ID3D12Fence* fence, uint64_t value)
{
    if (fence == nullptr || value == 0)
        return;
    PinFence(fence);
    if (!_ring.AddReader(index, fence, value, _frame))
        LOG_WARN("HudLess: slot {} tracks too many fences, a reader was not recorded", index);
}

void Engine::RetireSlot(size_t index)
{
    Slot& slot = _slots[index];

    Retired retired;
    const void* fence = nullptr;
    uint64_t value = 0;
    for (size_t i = 0; _ring.Reader(index, i, fence, value); ++i)
        retired.readers.emplace_back(ComPtr<ID3D12Fence>(static_cast<ID3D12Fence*>(const_cast<void*>(fence))), value);
    _ring.Clear(index);

    auto keep = [&](auto& pointer)
    {
        if (pointer == nullptr)
            return;
        retired.objects.emplace_back();
        retired.objects.back().Attach(pointer.Detach()); // moves the reference, no AddRef
    };
    keep(slot.hdrList);
    keep(slot.hdrAllocator);
    keep(slot.hdrFinal);
    keep(slot.shared.resource);
    keep(slot.shared.texture);
    if (slot.shared.handle != nullptr)
    {
        CloseHandle(slot.shared.handle);
        slot.shared.handle = nullptr;
    }
    slot.format = DXGI_FORMAT_UNKNOWN;
    slot.width = slot.height = 0;

    if (retired.objects.empty())
        return;
    if (!retired.readers.empty())
        _retired.push_back(std::move(retired)); // PollRetired releases it once its fences completed
}

void Engine::RetireAll()
{
    for (size_t i = 0; i < kSlots; ++i)
        RetireSlot(i);
    _ring.ClearAll();
    _pinned.clear();
    _produceFence.Reset(); // the retired readers keep their own references to whatever fences matter
    _produceValue = 0;
    _stagedSlot = -1;
    _baseSwapped = false;
    _next = 0;
    _hdrLatch.Clear();
    UiPaste::Release();
    UiImage::Release();
}

void Engine::PollRetired()
{
    for (size_t i = 0; i < _retired.size();)
    {
        bool done = true;
        for (const auto& [fence, value] : _retired[i].readers)
        {
            const uint64_t completed = fence->GetCompletedValue();
            // UINT64_MAX = the device was removed: nothing can still be reading, releasing is safe
            if (completed != UINT64_MAX && completed < value)
            {
                done = false;
                break;
            }
        }
        if (done)
            _retired.erase(_retired.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
}

void Engine::OnModeOff()
{
    if (_everAllocated || _bridge != nullptr)
    {
        if (_everAllocated)
            LOG_INFO("HudLess: mode off, releasing the hud-less slots");
        RetireAll();
        _bridge = nullptr;
        _device12 = nullptr;
        _everAllocated = false;
    }
    PollRetired();

    // Retired resources still waiting for their fences keep the seams alive; otherwise the inline guard can go quiet.
    if (_retired.empty())
    {
        ext::SetActive(ext::F::Hudless, false);
        if (CurrentMode() != Mode::Off) // the menu switched the mode on in the meantime
            ext::SetActive(ext::F::Hudless, true);
    }
}

void Engine::SyncPasteOptions()
{
    const bool tint = ext::Cfg().hudless.showDetectedUi.value_or_default();
    const bool steady = ext::Cfg().hudless.steadyTiming.value_or_default();
    if (!_tintSynced || tint != _tint)
        UiPaste::SetDebugTint(tint);
    if (!_tintSynced || steady != _steady)
        UiPaste::SetSteadyTiming(steady);
    _tint = tint;
    _steady = steady;
    _tintSynced = true;
}

void Engine::Finish()
{
    _core.EndPresent();
    {
        std::lock_guard lock(_statusMutex);
        _status = _core.Snapshot();
    }
    _sc = nullptr;
}

// ---- the three seams of one present ------------------------------------------------------------------------------------------
void Engine::Pre(Dx11wDx12SC& sc)
{
    if (State::Instance().isShuttingDown)
        return;

    _bridgeSeen.store(true, std::memory_order_relaxed);
    const Mode mode = CurrentMode();
    _sc = &sc;
    _stagedSlot = -1;
    _baseSwapped = false;

    if (mode == Mode::Off)
    {
        // Logs the start-up mode line on the very first entry; nothing else is touched or allocated.
        _core.BeginPresent(mode, {});
        OnModeOff();
        _sc = nullptr;
        return;
    }

    Bind(sc);
    ++_frame;
    _bridgeFormat.store(static_cast<uint32_t>(TypedFormat(Access::Hudless::BufferFormat(sc))), std::memory_order_relaxed);
    PollRetired();

    PasteCounters paste;
    if (mode == Mode::UiFree)
    {
        const PasteStatus pasteStatus = UiPaste::Snapshot();
        paste = { pasteStatus.produced, pasteStatus.pasted, pasteStatus.skipped, pasteStatus.coverage,
                  pasteStatus.coverage008, pasteStatus.coverage05, pasteStatus.coverage2, pasteStatus.coverageSequence };
        SyncPasteOptions();
    }

    _core.BeginPresent(mode, paste);
}

void Engine::SwapBase(Dx11wDx12SC& sc, ID3D12Resource*& source, D3D12_RESOURCE_STATES& state)
{
    if (_sc != &sc || _stagedSlot < 0 || !_core.Staged() || !_core.DecideBase())
        return;

    // The copy writes into the frame generation swapchain's backbuffer: same size, and (SDR) the same format family. In HDR
    // the converter writes PQ whatever the source was.
    Slot& slot = _slots[static_cast<size_t>(_stagedSlot)];
    DXGI_SWAP_CHAIN_DESC1 desc {};
    if (IDXGISwapChain4* chain = Access::Hudless::FgSwapChain(sc); chain == nullptr || FAILED(chain->GetDesc1(&desc)))
    {
        NoteRefusal("the frame generation swapchain is missing or could not be described");
        _core.Reject(Reason::CaptureRejected);
        return;
    }
    if (desc.Width != slot.width || desc.Height != slot.height)
    {
        NoteRefusal(std::format("base swap: size mismatch: hud-less slot {}x{}, frame generation swapchain {}x{}", slot.width,
                                slot.height, desc.Width, desc.Height));
        _core.Reject(Reason::SizeMismatch);
        return;
    }
    if (!HdrOutput() && TypedFormat(desc.Format) != slot.format)
    {
        NoteRefusal(std::format("base swap: format mismatch: hud-less slot format {}, frame generation swapchain format {}",
                                static_cast<UINT>(slot.format), static_cast<UINT>(TypedFormat(desc.Format))));
        _core.Reject(Reason::FormatUnsupported);
        return;
    }

    source = slot.shared.resource.Get();
    state = D3D12_RESOURCE_STATE_COMMON; // shared textures rest in COMMON between uses
    _baseSwapped = true;
}

bool Engine::ProducePaste(Dx11wDx12SC& sc, Slot& slot, ID3D12Resource* finalImage, IFGFeature_Dx12* fg)
{
    ID3D12CommandQueue* queue = fg != nullptr ? fg->GetCommandQueue() : nullptr;
    if (queue == nullptr || finalImage == nullptr || _device12 == nullptr || _produceFence == nullptr)
        return false;

    const bool hdr = HdrOutput();
    ID3D12Resource* hdrFinal = nullptr;
    if (hdr)
    {
        hdrFinal = BuildHdrFinal(sc, slot, finalImage, queue);
        if (hdrFinal == nullptr)
            return false;
    }

    MaskParams params;
    params.threshold = ext::Cfg().hudless.Threshold();
    params.dilation = ext::Cfg().hudless.Dilation();
    params.hdr = hdr;
    // A UI-free base has no ghost of a closed window to clear (docs/HUDLESS.md 7)
    params.cleanup = ext::Cfg().hudless.pasteCleanup.value_or_default() && !_baseSwapped;

    // The same queue that waited for the interop copy, so both inputs are complete. Submitted before the frame generator
    // executes its UI list and DLSS-G's own work.
    UiPaste::Produce(_device12, queue, finalImage, TypedFormat(finalImage->GetDesc().Format), slot.shared.resource.Get(),
                     slot.format, hdrFinal, params);

    // Whatever Produce submitted, and the HDR stage list, precede this signal on the queue.
    if (SUCCEEDED(queue->Signal(_produceFence.Get(), _produceValue + 1)))
        AddReader(static_cast<size_t>(_stagedSlot), _produceFence.Get(), ++_produceValue);
    else
        LOG_WARN("HudLess: could not signal the paste producer fence; slot {} stays unreleased by this reader", _stagedSlot);
    return true;
}

void Engine::Post(Dx11wDx12SC& sc)
{
    if (_sc != &sc)
        return;

    const Mode mode = _core.CurrentMode();
    if (mode == Mode::Off)
    {
        _sc = nullptr;
        return;
    }

    auto* fg = Access::Hudless::Fg(sc);
    if (!_core.Staged() || _stagedSlot < 0)
    {
        // Exactly one of Produce / Invalidate / Hold per present: no usable pair, so stop pasting (the normal base carries
        // its UI), but not yet in the first presents after a swapped one (Hold: the ring stays, the paste stays visible)
        const PasteAction action = _core.PasteDecision();
        if (action != PasteAction::None)
            UiPaste::SetPasteEnabled(_core.PasteVisible());
        if (action == PasteAction::Invalidate)
            UiPaste::Invalidate(ReasonText(_core.PresentReason()));
        Finish();
        return;
    }

    const size_t index = static_cast<size_t>(_stagedSlot);
    Slot& slot = _slots[index];
    ID3D12Resource* finalImage = Access::Hudless::OpenedBackBuffer(sc, Access::Hudless::FakeIndex(sc));

    // The interop copy list read the slot when it was the base: it completes with the copy fence value of that copy.
    if (_baseSwapped)
        AddReader(index, Access::Hudless::CopyFence(sc), Access::Hudless::LastCopyValue(sc));

    // Tags. Not over HDR10 output: the base is already PQ, a hud-less SDR tag would be converted behind our back (and held
    // raw by scene HDR), and DLSS-G refuses an SDR UI image there (docs/HUDLESS.md 8). In ui_free only when the base really
    // was swapped to the hud-less image: a paste-not-ready or probing present keeps the normal base, and a hud-less tag
    // next to it would hand DLSS-G two images of the same frame that differ everywhere in a fade. Without the tag it gets
    // what it gets with the mode off. Recomposition tags every present (Core::WantsTags).
    if (fg != nullptr && _core.WantsTags() && !HdrOutput() && finalImage != nullptr)
    {
        adapters::TagContext context;
        context.device = _device12;
        context.hudless = slot.shared.resource.Get();
        context.final = finalImage;
        context.hudlessFormat = slot.format;
        context.finalFormat = TypedFormat(finalImage->GetDesc().Format);
        context.width = slot.width;
        context.height = slot.height;
        context.recomposition = mode == Mode::Recomposition;
        context.mask.threshold = ext::Cfg().hudless.Threshold();
        context.mask.dilation = ext::Cfg().hudless.Dilation();
        context.mask.hdr = false;
        context.mask.cleanup = true;

        adapters::TagResult tagged;
        switch (FrameGenProvider())
        {
        case Provider::DlssG:
            tagged = adapters::DlssgTag(*fg, context);
            break;
        case Provider::FsrFg:
            tagged = adapters::FsrFgTag(*fg, context);
            break;
        case Provider::XeFg:
            tagged = adapters::XeFgTag(*fg, context);
            break;
        default:
            break;
        }
        AddReader(index, tagged.readerFence, tagged.readerValue);
        _core.NoteTagSet(static_cast<uint8_t>((tagged.hudlessTagged ? 1 : 0) | (tagged.uiTagged ? 2 : 0)));

        // In ui_free the base is the hud-less image (or the normal frame): a refused tag costs nothing. In recomposition
        // the tags ARE the feature.
        if (mode == Mode::Recomposition && (!tagged.listAvailable || !tagged.hudlessTagged || (tagged.uiWanted && !tagged.uiTagged)))
            _core.Fail(Reason::CaptureRejected);
    }

    const PasteAction action = _core.PasteDecision();
    if (action != PasteAction::None)
        UiPaste::SetPasteEnabled(_core.PasteVisible());
    bool produced = true;
    if (action == PasteAction::Produce)
    {
        // A probing present never asked PasteReady, the only other caller of EnsureHdrStage: under HDR output the
        // conversion resources of this slot have to be there before the paste image can be built. [HudLess] HdrPaste = on
        // does the same on every HDR present (PasteReady only gets there after a first image exists); see HudlessLogic.h.
        _hdrLatch.Observe(HdrOutput(), ext::Cfg().hudless.HdrPasteOn());
        produced = ProduceWithHdrStage(HdrOutput(), _core.Probing(), ext::Cfg().hudless.HdrPasteOn(),
                                       [&] { return EnsureHdrStageOnce(index); },
                                       [&] { return ProducePaste(sc, slot, finalImage, fg); });
    }
    else if (action == PasteAction::Invalidate)
        UiPaste::Invalidate(ReasonText(_core.PresentReason()));

    if (!produced)
    {
        // The ring is empty from here on. On a base that is not swapped nothing was supposed to be pasted from it, so the
        // gate closes and no output counts as a skipped paste. On a swapped base the paste IS missing: leave the gate
        // alone so every output that shows no UI keeps counting as skipped.
        if (!_core.Swapped())
            UiPaste::SetPasteEnabled(false);
        UiPaste::Invalidate("the paste producer could not run");
        _core.Fail(Reason::PasteNotReady);
    }

    Finish();
}

void Engine::Release(Dx11wDx12SC& sc)
{
    if (State::Instance().isShuttingDown)
        return; // never release or wait from DLL detach under the loader lock; the process is going away

    if (!_everAllocated && _bridge == nullptr && _retired.empty())
        return; // nothing was ever created
    if (_bridge != nullptr && _bridge != &sc)
        return; // another bridge's resources are not ours

    RetireAll();
    _bridge = nullptr;
    _device12 = nullptr;
    _sc = nullptr;
}

void Engine::Overlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    const Mode mode = CurrentMode();
    if (mode != Mode::UiFree)
    {
        // A game without the D3D11 bridge never reaches Pre, the only other place that quietens the inline guard when the
        // mode is off. With a bridge, Pre owns the bit (it still has slots to retire).
        if (mode == Mode::Off && !_bridgeSeen.load(std::memory_order_relaxed))
        {
            ext::SetActive(ext::F::Hudless, false);
            if (CurrentMode() != Mode::Off) // the menu switched the mode on in the meantime
                ext::SetActive(ext::F::Hudless, true);
        }
        return;
    }

    // Only DLSS-G is known to route every generated frame through this present; "Show Detected UI" tints what DLSS-G
    // receives, pasting untinted UI would hide that view (docs/HUDLESS.md 5, DebugTintOn).
    auto& state = State::Instance();
    if (state.isShuttingDown || state.activeFgOutput != FGOutput::DLSSG || state.fgHudlessCompare)
        return;

    UiPaste::Paste(swapChain, queue);
}

Recomposition Engine::QueryRecomposition()
{
    Recomposition result;
    const auto& configured = ext::Cfg().fg.uiRecomposition;
    result.enabled = CurrentMode() == Mode::Recomposition && ext::game::Current().hudless != nullptr &&
                     (configured.has_value() ? configured.value() : true);

    if (const uint32_t last = _lastFormat.load(std::memory_order_relaxed); last != 0)
    {
        result.format = last;
        result.formatSource = "last capture";
    }
    else if (const uint32_t bridge = _bridgeFormat.load(std::memory_order_relaxed); bridge != 0)
    {
        result.format = bridge;
        result.formatSource = "bridge buffer";
    }
    return result;
}
} // namespace

// ---- public face ------------------------------------------------------------------------------------------------------------
Status GetStatus() { return Engine::Get().GetStatus(); }
Recomposition QueryRecomposition() { return Engine::Get().QueryRecomposition(); }

std::string StatusLine(const Status& status)
{
    if (status.mode == Mode::Off)
        return "Off";
    std::string line = StateText(status.phase, status.reason);
    if (status.phase == Phase::Fallback)
        line += std::format(": {}", ReasonText(status.reason));
    return line;
}

namespace detail
{
void PreInteropCopy(Dx11wDx12SC& sc) { Engine::Get().Pre(sc); }

void UiFreeFrame(Dx11wDx12SC& sc, unsigned int, ID3D12Resource*& copySource, D3D12_RESOURCE_STATES& sourceState)
{
    Engine::Get().SwapBase(sc, copySource, sourceState);
}

void PostInteropCopy(Dx11wDx12SC& sc) { Engine::Get().Post(sc); }
void OnReleaseInterop(Dx11wDx12SC& sc) { Engine::Get().Release(sc); }
void BeforeOverlay(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue) { Engine::Get().Overlay(swapChain, queue); }
} // namespace detail
} // namespace ext::hudless
