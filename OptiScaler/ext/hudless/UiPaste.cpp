#include "pch.h"

#include "ext/gpu/Gpu.h"
#include "ext/hudless/HudlessGpu.h"
#include "ext/hudless/UiGpuCommon.h"
#include "ext/hudless/UiPasteRing.h"

#include <UiPasteProduce_Shader.h>
#include <UiPasteVs_Shader.h>
#include <UiPastePs_Shader.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// Post frame-generation UI paste (docs/HUDLESS.md sections 6 and 7; GPU half of the UI-free mode).
//
//  Producer  UiPaste::Produce, real-frame thread, FG queue: one compute pass per real frame builds the "paste image"
//            from (final, UI-free) into a slot of a small ring and counts the flagged pixels for the coverage guard.
//  Consumer  UiPaste::Paste, every output present (real and generated): draws the newest COMPLETED image over the
//            backbuffer on the presenting queue.
//
// Synchronisation. Two fences, each signalled by exactly one queue: the produce fence (FG queue) and the paste fence
// (presenting queue). The producer only reuses a slot whose write and every paste that read it have completed
// (CPU-side GetCompletedValue, never a wait); the consumer only picks images whose write completed. All slot rules are
// in UiPasteRing.h (pure model, tested on the CPU).
//
// Locking. `prodMutex` serialises the producer-side calls (Produce, Invalidate, Release); `pasteMutex` serialises the
// consumer's command recording; `mutex` guards the shared bookkeeping and is held only for short sections, never while
// recording or submitting. Order: prodMutex -> mutex, pasteMutex -> mutex.
//
// Lifetime. Images (texture + coverage counter + readback + producer descriptors + allocator) are retired together and
// destroyed once the produce fence passed their write and the paste fence passed their last read; Release() only moves
// them to the retire list. The singleton is heap allocated and never destroyed (loader lock).
namespace ext::hudless
{
namespace
{
using namespace detail;

constexpr uint32_t kPasteRing = 32; // consumer command storage: covers 4 base frames at 8x generation
constexpr UINT kProduceDescriptors = 6; // t0 final, t1 UI-free, t2 previous paste image, t3 HDR final, u0 image, u1 counter
constexpr D3D12_RESOURCE_STATES kRest = static_cast<D3D12_RESOURCE_STATES>(
    static_cast<int>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) | static_cast<int>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct ProduceConstants
{
    float threshold;
    uint32_t radius;
    uint32_t width;
    uint32_t height;
    uint32_t hasPrevious;
    uint32_t cleanup;
    uint32_t useHdr;
    uint32_t pad;
};

// Everything a ring slot owns on the GPU; retired as one unit.
struct Image
{
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> counter;  // 4 bytes, UAV, rests in COPY_SOURCE
    ComPtr<ID3D12Resource> readback; // 4 bytes, READBACK heap
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 width = 0;
    UINT height = 0;
    bool coveragePending = false; // the counter of the newest write was not read back yet
    uint64_t ordinal = 0;         // PasteStatus::produced right after this image was built (PasteStatus::coverageSequence)
};

struct Retired
{
    std::unique_ptr<Image> image;
    ring::Slot meta; // fence values and reader count at the time of retirement
};

struct PasteCompute
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
};

struct Consumer
{
    struct Pso
    {
        DXGI_FORMAT format;
        ComPtr<ID3D12PipelineState> pso; // null: creation failed for this format, do not retry every present
    };
    ComPtr<ID3D12RootSignature> root;
    std::vector<Pso> psos;
    std::array<ComPtr<ID3D12CommandAllocator>, kPasteRing> allocators;
    std::array<uint64_t, kPasteRing> allocatorValues {};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> csuHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvIncrement = 0;
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    uint64_t counter = 0;
    ID3D12CommandQueue* queue = nullptr; // identity of the queue that signals `fence`
    ID3D12Device* device = nullptr;      // identity of the device these objects belong to
    bool ready = false;
    bool failed = false;
};

struct Core
{
    std::mutex prodMutex;
    std::mutex pasteMutex;
    std::mutex mutex; // guards everything below except `consumer` (pasteMutex) and the atomics

    // shared bookkeeping
    ring::Ring ring;
    std::array<std::unique_ptr<Image>, ring::kSlots> images;
    std::vector<Retired> retired;
    PasteStatus status;
    uint64_t coverageFrame = 0; // frame of the write the published coverage belongs to
    ID3D12Device* device = nullptr; // identity only
    bool failed = false;            // shader / pipeline creation failed on this device
    UINT64 width = 0;
    UINT height = 0;
    DXGI_FORMAT wantedFormat = DXGI_FORMAT_UNKNOWN; // what the final image asked for
    DXGI_FORMAT imageFormat = DXGI_FORMAT_UNKNOWN;  // what the images use (typed UAV store available)
    bool loggedFirstImage = false;
    bool loggedFirstPaste = false;

