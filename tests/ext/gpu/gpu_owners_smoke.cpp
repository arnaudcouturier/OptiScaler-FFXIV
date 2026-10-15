// Exercises the ext::gpu owner registry, the Reset/Execute wrappers and Scope on WARP (no hooks, no product build).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "ext/gpu/Gpu.h"
using Microsoft::WRL::ComPtr;

// The detour lives in QueueHook.cpp (needs the product); the test decides whether "the hook is installed".
static bool g_hookInstalled = false;
namespace ext::gpu
{
bool QueueHookInstalled() { return g_hookInstalled; }
} // namespace ext::gpu

static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed"); }
static void expect(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }

struct Probe
{
    ext::gpu::GpuLifetime lifetime;
    int begins = 0, quarantines = 0, resets = 0, befores = 0, afters = 0;
    bool throwOnBegin = false;
    int isolateIndex = -1;
    std::vector<UINT> batchSizes;
};

int main()
try
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc {};
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    auto makeList = [&]
    {
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        check(list->Close());
        return list;
    };
    ComPtr<ID3D12Fence> drain;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
    UINT64 serial = 0;
    auto wait = [&]
    {
        check(queue->Signal(drain.Get(), ++serial));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(drain->SetEventOnCompletion(serial, event));
        const auto result = WaitForSingleObject(event, 5000);
        CloseHandle(event);
        expect(result == WAIT_OBJECT_0, "GPU did not drain");
    };

    // 1. No owner: plain calls, nothing active.
    {
        expect(!ext::gpu::SubmissionOwnersActive() && !ext::gpu::OwnersWantReset(), "owners active at start");
        auto list = makeList();
        check(allocator->Reset());
        check(ext::gpu::ResetCommands(list.Get(), allocator.Get()));
        check(list->Close());
        ID3D12CommandList* lists[] { list.Get() };
        ext::gpu::ExecuteCommands(queue.Get(), 1, lists);
        wait();
    }

    // 2. One owner backed by a GpuLifetime: Reset closes the recording, Execute pins it until the GPU is done.
    Probe probe;
    ext::gpu::Owner owner;
    owner.ctx = &probe;
    owner.begin = [](void* ctx, UINT count, ID3D12CommandList* const* lists)
    {
        auto& p = *static_cast<Probe*>(ctx);
        ++p.begins;
        if (p.throwOnBegin)
            throw std::runtime_error("begin failed");
        return p.lifetime.BeginSubmission(count, lists);
    };
    owner.quarantine = [](void* ctx, UINT count, ID3D12CommandList* const* lists)
    {
        auto& p = *static_cast<Probe*>(ctx);
        ++p.quarantines;
        p.lifetime.QuarantineSubmission(count, lists);
    };
    owner.reset = [](void* ctx, ID3D12CommandList* commands)
    {
        auto& p = *static_cast<Probe*>(ctx);
        ++p.resets;
        p.lifetime.ResetRecording(commands);
    };
    owner.isolate = [](void* ctx, UINT, ID3D12CommandList* const*) { return static_cast<Probe*>(ctx)->isolateIndex; };
    owner.before = [](void* ctx, ID3D12CommandQueue*, UINT count, ID3D12CommandList* const*)
    {
        auto& p = *static_cast<Probe*>(ctx);
        ++p.befores;
        p.batchSizes.push_back(count);
    };
    owner.after = [](void* ctx, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) { ++static_cast<Probe*>(ctx)->afters; };
    const int handle = ext::gpu::RegisterOwner(owner);
    expect(handle >= 0, "owner not registered");
    expect(ext::gpu::SubmissionOwnersActive() && ext::gpu::OwnersWantReset(), "registered owner not active");

    {
        auto list = makeList();
        int released = 0;
        probe.lifetime.Record(list.Get());
        probe.lifetime.Retire([&] { ++released; });
        ID3D12CommandList* lists[] { list.Get() };
        ext::gpu::ExecuteCommands(queue.Get(), 1, lists);
        expect(probe.begins == 1 && probe.befores == 1 && probe.afters == 1, "owner not called around the execute");
        expect(released == 0, "submitted recording released before it was reset");
        wait(); // the GPU is done with the list
        check(allocator->Reset());
        check(ext::gpu::ResetCommands(list.Get(), allocator.Get()));
        expect(probe.resets == 1, "owner not told about the Reset");
        check(list->Close());
        probe.lifetime.Collect();
        expect(released == 1 && probe.lifetime.Idle(), "recording not released after Reset on a completed submission");
    }

    // 3. Isolation: the list at index 1 is executed alone -> batches of 1, 1, 1.
    {
        auto a = makeList(), b = makeList(), c = makeList();
        ID3D12CommandList* lists[] { a.Get(), b.Get(), c.Get() };
        probe.batchSizes.clear();
        probe.isolateIndex = 1;
        ext::gpu::ExecuteCommands(queue.Get(), 3, lists);
        wait();
        expect(probe.batchSizes == std::vector<UINT>({ 1, 1, 1 }), "isolated list was not split out");
        probe.batchSizes.clear();
        probe.isolateIndex = 2;
        ext::gpu::ExecuteCommands(queue.Get(), 3, lists);
        wait();
        expect(probe.batchSizes == std::vector<UINT>({ 2, 1 }), "trailing isolated list was not split out");
        probe.isolateIndex = -1;
        probe.batchSizes.clear();
        ext::gpu::ExecuteCommands(queue.Get(), 3, lists);
        wait();
        expect(probe.batchSizes == std::vector<UINT>({ 3 }), "batch split without an isolated list");
    }

    // 4. A throwing begin quarantines and the submission still executes.
    {
        auto list = makeList();
        probe.lifetime.Record(list.Get());
        probe.throwOnBegin = true;
        ID3D12CommandList* lists[] { list.Get() };
        const int before = probe.afters;
        ext::gpu::ExecuteCommands(queue.Get(), 1, lists);
        wait();
        probe.throwOnBegin = false;
        expect(probe.quarantines == 1 && probe.afters == before + 1, "throwing begin was not contained");
        int released = 0;
        probe.lifetime.Retire([&] { ++released; });
        probe.lifetime.ResetRecording(list.Get());
        expect(released == 0 && !probe.lifetime.Idle(), "quarantined recording was released");
    }

    // 5. When the detour covers the queue it does the owners' work: the wrapper only executes.
    {
        auto list = makeList();
        ID3D12CommandList* lists[] { list.Get() };
        g_hookInstalled = true;
        const int begins = probe.begins;
        ext::gpu::ExecuteCommands(queue.Get(), 1, lists);
        wait();
        g_hookInstalled = false;
        expect(probe.begins == begins, "wrapper repeated the detour's owner calls");
    }

    // 6. Scope: Submit closes and executes through the owners; an abandoned scope closes the list.
    {
        auto list = makeList();
        check(allocator->Reset());
        const int begins = probe.begins;
        {
            ext::gpu::Scope scope(list.Get(), allocator.Get());
            expect(bool(scope), "scope reset failed");
            check(scope.Submit(queue.Get()));
        }
        wait();
        expect(probe.begins == begins + 1, "scope submit did not go through the owners");
        check(allocator->Reset());
        {
            ext::gpu::Scope scope(list.Get(), allocator.Get()); // abandoned
        }
        check(ext::gpu::ResetCommands(list.Get(), allocator.Get())); // fails if the abandoned scope left it recording
        check(list->Close());
    }

    // 7. Disabled owner: wrappers are plain again.
    ext::gpu::SetOwnerEnabled(handle, false);
    expect(!ext::gpu::SubmissionOwnersActive() && !ext::gpu::OwnersWantReset(), "disabled owner still active");
    {
        auto list = makeList();
        ID3D12CommandList* lists[] { list.Get() };
        const int begins = probe.begins;
        ext::gpu::ExecuteCommands(queue.Get(), 1, lists);
        wait();
        expect(probe.begins == begins, "disabled owner was called");
    }

    std::puts("GPU owners smoke passed");
    return 0;
}
catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
