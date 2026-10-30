// The retirement-sequence gate the frame-rate governor waits on (ext::gpu::GpuLifetime): the mark counts
// retirements, completeness asks only about retirements queued at or before the mark, and a later retirement never
// holds an earlier mark. Runs on the D3D12 software adapter like the lifetime smoke test.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include "ext/gpu/GpuLifetime.h"

using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed"); }
static void expect(bool yes, const char* why)
{
    if (!yes)
        throw std::runtime_error(why);
}

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
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)));
    check(commands->Close());
    ID3D12CommandList* lists[] { commands.Get() };
    ComPtr<ID3D12Fence> gate, drain;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
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

    // the empty set releases at once: a governor tick with no size change never waits
    {
        ext::gpu::GpuLifetime life;
        expect(life.RetireSeq() == 0, "fresh mark is not zero");
        expect(life.RetirementsComplete(0), "empty set not released");
        expect(life.RetirementsComplete(life.RetireSeq()), "fresh mark not released");
    }
    // the mark counts retirements, one by one
    {
        ext::gpu::GpuLifetime life;
        int released = 0;
        const uint64_t s0 = life.RetireSeq();
        life.Retire([&] { ++released; });
        const uint64_t s1 = life.RetireSeq();
        life.Retire([&] { ++released; });
        const uint64_t s2 = life.RetireSeq();
        expect(s1 == s0 + 1 && s2 == s1 + 1, "mark is not monotonic");
        life.Collect();
        expect(released == 2, "bare retirements not released");
        expect(life.RetirementsComplete(s2), "released retirements still hold");
    }
    // false while the step's retirements pend, true again after the fences pass
    {
        ext::gpu::GpuLifetime life;
        int released = 0;
        life.Record(commands.Get());
        check(queue->Wait(gate.Get(), 1));
        queue->ExecuteCommandLists(1, lists);
        life.Submitted(queue.Get(), 1, lists);
        life.Retire([&] { ++released; });
        life.ResetRecording(commands.Get()); // the list is recycled; the fence still holds the work
        const uint64_t mark = life.RetireSeq();
        life.Collect();
        expect(released == 0 && !life.RetirementsComplete(mark), "pending retirement reads released");
        check(gate->Signal(1));
        wait();
        life.Collect();
        expect(released == 1, "fenced retirement not released");
        expect(life.RetirementsComplete(mark), "fenced retirement still holds after release");
    }
    // later retirements never hold an earlier mark: an unrelated pending retirement does not park the gate
    {
        ext::gpu::GpuLifetime life;
        int releasedA = 0, releasedB = 0;
        life.Record(commands.Get());
        check(queue->Wait(gate.Get(), 2));
        queue->ExecuteCommandLists(1, lists);
        life.Submitted(queue.Get(), 1, lists);
        life.Retire([&] { ++releasedA; });
        life.ResetRecording(commands.Get());
        const uint64_t markA = life.RetireSeq();
        check(gate->Signal(2));
        wait();
        life.Collect();
        expect(releasedA == 1, "first retirement not released");
        expect(life.RetirementsComplete(markA), "released mark not released");
        life.Record(commands.Get()); // an unrelated recording, never submitted
        life.Retire([&] { ++releasedB; });
        const uint64_t markB = life.RetireSeq();
        expect(markB == markA + 1, "mark did not advance");
        life.Collect();
        expect(releasedB == 0 && life.RetirementsComplete(markA), "later pending retirement holds the earlier mark");
        expect(!life.RetirementsComplete(markB), "pending retirement reads released");
        life.ResetRecording(commands.Get());
        life.Collect();
        expect(releasedB == 1 && life.RetirementsComplete(markB), "discarded retirement not released");
    }
    // a retirement spanning two recordings releases only when both lists work has run
    {
        ComPtr<ID3D12CommandAllocator> allocator2;
        ComPtr<ID3D12GraphicsCommandList> commands2;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator2)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator2.Get(), nullptr,
                                        IID_PPV_ARGS(&commands2)));
        check(commands2->Close());
        ID3D12CommandList* listsB[] { commands2.Get() };
        ext::gpu::GpuLifetime life;
        int released = 0;
        life.Record(commands.Get());
        life.Record(commands2.Get());
        check(queue->Wait(gate.Get(), 5));
        queue->ExecuteCommandLists(1, lists);
        life.Submitted(queue.Get(), 1, lists);
        check(queue->Wait(gate.Get(), 6));
        queue->ExecuteCommandLists(1, listsB);
        life.Submitted(queue.Get(), 1, listsB);
        life.Retire([&] { ++released; });
        const uint64_t mark = life.RetireSeq();
        life.ResetRecording(commands.Get());
        life.ResetRecording(commands2.Get());
        check(gate->Signal(5)); // the first list runs; the second stays gated
        life.Collect();
        expect(released == 0 && !life.RetirementsComplete(mark), "half-fenced retirement not held");
        check(gate->Signal(6));
        wait();
        life.Collect();
        expect(released == 1 && life.RetirementsComplete(mark), "fully fenced retirement not released");
    }
    std::puts("PASS: retirement-sequence gate (monotonic mark, pending hold, later retirements never hold)");
    return 0;
}
catch (const std::exception& e)
{
    std::printf("FAIL: %s\n", e.what());
    return 1;
}