    // producer objects
    PasteCompute produce;
    ComPtr<ID3D12Fence> produceFence;
    ComPtr<ID3D12Fence> pasteFence; // published by the consumer (EnsureConsumer), read by both sides under `mutex`
    uint64_t produceValue = 0;
    ComPtr<ID3D12Resource> zero; // 4 zero bytes, copied over a slot's counter before each pass
    ComPtr<ID3D12CommandAllocator> bootstrapAllocator;
    ComPtr<ID3D12GraphicsCommandList> produceList;
    ID3D12CommandQueue* produceQueue = nullptr; // identity of the queue that signals `produceFence`
    UINT csuIncrement = 0;

    Consumer consumer;

    std::atomic<bool> debugTint { false };
    std::atomic<bool> steady { false };

    // Objects of a device that went away are parked here and never released (work on that device may still reference
    // them; it is gone or about to be).
    std::vector<ComPtr<IUnknown>> graveyard;
    std::vector<std::unique_ptr<Image>> parkedImages;
    std::vector<std::unique_ptr<Consumer>> parkedConsumers;
};

Core& C()
{
    static Core* core = new Core;
    return *core;
}

void SetMessageLocked(Core& c, const char* message)
{
    if (message == nullptr)
        message = "";
    if (std::strncmp(c.status.lastMessage, message, sizeof(c.status.lastMessage)) == 0)
        return;
    strncpy_s(c.status.lastMessage, sizeof(c.status.lastMessage), message, _TRUNCATE);
    LOG_DEBUG("UI paste: {}", c.status.lastMessage);
}

// Completed fence values; UINT64_MAX (device removed) reads as "nothing completed" and sets `removed`.
struct Done
{
    ring::Fences fences;
    bool removed = false;
};

Done ReadFences(Core& c)
{
    Done done;
    const uint64_t produced = c.produceFence ? c.produceFence->GetCompletedValue() : 0;
    const uint64_t pasted = c.pasteFence ? c.pasteFence->GetCompletedValue() : 0;
    if (produced == UINT64_MAX || pasted == UINT64_MAX)
    {
        done.removed = true;
        return done;
    }
    done.fences = { produced, pasted };
    return done;
}

// Moves the retired images whose write and last read completed (or whose device was removed) into reed. The caller
// lets reed die AFTER it released c.mutex: releasing a committed 4K texture can take milliseconds in the driver and
// the consumer's pick needs that lock.
void CollectRetiredLocked(Core& c, const Done& done, std::vector<Retired>& freed)
{
    std::erase_if(c.retired,
                  [&](Retired& item)
                  {
                      if (item.meta.readers != 0)
                          return false;
                      if (!done.removed && (item.meta.producedValue > done.fences.produced ||
                                            (item.meta.allocated && item.meta.lastReadValue > done.fences.pasted)))
                      {
                          return false;
                      }
                      freed.push_back(std::move(item));
                      return true;
                  });
}

void RetireAllLocked(Core& c)
{
    for (int i = 0; i < ring::kSlots; ++i)
    {
        if (c.images[i])
            c.retired.push_back({ std::move(c.images[i]), c.ring.slots[i] });
    }
    c.ring.Reset();
    c.status.coverage = -1.f;
    c.status.coverageSequence = 0;
    c.coverageFrame = 0;
}

// Reads back the flagged-pixel counters whose pass completed. Never stalls: only fence-completed writes are mapped.
void HarvestCoverageLocked(Core& c, const Done& done)
{
    if (done.removed)
        return;
    for (int i = 0; i < ring::kSlots; ++i)
    {
        Image* image = c.images[i].get();
        const ring::Slot& slot = c.ring.slots[i];
        if (image == nullptr || !image->coveragePending || slot.producedValue > done.fences.produced)
            continue;

        image->coveragePending = false;
        uint32_t flagged = 0;
        void* mapped = nullptr;
        const D3D12_RANGE read { 0, sizeof(uint32_t) };
        if (FAILED(image->readback->Map(0, &read, &mapped)) || mapped == nullptr)
            continue;
        std::memcpy(&flagged, mapped, sizeof(flagged));
        const D3D12_RANGE none { 0, 0 };
        image->readback->Unmap(0, &none);

        if (slot.frame >= c.coverageFrame && image->width != 0 && image->height != 0)
        {
            const double pixels = static_cast<double>(image->width) * image->height;
            c.status.coverage = static_cast<float>(std::min(1.0, flagged / pixels));
            c.status.coverageSequence = image->ordinal;
            c.coverageFrame = slot.frame;
        }
    }
}

DXGI_FORMAT ImageFormatFor(DXGI_FORMAT finalFormat)
{
    switch (TypedFormat(finalFormat))
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM; // 2-bit alpha holds 0 / ~0.33-0.67 / 1: the three levels survive
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// Moves a COM pointer into the graveyard without releasing it.
template <typename T> void Park(Core& c, ComPtr<T>& pointer)
{
    if (pointer)
        c.graveyard.emplace_back(std::move(pointer));
}

void ParkLocked(Core& c)
{
    // Called on a device change: nothing of the old device is released (the Core is never destroyed).
    for (auto& image : c.images)
        if (image)
            c.parkedImages.push_back(std::move(image));
    for (auto& item : c.retired)
        if (item.image)
            c.parkedImages.push_back(std::move(item.image));
    c.retired.clear();
    c.ring = {};
    c.produce = {};
    Park(c, c.produceFence);
    Park(c, c.pasteFence);
    Park(c, c.zero);
    Park(c, c.bootstrapAllocator);
    Park(c, c.produceList);
    c.produceValue = 0;
    c.produceQueue = nullptr;
    c.width = 0;
    c.height = 0;
    c.wantedFormat = c.imageFormat = DXGI_FORMAT_UNKNOWN;
    c.status.coverage = -1.f;
    c.status.coverageSequence = 0;
    c.coverageFrame = 0;
}

// Needs c.mutex and prodMutex held. Creates the producer pipeline once per device.
bool EnsureProducerLocked(Core& c, ID3D12Device* device)
{
    if (c.device != device)
    {
        if (c.device != nullptr)
            LOG_WARN("UI paste: D3D12 device changed, recreating");
        ParkLocked(c);
        c.device = device;
        c.failed = false;
    }
    if (c.failed)
        return false;
    if (c.produce.pso)
        return true;

    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, sizeof(ProduceConstants) / 4 };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 2, ranges };
    D3D12_ROOT_SIGNATURE_DESC rootDesc { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };

