#include "pch.h"

#include "ext/gpu/Gpu.h"
#include "ext/gpu/GpuLifetime.h"
#include "ext/gpu/Restrack.h"
#include "ext/hudless/HudlessGpu.h"
#include "ext/hudless/UiGpuCommon.h"

#include <UiRecomposition_Shader.h>

#include <array>
#include <mutex>

// Recomposition mode (docs/HUDLESS.md sections 7 and 9.1): the "UI colour + alpha" image the frame-generation provider
// takes as its UI input, recorded on the provider's own UI command list.
//
// Lifetime: the provider submits and resets that list itself, so completion cannot be fenced here. The images and
// descriptor heaps are therefore owned through ext::gpu::GpuLifetime, registered as one ext::gpu::Owner on first use
// (modes other than recomposition cost nothing): every list Build records on is tracked, and a retired image is
// destroyed only after all those lists were reset and their submissions completed. A list that is submitted behind
// the owners' back (no queue hook) is never seen as in flight; ext::restrack::EnsureHooks installs the queue hook for
// that reason.
namespace ext::hudless
{
namespace
{
using namespace detail;

constexpr int kMaxSlots = 8; // BUFFER_COUNT is 4; the provider indexes its UI lists by the same fake index
constexpr DXGI_FORMAT kImageFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr D3D12_RESOURCE_STATES kRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct Constants
{
    float threshold;
    uint32_t radius;
    uint32_t width;
    uint32_t height;
};

struct Slot
{
    ComPtr<ID3D12Resource> image;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT64 width = 0;
    UINT height = 0;
};

struct State
{
    std::mutex mutex; // guards everything below; the lifetime tracker has its own lock
    gpu::GpuLifetime lifetime;
    ID3D12Device* device = nullptr; // identity only
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    UINT csuIncrement = 0;
    bool failed = false; // pipeline creation failed on this device
    bool loggedFirst = false;
    std::array<Slot, kMaxSlots> slots {};
    std::once_flag ownerOnce;
    int ownerHandle = -1;
};

// Never destroyed (loader lock, GPU work that may still reference it).
State& S()
{
    static State* state = new State;
    return *state;
}

void RegisterOwner(State& state)
{
    std::call_once(state.ownerOnce,
                   []
                   {
                       gpu::Owner owner;
                       owner.begin = [](void*, UINT n, ID3D12CommandList* const* l) { return S().lifetime.BeginSubmission(n, l); };
                       owner.quarantine = [](void*, UINT n, ID3D12CommandList* const* l) { S().lifetime.QuarantineSubmission(n, l); };
                       owner.reset = [](void*, ID3D12CommandList* c) { S().lifetime.ResetRecording(c); };
                       S().ownerHandle = gpu::RegisterOwner(owner);
                   });
}

// Everything bound to the old device is handed to the tracker; whatever the GPU may still run stays alive until then.
void RetireSlot(State& state, Slot& slot)
{
    if (slot.image || slot.heap)
    {
        state.lifetime.Retire([image = std::move(slot.image), heap = std::move(slot.heap)]() mutable
                              {
                                  image.Reset();
                                  heap.Reset();
                              });
    }
    slot = {};
}

void RetireAll(State& state)
{
    for (auto& slot : state.slots)
        RetireSlot(state, slot);
    state.lifetime.BeginGeneration(); // lists recorded from now on use the replacement resources only
}

bool EnsurePipeline(State& state, ID3D12Device* device)
{
    if (state.device != device)
    {
        if (state.device != nullptr)
            LOG_WARN("UI image: D3D12 device changed, recreating");
        RetireAll(state);
        // The old device's pipeline objects are dropped through the same path as its images.
        state.lifetime.Retire([root = std::move(state.root), pso = std::move(state.pso)]() mutable
                              {
                                  pso.Reset();
                                  root.Reset();
                              });
        state.device = device;
        state.failed = false;
        state.csuIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        RegisterOwner(state);
        if (state.ownerHandle >= 0)
            restrack::EnsureHooks(device);
    }

    if (state.failed)
        return false;
    if (state.pso)
        return true;

    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, sizeof(Constants) / 4 };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 2, ranges };
    D3D12_ROOT_SIGNATURE_DESC rootDesc { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };

