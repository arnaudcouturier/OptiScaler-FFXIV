// NR DescriptorSlots<N> (ext/nr/pass/DlssNr_DescriptorSlots.h) on WARP: frame budget, ring wrap, replayable recordings and
// abandoned submissions. Was part of nr_gpu_lifetime_smoke; the GpuLifetime blocks moved to tests/ext/gpu/gpu_lifetime_smoke
// with the C6 GpuLifetime move (S4), these blocks return here as an NR test.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>
#include <Util.h>
#include "ext/gpu/GpuLifetime.h"
#include "ext/nr/pass/DlssNr_DescriptorSlots.h"
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed"); }
static void expect(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }
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
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    check(commands->Close());
    ID3D12CommandList* lists[] { commands.Get() };
    ComPtr<ID3D12Fence> gate, drain;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)));
    UINT64 serial = 0;
    auto waitOn = [&](ID3D12CommandQueue* submittedQueue)
    {
        check(submittedQueue->Signal(drain.Get(), ++serial));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(drain->SetEventOnCompletion(serial, event));
        const auto result = WaitForSingleObject(event, 5000);
        CloseHandle(event);
        expect(result == WAIT_OBJECT_0, "GPU did not drain");
    };
    auto wait = [&] { waitOn(queue.Get()); };
    {
        DlssNr::DescriptorSlots<2> slots;
        expect(slots.HasCapacity(2) && !slots.HasCapacity(3), "descriptor frame budget is incorrect");
        expect(slots.Acquire(commands.Get()).has_value(), "first descriptor lease failed");
        expect(slots.Acquire(commands.Get()).has_value(), "second descriptor lease failed");
        expect(!slots.Acquire(commands.Get()), "live descriptor ring wrapped");
        expect(!slots.HasCapacity(1), "frame budget allowed an incomplete NR recording");
        bool codecReleased = false;
        slots.Retire([&] { codecReleased = true; });
        expect(!codecReleased, "codec released while its list remained replayable");
        check(queue->Wait(gate.Get(), 99));
        auto captured = slots.BeginSubmission(1, lists);
        queue->ExecuteCommandLists(1, lists);
        // Reset can arrive before the post-Execute notification: captured pins
        // must preserve the original generation rather than signal a new one.
        slots.ResetRecording(commands.Get());
        expect(!slots.Acquire(commands.Get()), "pending submission released descriptors");
        captured.Complete(queue.Get());
        expect(!slots.Acquire(commands.Get()), "blocked GPU released descriptors");
        expect(!codecReleased, "codec released before its captured GPU execution");
        check(gate->Signal(99)); wait();
        expect(slots.Acquire(commands.Get()).has_value(), "completed descriptor lease was not reclaimed");
        expect(codecReleased, "completed codec recording did not retire");
        slots.ResetRecording(commands.Get());
    }
    {
        DlssNr::DescriptorSlots<1> slots;
        expect(slots.Acquire(commands.Get()).has_value(), "abandon test acquisition failed");
        { auto abandoned = slots.BeginSubmission(1, lists); }
        slots.ResetRecording(commands.Get());
        expect(!slots.Acquire(commands.Get()), "abandoned submission allowed descriptor overwrite");
    }
    {
        // The token of a lease: the generation in the high bits, the slot in the low ones (two slots: one slot bit). A lease handed out
        // again has the next generation, so a token of the earlier one, or generation 0 (never handed out), is refused.
        DlssNr::DescriptorSlots<2> slots;
        const auto first = slots.Acquire(commands.Get());
        expect(first.has_value(), "token test: the first lease failed");
        uint32_t token = UINT32_MAX;
        expect(slots.PublishImmutable(commands.Get(), *first, token), "token test: the lease could not be published");
        expect(token == ((1u << 1) | *first), "the token does not carry the first generation of the slot");
        const auto again = slots.Acquire(commands.Get(), &token);
        expect(again.has_value() && *again == *first, "the published token was not accepted for its own slot");
        uint32_t otherGeneration = (2u << 1) | *first, neverHandedOut = *first;
        expect(!slots.Acquire(commands.Get(), &otherGeneration).has_value(), "a token of another generation was accepted");
        expect(!slots.Acquire(commands.Get(), &neverHandedOut).has_value(), "a token of generation 0 was accepted");
        slots.ResetRecording(commands.Get());
    }
    {
        // A token of an older generation than the slot is refused: the slot has since been leased again, so the token names a
        // recording that no longer owns it (any generation mismatch refuses, an older one included).
        DlssNr::DescriptorSlots<2> slots;
        const auto first = slots.Acquire(commands.Get());
        expect(first.has_value(), "stale token test: the first lease failed");
        uint32_t stale = UINT32_MAX;
        expect(slots.PublishImmutable(commands.Get(), *first, stale), "stale token test: the lease could not be published");
        slots.ResetRecording(commands.Get());
        const auto second = slots.Acquire(commands.Get());
        const auto third = slots.Acquire(commands.Get());
        expect(second.has_value() && third.has_value(), "stale token test: the re-leases failed");
        expect(!slots.Acquire(commands.Get(), &stale).has_value(), "a token of an older generation than the slot was accepted");
        slots.ResetRecording(commands.Get());
    }
    {
        // The ring continues after the slot it handed out last, whichever slots are free: with the first and the third slot released and
        // the second still held, the next lease is the first (a ring that restarts at the slot it last handed out would give the third).
        ComPtr<ID3D12CommandAllocator> allocator2;
        ComPtr<ID3D12GraphicsCommandList> commands2;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator2)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator2.Get(), nullptr, IID_PPV_ARGS(&commands2)));
        check(commands2->Close());
        DlssNr::DescriptorSlots<3> slots;
        const auto a = slots.Acquire(commands.Get());
        const auto b = slots.Acquire(commands2.Get());
        const auto c = slots.Acquire(commands.Get());
        expect(a && b && c && *a == 0 && *b == 1 && *c == 2, "the ring did not hand the slots out in order");
        slots.ResetRecording(commands.Get());
        const auto d = slots.Acquire(commands.Get());
        expect(d.has_value() && *d == 0, "the ring did not continue after the last slot it handed out");
        slots.ResetRecording(commands.Get());
        slots.ResetRecording(commands2.Get());
    }
    std::puts("NR descriptor slots smoke passed");
    return 0;
}
catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