    PasteCompute made;
    made.root = CreateRootSignature(device, rootDesc, "UI paste producer");
    if (!made.root)
    {
        c.failed = true;
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
    psoDesc.pRootSignature = made.root.Get();
    psoDesc.CS = { UiPasteProduce_cso, sizeof(UiPasteProduce_cso) };
    HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&made.pso));
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: CreateComputePipelineState failed: {:X}", (UINT) hr);
        c.failed = true;
        return false;
    }

    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr))
    {
        LOG_ERROR("UI paste: producer CreateFence failed: {:X}", (UINT) hr);
        c.failed = true;
        return false;
    }

    ComPtr<ID3D12Resource> zero;
    {
        const auto heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = BufferDesc(sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE);
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&zero));
        void* mapped = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(zero->Map(0, nullptr, &mapped)) && mapped != nullptr)
        {
            std::memset(mapped, 0, sizeof(uint32_t));
            zero->Unmap(0, nullptr);
        }
        else
        {
            LOG_ERROR("UI paste: creating the counter reset buffer failed: {:X}", (UINT) hr);
            c.failed = true;
            return false;
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        LOG_ERROR("UI paste: creating the producer command list failed");
        c.failed = true;
        return false;
    }
    list->Close();

    c.produce = std::move(made);
    c.produceFence = std::move(fence);
    c.zero = std::move(zero);
    c.bootstrapAllocator = std::move(allocator);
    c.produceList = std::move(list);
    c.produceValue = 0;
    c.produceQueue = nullptr;
    c.csuIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    LOG_INFO("UI paste: producer ready");
    return true;
}