    auto root = CreateRootSignature(device, rootDesc, "UI image");
    if (!root)
    {
        state.failed = true;
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
    psoDesc.pRootSignature = root.Get();
    psoDesc.CS = { UiRecomposition_cso, sizeof(UiRecomposition_cso) };
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    if (FAILED(hr))
    {
        LOG_ERROR("UI image: CreateComputePipelineState failed: {:X}", (UINT) hr);
        state.failed = true;
        return false;
    }

    state.root = std::move(root);
    state.pso = std::move(pso);
    return true;
}

bool EnsureSlot(State& state, ID3D12Device* device, int index, UINT64 width, UINT height)
{
    auto& slot = state.slots[index];
    if (slot.image && (slot.width != width || slot.height != height))
    {
        RetireSlot(state, slot);
        state.lifetime.BeginGeneration();
    }

    if (!slot.heap)
    {
        slot.heap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, true);
        if (!slot.heap)
        {
            LOG_ERROR("UI image: descriptor heap creation failed");
            return false;
        }
    }

    if (!slot.image)
    {
        const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = Texture2DDesc(width, height, kImageFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRest, nullptr,
                                                           IID_PPV_ARGS(&slot.image));
        if (FAILED(hr) || !slot.image)
        {
            LOG_ERROR("UI image: creating the {}x{} image for slot {} failed: {:X}", width, height, index, (UINT) hr);
            slot.image.Reset();
            return false;
        }
        slot.image->SetName(L"HudLess UI image");
        slot.width = width;
        slot.height = height;
    }
    return true;
}
} // namespace

namespace UiImage
{
ID3D12Resource* Build(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, int slotIndex,
                      ID3D12Resource* finalImage, DXGI_FORMAT finalFormat, ID3D12Resource* hudlessImage,
                      DXGI_FORMAT hudlessFormat, const MaskParams& params)
{
    try
    {
        if (device == nullptr || commandList == nullptr || finalImage == nullptr || hudlessImage == nullptr ||
            finalImage == hudlessImage || slotIndex < 0 || slotIndex >= kMaxSlots)
        {
            return nullptr;
        }

        const auto finalDesc = finalImage->GetDesc();
        const auto hudlessDesc = hudlessImage->GetDesc();
        const auto finalView = ViewFormat(finalFormat, finalDesc);
        const auto hudlessView = ViewFormat(hudlessFormat, hudlessDesc);
        if (!IsSampleableTexture(finalDesc) || !IsSampleableTexture(hudlessDesc) || !IsColorFormat(finalView) ||
            !IsColorFormat(hudlessView) || finalDesc.Width != hudlessDesc.Width || finalDesc.Height != hudlessDesc.Height ||
            finalDesc.Width == 0 || finalDesc.Height == 0)
        {
            return nullptr;
        }

        auto& state = S();
        std::lock_guard lock(state.mutex);

        state.lifetime.Collect();
        if (!EnsurePipeline(state, device) || !EnsureSlot(state, device, slotIndex, finalDesc.Width, finalDesc.Height))
            return nullptr;

        auto& slot = state.slots[slotIndex];
        state.lifetime.Record(commandList);

        // Rewritten per Build: the provider resets this slot's allocator only after its previous list completed.
        WriteTextureSrv(device, finalImage, finalView, CpuHandle(slot.heap.Get(), 0, state.csuIncrement));
        WriteTextureSrv(device, hudlessImage, hudlessView, CpuHandle(slot.heap.Get(), 1, state.csuIncrement));
        WriteTextureUav(device, slot.image.Get(), kImageFormat, CpuHandle(slot.heap.Get(), 2, state.csuIncrement));

        const Constants constants { ClampThreshold(params.threshold), ClampDilation(params.dilation),
                                    static_cast<uint32_t>(finalDesc.Width), finalDesc.Height };

        // Both inputs rest in COMMON between uses (interop copy / provider tag copy) and are returned to it.
        Transition(commandList, finalImage, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, hudlessImage, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(commandList, slot.image.Get(), kRest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* heaps[] = { slot.heap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(state.root.Get());
        commandList->SetPipelineState(state.pso.Get());
        commandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
        commandList->SetComputeRootDescriptorTable(1, GpuHandle(slot.heap.Get(), 0, state.csuIncrement));
        commandList->Dispatch(static_cast<UINT>((finalDesc.Width + 15) / 16), (finalDesc.Height + 15) / 16, 1);

        Transition(commandList, slot.image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRest);
        Transition(commandList, hudlessImage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        Transition(commandList, finalImage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);

        if (!state.loggedFirst)
        {
            state.loggedFirst = true;
            LOG_INFO("UI image: first recomposition mask built ({}x{})", finalDesc.Width, finalDesc.Height);
        }
        return slot.image.Get();
    }
    catch (...)
    {
        return nullptr;
    }
}

void Release()
{
    try
    {
        auto& state = S();
        std::lock_guard lock(state.mutex);
        RetireAll(state);
        state.lifetime.Collect();
    }
    catch (...)
    {
    }
}
} // namespace UiImage
} // namespace ext::hudless