// Creates the objects of one ring slot. No lock needed: only the producer touches a slot that is not ready.
std::unique_ptr<Image> CreateImage(ID3D12Device* device, UINT64 width, UINT height, DXGI_FORMAT format, [[maybe_unused]] int index)
{
    auto image = std::make_unique<Image>();
    image->width = width;
    image->height = height;

    {
        const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = Texture2DDesc(width, height, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRest, nullptr,
                                                           IID_PPV_ARGS(&image->texture));
        if (FAILED(hr) || !image->texture)
        {
            LOG_ERROR("UI paste: creating paste image {} ({}x{}, format {}) failed: {:X}", index, width, height,
                      (UINT) format, (UINT) hr);
            return nullptr;
        }
        image->texture->SetName(L"HudLess paste image");
    }
    {
        const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = BufferDesc(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                   nullptr, IID_PPV_ARGS(&image->counter))))
        {
            LOG_ERROR("UI paste: creating the coverage counter failed");
            return nullptr;
        }
    }
    {
        const auto heap = HeapProps(D3D12_HEAP_TYPE_READBACK);
        const auto desc = BufferDesc(sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&image->readback))))
        {
            LOG_ERROR("UI paste: creating the coverage readback failed");
            return nullptr;
        }
    }
    image->heap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kProduceDescriptors, true);
    if (!image->heap ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&image->allocator))))
    {
        LOG_ERROR("UI paste: creating the producer descriptors / allocator failed");
        return nullptr;
    }
    return image;
}

// ---------------------------------------------------------------------------------------------------------------------
// Consumer objects (pasteMutex held)

bool EnsureConsumer(Core& c, ID3D12Device* device)
{
    Consumer& k = c.consumer;
    if (k.device != device && (k.ready || k.failed))
    {
        // A different D3D12 device: the old objects are parked, never released.
        std::lock_guard lock(c.mutex);
        auto parked = std::make_unique<Consumer>(std::move(k));
        c.parkedConsumers.push_back(std::move(parked));
        k = Consumer {};
    }
    if (k.failed)
        return false;
    if (k.ready)
        return true;

    D3D12_DESCRIPTOR_RANGE range { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, 1 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &range };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };

    Consumer made;
    made.root = CreateRootSignature(device, rootDesc, "UI paste draw");
    bool ok = made.root != nullptr && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&made.fence)));
    for (uint32_t i = 0; ok && i < kPasteRing; ++i)
        ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&made.allocators[i])));
    if (ok)
    {
        ok = SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, made.allocators[0].Get(), nullptr,
                                                 IID_PPV_ARGS(&made.list)));
        if (ok)
            made.list->Close();
    }
    if (ok)
    {
        made.csuHeap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kPasteRing, true);
        made.rtvHeap = CreateHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kPasteRing, false);
        ok = made.csuHeap && made.rtvHeap;
    }
    if (!ok)
    {
        LOG_ERROR("UI paste: creating the paste draw objects failed");
        k.failed = true;
        k.device = device;
        return false;
    }
    made.rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    made.device = device;
    made.ready = true;
    k = std::move(made);
    {
        std::lock_guard lock(c.mutex);
        c.pasteFence = k.fence; // the producer's retire logic reads it
    }
    return true;
}

ID3D12PipelineState* EnsurePastePso(Core& c, ID3D12Device* device, DXGI_FORMAT rtvFormat)
{
    Consumer& k = c.consumer;
    for (auto& entry : k.psos)
        if (entry.format == rtvFormat)
            return entry.pso.Get();

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = k.root.Get();
    desc.VS = { UiPasteVs_cso, sizeof(UiPasteVs_cso) };
    desc.PS = { UiPastePs_cso, sizeof(UiPastePs_cso) };
    // No blending: pasted pixels overwrite. Alpha of the backbuffer is left alone.
    desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtvFormat;
    desc.SampleDesc = { 1, 0 };

    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr))
        LOG_ERROR("UI paste: CreateGraphicsPipelineState for format {} failed: {:X}", (UINT) rtvFormat, (UINT) hr);
    k.psos.push_back({ rtvFormat, pso });
    return pso.Get();
}

// A slot picked by Paste was submitted (value) or abandoned (abort). The slot may have been retired meanwhile.
void FinishReadLocked(Core& c, Image* image, int slot, uint32_t epoch, bool submitted, uint64_t value)
{
    if (epoch == c.ring.epoch)
    {
        if (submitted)
            c.ring.EndRead(slot, value);
        else
            c.ring.AbortRead(slot);
        return;
    }
    for (auto& item : c.retired)
    {
        if (item.image.get() != image)
            continue;
        if (item.meta.readers != 0)
            --item.meta.readers;
        if (submitted && item.meta.lastReadValue != ring::kNever &&
            (value == ring::kNever || value > item.meta.lastReadValue))
        {
            item.meta.lastReadValue = value;
        }
        return;
    }
}
} // namespace

// =====================================================================================================================

namespace UiPaste
{
void Produce(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* finalImage, DXGI_FORMAT finalFormat,
             ID3D12Resource* hudlessImage, DXGI_FORMAT hudlessFormat, ID3D12Resource* hdrFinalImage,
             const MaskParams& params)
{
    try
    {
        if (device == nullptr || queue == nullptr || finalImage == nullptr || hudlessImage == nullptr)
            return;

        Core& c = C();
        std::lock_guard producer(c.prodMutex);

        // ---- phase 1: decisions, under the shared lock (no GPU work, no waiting) -----------------------------------
        int previous = -1;
        int target = -1;
        Done done;
        UINT64 width = 0;
        UINT height = 0;
        DXGI_FORMAT imageFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT finalView = DXGI_FORMAT_UNKNOWN, hudlessView = DXGI_FORMAT_UNKNOWN, hdrView = DXGI_FORMAT_UNKNOWN;
        std::vector<Retired> freed; // destroyed after the lock below is released (also on the early returns)
        {
            std::lock_guard lock(c.mutex);
            previous = c.ring.Tick();

            if (!EnsureProducerLocked(c, device))
            {
                SetMessageLocked(c, "Paste shaders failed to initialise (see OptiScaler.log)");
                return;
            }

            // One fence per queue: values signalled from two queues could complete out of order.
            if (queue != c.produceQueue)
            {
                if (c.produceQueue != nullptr && c.produceValue > c.produceFence->GetCompletedValue())
                {
                    SetMessageLocked(c, "The frame-generation queue changed while paste work is in flight");
                    return;
                }
                c.produceQueue = queue;
                previous = -1; // the cleanup input was written by the other queue
            }

            done = ReadFences(c);
            if (done.removed)
            {
                SetMessageLocked(c, "D3D12 device removed");
                return;
            }
            CollectRetiredLocked(c, done, freed);
            HarvestCoverageLocked(c, done);

            const auto finalDesc = finalImage->GetDesc();
            const auto hudlessDesc = hudlessImage->GetDesc();
            finalView = ViewFormat(finalFormat, finalDesc);
            hudlessView = ViewFormat(hudlessFormat, hudlessDesc);
            if (!IsSampleableTexture(finalDesc) || !IsSampleableTexture(hudlessDesc) || !IsColorFormat(finalView) ||
                !IsColorFormat(hudlessView))
            {
                SetMessageLocked(c, "Backbuffer copy is not readable as a colour texture");
                return;
            }
            if (finalDesc.Width != hudlessDesc.Width || finalDesc.Height != hudlessDesc.Height || finalDesc.Width == 0 ||
                finalDesc.Height == 0 || finalImage == hudlessImage)
            {
                SetMessageLocked(c, "Backbuffer and HUD-less sizes differ");
                return;
            }
            if (params.hdr)
            {
                if (hdrFinalImage == nullptr)
                {
                    SetMessageLocked(c, "HDR final image missing");
                    return;
                }
                const auto hdrDesc = hdrFinalImage->GetDesc();
                hdrView = ViewFormat(DXGI_FORMAT_UNKNOWN, hdrDesc);
                if (!IsSampleableTexture(hdrDesc) || hdrView != DXGI_FORMAT_R10G10B10A2_UNORM ||
                    hdrDesc.Width != finalDesc.Width || hdrDesc.Height != finalDesc.Height)
                {
                    SetMessageLocked(c, "HDR final image is not a PQ image of the frame size");
                    return;
                }
            }

            width = finalDesc.Width;
            height = finalDesc.Height;
            const DXGI_FORMAT wanted = params.hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : ImageFormatFor(finalFormat);
            if (width != c.width || height != c.height || wanted != c.wantedFormat)
            {
                RetireAllLocked(c);
                previous = -1;
                c.width = width;
                c.height = height;
                c.wantedFormat = wanted;
                // Typed UAV stores of R10G10B10A2 are optional; FP16 is mandatory and holds the same three levels.
                c.imageFormat = (wanted == DXGI_FORMAT_R8G8B8A8_UNORM || SupportsTypedUavStore(device, wanted))
                                    ? wanted
                                    : DXGI_FORMAT_R16G16B16A16_FLOAT;
            }
            imageFormat = c.imageFormat;

            // Reuse only a slot whose previous write and every paste of it have completed: never a GPU wait.
            target = c.ring.PickTarget(previous, done.fences);
            if (target < 0)
            {
                SetMessageLocked(c, "No free UI image this frame (presents are running behind)");
                return;
            }
            c.ring.BeginWrite(target);
        }

        freed.clear();

        // ---- phase 2: create, record, submit (producer-private objects only; the consumer cannot see `target`) ------
        if (!c.images[target])
        {
            auto fresh = CreateImage(device, width, height, imageFormat, target);
            std::lock_guard lock(c.mutex);
            if (!fresh)
            {
                SetMessageLocked(c, "Could not create the UI paste image");
                return;
            }
            c.images[target] = std::move(fresh); // published under the lock: other threads read the table
        }
        Image& image = *c.images[target];

        auto fail = [&](const char* message)
        {
            std::lock_guard lock(c.mutex);
            SetMessageLocked(c, message);
        };

        // The slot's previous producer work has completed (PickTarget), so its allocator is idle.
        if (FAILED(image.allocator->Reset()))
        {
            fail("Command allocator reset failed");
            return;
        }
        ID3D12GraphicsCommandList* list = c.produceList.Get();
        HRESULT hr = ext::gpu::ResetCommands(list, image.allocator.Get());
        if (FAILED(hr))
        {
            LOG_ERROR("UI paste: producer command list reset failed: {:X}", (UINT) hr);
            fail("Command list reset failed");
            return;
        }

        const UINT inc = c.csuIncrement;
        ID3D12DescriptorHeap* heap = image.heap.Get();
        WriteTextureSrv(device, finalImage, finalView, CpuHandle(heap, 0, inc));
        WriteTextureSrv(device, hudlessImage, hudlessView, CpuHandle(heap, 1, inc));
        WriteTextureSrv(device, previous >= 0 ? c.images[previous]->texture.Get() : nullptr, imageFormat,
                        CpuHandle(heap, 2, inc));
        WriteTextureSrv(device, params.hdr ? hdrFinalImage : nullptr,
                        params.hdr ? hdrView : DXGI_FORMAT_R8G8B8A8_UNORM, CpuHandle(heap, 3, inc));
        WriteTextureUav(device, image.texture.Get(), imageFormat, CpuHandle(heap, 4, inc));
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = 1;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device->CreateUnorderedAccessView(image.counter.Get(), nullptr, &uav, CpuHandle(heap, 5, inc));
        }

        ProduceConstants constants {};
        constants.threshold = ClampThreshold(params.threshold);
        constants.radius = ClampDilation(params.dilation);
        constants.width = static_cast<uint32_t>(width);
        constants.height = height;
        constants.hasPrevious = previous >= 0 ? 1 : 0;
        constants.cleanup = params.cleanup ? 1 : 0;
        constants.useHdr = params.hdr ? 1 : 0;

        // final and UI-free rest in COMMON between uses (interop copy, provider tagging) and are returned to it.
        // hdrFinalImage is only read, in the state it arrives in (COMMON or NON_PIXEL_SHADER_RESOURCE).
        Transition(list, finalImage, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list, hudlessImage, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list, image.texture.Get(), kRest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // zero the counter
        Transition(list, image.counter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(image.counter.Get(), 0, c.zero.Get(), 0, sizeof(uint32_t));
        Transition(list, image.counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* heaps[] = { heap };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(c.produce.root.Get());
        list->SetPipelineState(c.produce.pso.Get());
        list->SetComputeRoot32BitConstants(0, sizeof(ProduceConstants) / 4, &constants, 0);
        list->SetComputeRootDescriptorTable(1, GpuHandle(heap, 0, inc));
        list->Dispatch(static_cast<UINT>((width + 15) / 16), (height + 15) / 16, 1);

        Transition(list, image.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRest);
        Transition(list, image.counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(image.readback.Get(), 0, image.counter.Get(), 0, sizeof(uint32_t));
        Transition(list, hudlessImage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Transition(list, finalImage, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

        hr = list->Close();
        if (FAILED(hr))
        {
            LOG_ERROR("UI paste: producer command list close failed: {:X}", (UINT) hr);
            fail("Command list close failed");
            return;
        }

        ID3D12CommandList* lists[] = { list };
        ext::gpu::ExecuteCommands(queue, 1, lists);

        const uint64_t value = c.produceValue + 1;
        hr = queue->Signal(c.produceFence.Get(), value);
        c.produceValue = value;

        // ---- phase 3: publish ------------------------------------------------------------------------------------
        std::lock_guard lock(c.mutex);
        if (FAILED(hr))
        {
            // The write is submitted but has no completion token: never reuse or paste this slot again.
            LOG_ERROR("UI paste: producer Signal failed: {:X}", (UINT) hr);
            c.ring.PoisonWrite(target);
            SetMessageLocked(c, "Fence signal failed");
            return;
        }
        c.ring.EndWrite(target, value, NowMs());
        image.coveragePending = true;
        image.ordinal = c.status.produced + 1;
        if (c.status.produced++ == 0 && !c.loggedFirstImage)
        {
            c.loggedFirstImage = true;
            LOG_INFO("UI paste: first UI image built ({}x{})", width, height);
        }
        SetMessageLocked(c, "Active");
    }
    catch (...)
    {
    }
}

void Invalidate(const char* reason)
{
    try
    {
        Core& c = C();
        std::lock_guard producer(c.prodMutex);
        std::lock_guard lock(c.mutex);
        c.ring.Tick();
        c.ring.InvalidateReady();
        SetMessageLocked(c, reason);
    }
    catch (...)
    {
    }
}

bool Ready(ID3D12Device* device, uint32_t width, uint32_t height)
{
    try
    {
        Core& c = C();
        std::lock_guard lock(c.mutex);
        if (c.failed || c.device == nullptr || c.device != device || c.width != width || c.height != height)
            return false;
        const Done done = ReadFences(c);
        return !done.removed && c.ring.AnyPasteable(done.fences, NowMs());
    }
    catch (...)
    {
        return false;
    }
}

void Release()
{
    try
    {
        Core& c = C();
        std::vector<Retired> freed; // declared first: destroyed after both locks are released
        std::lock_guard producer(c.prodMutex);
        std::lock_guard lock(c.mutex);
        RetireAllLocked(c);
        CollectRetiredLocked(c, ReadFences(c), freed);
    }
    catch (...)
    {
    }
}

void Paste(IDXGISwapChain* swapChain, ID3D12CommandQueue* presentQueue)
{
    try
    {
        if (swapChain == nullptr || presentQueue == nullptr)
            return;

        Core& c = C();
        std::lock_guard consumerLock(c.pasteMutex);

        // ---- pick -------------------------------------------------------------------------------------------------
        int slot = -1;
        uint32_t epoch = 0;
        Image* image = nullptr;
        uint64_t producedValue = 0;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence> produceFence;
        {
            std::lock_guard lock(c.mutex);
            if (c.device == nullptr || !c.produceFence || c.status.produced == 0 || c.failed)
                return; // nothing was ever produced: not a skip of a frame-generation present
            const Done done = ReadFences(c);
            if (done.removed)
                return;
            slot = c.ring.PickPaste(done.fences, NowMs(), c.steady.load(std::memory_order_relaxed));
            if (slot < 0)
            {
                ++c.status.skipped;
                SetMessageLocked(c, "No completed UI image to paste");
                return;
            }
            c.ring.BeginRead(slot);
            epoch = c.ring.epoch;
            image = c.images[slot].get();
            producedValue = c.ring.slots[slot].producedValue;
            device = c.device;
            produceFence = c.produceFence;
        }

        auto abandon = [&](const char* message)
        {
            std::lock_guard lock(c.mutex);
            FinishReadLocked(c, image, slot, epoch, false, 0);
            ++c.status.skipped;
            SetMessageLocked(c, message);
        };

        // ---- validate the target ----------------------------------------------------------------------------------
        Consumer& k = c.consumer;
        if (!EnsureConsumer(c, device.Get()))
        {
            abandon("Paste draw objects failed to initialise (see OptiScaler.log)");
            return;
        }

        if (presentQueue != k.queue)
        {
            // The paste fence is signalled by one queue only: rebind after the old queue's pastes finished.
            if (k.queue != nullptr && k.value > k.fence->GetCompletedValue())
            {
                abandon("The presenting queue changed while pastes are in flight");
                return;
            }
            ComPtr<ID3D12Device> queueDevice;
            if (FAILED(presentQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != device.Get())
            {
                abandon("The presenting queue is on another D3D12 device");
                return;
            }
            k.queue = presentQueue;
        }

        ComPtr<IDXGISwapChain3> swapChain3;
        if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
        {
            abandon("The swapchain has no current backbuffer index");
            return;
        }
        ComPtr<ID3D12Resource> backBuffer;
        const HRESULT bbHr = swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
        swapChain3.Reset();
        if (FAILED(bbHr) || !backBuffer)
        {
            abandon("Could not get the presenting backbuffer");
            return;
        }
        // Do not keep the backbuffer past this call (it would make ResizeBuffers fail).
        struct BackBufferRelease
        {
            ComPtr<ID3D12Resource>& ref;
            ~BackBufferRelease() { ref.Reset(); }
        } backBufferRelease { backBuffer };

        const UINT64 imageWidth = image->width; // image may be retired and freed once this paste is accounted for
        const UINT imageHeight = image->height;
        const auto backDesc = backBuffer->GetDesc();
        if (backDesc.Width != imageWidth || backDesc.Height != imageHeight || backDesc.SampleDesc.Count != 1)
        {
            abandon("Backbuffer size differs from the UI image (resizing?)");
            return;
        }
        if ((backDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
        {
            abandon("The backbuffer cannot be a render target");
            return;
        }

        const DXGI_FORMAT rtvFormat = TypedFormat(backDesc.Format);
        ID3D12PipelineState* pso = EnsurePastePso(c, device.Get(), rtvFormat);
        if (pso == nullptr)
        {
            abandon("Paste pipeline failed to initialise (see OptiScaler.log)");
            return;
        }

        const uint32_t ringIndex = static_cast<uint32_t>(k.counter % kPasteRing);
        if (k.allocatorValues[ringIndex] > k.fence->GetCompletedValue())
        {
            abandon("Presenting queue is running behind, skipped a paste");
            return;
        }

        // ---- record -----------------------------------------------------------------------------------------------
        ID3D12CommandAllocator* allocator = k.allocators[ringIndex].Get();
        ID3D12GraphicsCommandList* list = k.list.Get();
        if (FAILED(allocator->Reset()) || FAILED(ext::gpu::ResetCommands(list, allocator)))
        {
            abandon("Paste command list reset failed");
            return;
        }

        const UINT csuInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        WriteTextureSrv(device.Get(), image->texture.Get(), image->texture->GetDesc().Format,
                        CpuHandle(k.csuHeap.Get(), ringIndex, csuInc));
        auto rtv = k.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(ringIndex) * k.rtvIncrement;
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc {};
        rtvDesc.Format = rtvFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(backBuffer.Get(), &rtvDesc, rtv);

        // A presentable backbuffer is in PRESENT (== COMMON) whenever Present is called.
        Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12DescriptorHeap* heaps[] = { k.csuHeap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(k.root.Get());
        list->SetPipelineState(pso);
        const UINT tint = c.debugTint.load(std::memory_order_relaxed) ? 1u : 0u;
        list->SetGraphicsRoot32BitConstants(0, 1, &tint, 0);
        list->SetGraphicsRootDescriptorTable(1, GpuHandle(k.csuHeap.Get(), ringIndex, csuInc));
        const D3D12_VIEWPORT viewport { 0.0f, 0.0f, static_cast<float>(imageWidth), static_cast<float>(imageHeight), 0.0f, 1.0f };
        const D3D12_RECT scissor { 0, 0, static_cast<LONG>(imageWidth), static_cast<LONG>(imageHeight) };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

        HRESULT hr = list->Close();
        if (FAILED(hr))
        {
            LOG_ERROR("UI paste: paste command list close failed: {:X}", (UINT) hr);
            abandon("Paste command list close failed");
            return;
        }

        // The image completed already (it was picked as completed); the wait only makes the dependency explicit.
        presentQueue->Wait(produceFence.Get(), producedValue);
        ID3D12CommandList* lists[] = { list };
        ext::gpu::ExecuteCommands(presentQueue, 1, lists);

        const uint64_t value = k.value + 1;
        hr = presentQueue->Signal(k.fence.Get(), value);
        k.value = value;
        ++k.counter;

        std::lock_guard lock(c.mutex);
        if (FAILED(hr))
        {
            // Submitted without a completion token: keep this image and allocator out of reuse for good.
            LOG_ERROR("UI paste: paste Signal failed: {:X}", (UINT) hr);
            k.allocatorValues[ringIndex] = ring::kNever;
            FinishReadLocked(c, image, slot, epoch, true, ring::kNever);
            return;
        }
        k.allocatorValues[ringIndex] = value;
        FinishReadLocked(c, image, slot, epoch, true, value);
        ++c.status.pasted;
        if (!c.loggedFirstPaste)
        {
            c.loggedFirstPaste = true;
            LOG_INFO("UI paste: first paste after frame generation ({}x{}, backbuffer format {})", imageWidth,
                     imageHeight, (UINT) rtvFormat);
        }
    }
    catch (...)
    {
    }
}

void SetDebugTint(bool enabled)
{
    C().debugTint.store(enabled, std::memory_order_relaxed);
}

void SetSteadyTiming(bool enabled)
{
    C().steady.store(enabled, std::memory_order_relaxed);
}

PasteStatus Snapshot()
{
    try
    {
        Core& c = C();
        std::lock_guard lock(c.mutex);
        if (c.produceFence)
            HarvestCoverageLocked(c, ReadFences(c));
        return c.status;
    }
    catch (...)
    {
        return {};
    }
}
} // namespace UiPaste
} // namespace ext::hudless
