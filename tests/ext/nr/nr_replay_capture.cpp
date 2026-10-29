// The recorder that takes a replay set from the running pass, run on the Direct3D 12 software adapter with a dispatch of
// its own: resources are copied out before and after a dispatch, shared when they did not change, written as a set that
// the reader accepts, and the set replays to the very bytes the dispatch produced. Whatever cannot be recorded is counted and
// the dispatch index moves on, a set that could not be written completely is reported as such, and a replay of an incomplete
// set fails.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include "ext/nr/diag/DlssNr_Capture.h"
#include "ext/nr/diag/NrReplayCapture.h"
#include "ext/nr/diag/NrReplayEngine.h"

#include <cmath>
#include <cstdio>

using namespace ext::nr::replay;
namespace fs = std::filesystem;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// copies the source, adds gDelta to red at one pixel
const char* kProbe = R"(
cbuffer P : register(b0) { uint gW; uint gH; float gDelta; uint gPx; uint gPy; }
Texture2D<float4> t0 : register(t0);
RWTexture2D<float4> u0 : register(u0);
[numthreads(8,8,1)] void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    float4 v = t0.Load(int3(id.xy, 0));
    if (id.x == gPx && id.y == gPy) v.x += gDelta;
    u0[id.xy] = v;
})";

struct ProbeConstants
{
    uint32_t w, h;
    float delta;
    uint32_t px, py;
};

constexpr uint32_t W = 40, H = 24;
constexpr UINT64 kRowBytes = W * 8;

// exact for the values used here (multiples of 1/64 below 4)
uint16_t FloatToHalf(float f)
{
    if (f == 0.0f)
        return 0;
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const int exp = static_cast<int>((bits >> 23) & 255) - 127 + 15;
    return static_cast<uint16_t>(((bits >> 16) & 0x8000) | (static_cast<uint32_t>(exp) << 10) | ((bits >> 13) & 1023));
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    HANDLE event = nullptr;
    UINT64 value = 0;

    bool Init(const void* code, size_t size)
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return false;
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !CreateRootSignature(device.Get(), root))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root.Get();
        pd.CS = { code, size };
        if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
            return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Wait()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++value);
        if (fence->GetCompletedValue() < value)
        {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, 60000);
        }
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    ComPtr<ID3D12Resource> Texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // Fills a texture (in COPY_DEST) from tight rows and leaves it in the given state.
    void Fill(ID3D12Resource* texture, const std::vector<uint8_t>& tight, D3D12_RESOURCE_STATES to)
    {
        Footprint fp;
        QueryFootprint(device.Get(), texture->GetDesc(), fp);
        ComPtr<ID3D12Resource> upload = CreateBuffer(device.Get(), fp.total, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* mapped = nullptr;
        upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        for (UINT y = 0; y < fp.rows; ++y)
            std::memcpy(mapped + static_cast<size_t>(y) * fp.layout.Footprint.RowPitch, tight.data() + static_cast<size_t>(y) * fp.rowBytes, static_cast<size_t>(fp.rowBytes));
        upload->Unmap(0, nullptr);
        CopyIn(list.Get(), texture, upload.Get(), fp);
        Transition(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, to);
        Wait();
    }
};

// One dispatch the way the pass issues it, recorded around by the recorder.
void RunDispatch(Gpu& gpu, Recorder& rec, ID3D12Resource* source, ID3D12Resource* depth, ID3D12Resource* output, const ProbeConstants& k,
                 bool depthReadable = true)
{
    const UINT step = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpu = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = gpu.heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * step;
        return h;
    };
    for (UINT s = 0; s < 6; ++s)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v {};
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        ID3D12Resource* r = s == 4 ? depth : source;
        v.Format = s == 4 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
        gpu.device->CreateShaderResourceView(r, &v, cpu(s));
    }
    for (UINT s = 0; s < 2; ++s)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
        v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        gpu.device->CreateUnorderedAccessView(output, nullptr, &v, cpu(6 + s));
    }
    ComPtr<ID3D12Resource> cb = CreateBuffer(gpu.device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD);
    void* m = nullptr;
    cb->Map(0, nullptr, &m);
    std::memset(m, 0, 256);
    std::memcpy(m, &k, sizeof(k));
    cb->Unmap(0, nullptr);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv {};
    cv.BufferLocation = cb->GetGPUVirtualAddress();
    cv.SizeInBytes = 256;
    gpu.device->CreateConstantBufferView(&cv, cpu(8));

    std::vector<uint8_t> constants(256, 0);
    std::memcpy(constants.data(), &k, sizeof(k));
    DispatchDesc dd;
    dd.kind = "probe";
    dd.shader = "probe";
    dd.shaderHash = 0x1234;
    dd.groups[0] = (W + 7) / 8;
    dd.groups[1] = (H + 7) / 8;
    dd.constants = constants.data();
    for (uint32_t s = 0; s < 6; ++s)
    {
        BindingDesc& b = dd.bindings[dd.bindingCount++];
        b.slot = s;
        b.resource = s == 4 ? depth : source;
        b.viewFormat = s == 4 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
        b.readable = s == 4 ? depthReadable : true;
    }
    for (uint32_t s = 0; s < 2; ++s)
    {
        BindingDesc& b = dd.bindings[dd.bindingCount++];
        b.uav = true;
        b.slot = s;
        b.resource = output;
        b.viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd);
    ID3D12DescriptorHeap* heaps[] = { gpu.heap.Get() };
    gpu.list->SetDescriptorHeaps(1, heaps);
    gpu.list->SetComputeRootSignature(gpu.root.Get());
    gpu.list->SetPipelineState(gpu.pso.Get());
    gpu.list->SetComputeRootDescriptorTable(0, gpu.heap->GetGPUDescriptorHandleForHeapStart());
    gpu.list->Dispatch(dd.groups[0], dd.groups[1], 1);
    rec.AfterDispatch(gpu.list.Get(), gpu.device.Get());
    gpu.Wait(); // each dispatch is finished before the next descriptors are written
}

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

bool AnyLine(const RunReport& r, const std::string& part)
{
    for (const auto& l : r.lines)
        if (Contains(l, part))
            return true;
    return false;
}

// One frame with one ordinary dispatch, ready to be written.
void RecordOneFrame(Gpu& gpu, Recorder& rec, ID3D12Resource* source, ID3D12Resource* depth, ID3D12Resource* output)
{
    ProbeConstants k { W, H, 0.5f, 5, 3 };
    CHECK(rec.Request(1));
    CHECK(rec.BeginFrame(true));
    RunDispatch(gpu, rec, source, depth, output, k, false);
    rec.EndFrame(50);
    gpu.Wait();
    CHECK(rec.WritePending());
}

// A dispatch as the pass describes it: t0..t5 on the source, u0 and u1 on the output.
DispatchDesc Describe(const std::vector<uint8_t>& constants, ID3D12Resource* srv, ID3D12Resource* uav)
{
    DispatchDesc dd;
    dd.kind = "probe";
    dd.shader = "probe";
    dd.constants = constants.data();
    for (uint32_t s = 0; s < 6; ++s)
    {
        BindingDesc& b = dd.bindings[dd.bindingCount++];
        b.slot = s;
        b.resource = srv;
        b.viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    for (uint32_t s = 0; s < 2; ++s)
    {
        BindingDesc& b = dd.bindings[dd.bindingCount++];
        b.uav = true;
        b.slot = s;
        b.resource = uav;
        b.viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    return dd;
}

// Every way a dispatch can fail to be recorded is counted and moves the index on, so the set shows the gap.
void TestNothingIsLeftOutSilently(Gpu& gpu, ID3D12Resource* source, ID3D12Resource* depth, ID3D12Resource* output)
{
    const std::vector<uint8_t> constants(256, 0);
    Recorder rec;
    CHECK(rec.Request(1));
    CHECK(rec.BeginFrame(true));

    // index 0: an ordinary dispatch, recorded
    ProbeConstants k { W, H, 0.5f, 5, 3 };
    RunDispatch(gpu, rec, source, depth, output, k, false);
    CHECK(rec.DispatchCount() == 1);

    // index 1: no constants
    {
        DispatchDesc dd = Describe(constants, source, output);
        dd.constants = nullptr;
        CHECK(!rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd));
    }
    // index 2: no bindings
    {
        DispatchDesc dd = Describe(constants, source, output);
        dd.bindingCount = 0;
        CHECK(!rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd));
    }
    // index 3: a binding with no resource
    {
        DispatchDesc dd = Describe(constants, source, output);
        dd.bindings[3].resource = nullptr;
        CHECK(!rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd));
    }
    // index 4: a resource that is not a texture
    {
        ComPtr<ID3D12Resource> buffer = CreateBuffer(gpu.device.Get(), 4096, D3D12_HEAP_TYPE_UPLOAD);
        DispatchDesc dd = Describe(constants, buffer.Get(), output);
        CHECK(!rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd));
    }
    // index 5: one resource read and written by the same dispatch
    {
        DispatchDesc dd = Describe(constants, output, output);
        CHECK(!rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd));
    }
    // index 6: a read that cannot be taken because the memory cap is reached
    ComPtr<ID3D12Resource> fresh = gpu.Texture(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    rec.SetMaxPendingBytes(1);
    {
        DispatchDesc dd = Describe(constants, fresh.Get(), output);
        CHECK(!rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd));
    }
    // index 7: the reads are cached but the copy of the outputs after the dispatch cannot be taken
    {
        DispatchDesc dd = Describe(constants, source, output);
        CHECK(rec.BeforeDispatch(gpu.list.Get(), gpu.device.Get(), dd));
        rec.AfterDispatch(gpu.list.Get(), gpu.device.Get());
    }
    // index 8: the pass gave up on a dispatch before it reached the recorder
    rec.DispatchDropped();
    // two boundary pictures that cannot be taken: the cap, and no resource at all
    rec.AddBoundary(gpu.list.Get(), gpu.device.Get(), "final_frame", output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rec.AddBoundary(gpu.list.Get(), gpu.device.Get(), "game_frame", nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CHECK(rec.BoundaryCount() == 0);
    rec.SetMaxPendingBytes(3ull << 30);
    // index 9: recorded again
    RunDispatch(gpu, rec, source, depth, output, k, false);
    CHECK(rec.DispatchCount() == 2);
    rec.EndFrame(60);
    gpu.Wait();

    const fs::path dir = fs::temp_directory_path() / "optiscaler-nr-replay-skips-test";
    fs::remove_all(dir);
    const WriteResult wr = rec.Write(dir);
    CHECK(wr.ok && wr.error.empty());
    CHECK(wr.dispatches == 2 && wr.skipped == 8 && wr.boundariesSkipped == 2);

    SetFiles set;
    std::string error;
    CHECK(LoadSet(dir, set, error));
    const Manifest& m = set.manifest;
    CHECK(m.frames == 1 && m.frameInfos.size() == 1);
    if (!m.frameInfos.empty())
    {
        // ten dispatches were made, eight are not in the set, and the manifest says both
        CHECK(m.frameInfos[0].frame == 0 && m.frameInfos[0].dispatches == 10 && m.frameInfos[0].skipped == 8 && m.frameInfos[0].boundariesSkipped == 2);
    }
    CHECK(m.dispatches.size() == 2 && m.dispatches[0].index == 0 && m.dispatches[1].index == 9); // the gap is in the indices
    const auto problems = CheckComplete(m);
    CHECK(!problems.empty());

    // and a replay refuses it
    Replayer replayer;
    CHECK(replayer.Init(error));
    RunOptions opt;
    opt.setDir = dir;
    opt.outDir = dir / "out";
    opt.warp = GetWarpInfo();
    opt.bless = ParseBless("all");
    opt.shaders = [](const std::string&, Blob&) { return false; };
    const RunReport r = RunReplaySet(opt, replayer);
    CHECK(!r.ok && r.incomplete && r.dispatches == 0);
    CHECK(AnyLine(r, "8 dispatch(es) were left out") && AnyLine(r, "2 boundary picture(s) were left out") && AnyLine(r, "dispatch index 1, 2, 3, 4, 5, 6, 7, 8 missing"));
    fs::remove_all(dir);

    // outside a recording a dropped dispatch changes nothing: the next recorded frame starts at index 0 with nothing left out
    {
        Recorder idle;
        idle.DispatchDropped();
        RecordOneFrame(gpu, idle, source, depth, output);
        const fs::path idleDir = fs::temp_directory_path() / "optiscaler-nr-replay-idle-drop-test";
        fs::remove_all(idleDir);
        const WriteResult iw = idle.Write(idleDir);
        CHECK(iw.ok && iw.skipped == 0 && iw.dispatches == 1);
        SetFiles idleSet;
        CHECK(LoadSet(idleDir, idleSet, error) && !idleSet.manifest.dispatches.empty() && idleSet.manifest.dispatches[0].index == 0);
        fs::remove_all(idleDir);
    }
}

// A set that cannot be written completely is an error, leaves no manifest behind, and frees the recorder for the next capture.
void TestWriteFailuresAreReported(Gpu& gpu, ID3D12Resource* source, ID3D12Resource* depth, ID3D12Resource* output)
{
    const fs::path root = fs::temp_directory_path() / "optiscaler-nr-replay-write-failures";
    fs::remove_all(root);
    fs::create_directories(root);
    const auto next = [&](Recorder& rec) {
        CHECK(!rec.WritePending() && rec.ImageCount() == 0 && rec.DispatchCount() == 0); // freed, so a new capture can start
        CHECK(rec.Request(1));
    };

    // the folder cannot be made: a file sits where a folder should be
    {
        WriteText(root / "blocker", "a file");
        Recorder rec;
        RecordOneFrame(gpu, rec, source, depth, output);
        const WriteResult wr = rec.Write(root / "blocker" / "replay-1");
        CHECK(!wr.ok && Contains(wr.error, "cannot create") && wr.directory.empty());
        next(rec);
    }
    // an image cannot be written: its file name is taken by a folder
    {
        const fs::path dir = root / "replay-2";
        fs::create_directories(dir / "images" / "img_0000.raw");
        WriteText(dir / "keep.txt", "was here before");
        Recorder rec;
        RecordOneFrame(gpu, rec, source, depth, output);
        const WriteResult wr = rec.Write(dir);
        CHECK(!wr.ok && Contains(wr.error, "cannot write") && Contains(wr.error, "img_0000.raw") && wr.directory.empty());
        CHECK(!fs::exists(dir / "manifest.txt")); // nothing that looks like a set is left
        // what was in the folder before stays: the failure removes only what this write opened
        CHECK(fs::is_regular_file(dir / "keep.txt") && fs::is_directory(dir / "images" / "img_0000.raw"));
        next(rec);
    }
    // a later image cannot be written: the earlier images this write made are removed, the folder it found stays
    {
        const fs::path dir = root / "replay-2b";
        fs::create_directories(dir / "images" / "img_0001.raw");
        WriteText(dir / "keep.txt", "was here before");
        Recorder rec;
        RecordOneFrame(gpu, rec, source, depth, output);
        const WriteResult wr = rec.Write(dir);
        CHECK(!wr.ok && Contains(wr.error, "img_0001.raw"));
        CHECK(!fs::exists(dir / "images" / "img_0000.raw") && !fs::exists(dir / "manifest.txt"));
        CHECK(fs::is_regular_file(dir / "keep.txt") && fs::is_directory(dir / "images" / "img_0001.raw"));
        next(rec);
    }
    // the manifest cannot be written
    {
        const fs::path dir = root / "replay-3";
        fs::create_directories(dir / "manifest.txt");
        Recorder rec;
        RecordOneFrame(gpu, rec, source, depth, output);
        const WriteResult wr = rec.Write(dir);
        CHECK(!wr.ok && Contains(wr.error, "cannot write") && Contains(wr.error, "manifest.txt") && wr.directory.empty());
        CHECK(!fs::is_regular_file(dir / "manifest.txt"));
        next(rec);
    }
    // nothing waiting
    {
        Recorder rec;
        const WriteResult wr = rec.Write(root / "replay-4");
        CHECK(!wr.ok && !wr.error.empty() && !fs::exists(root / "replay-4"));
    }
    // and the same recorder writes a good set afterwards
    {
        const fs::path dir = root / "replay-5";
        Recorder rec;
        RecordOneFrame(gpu, rec, source, depth, output);
        const WriteResult wr = rec.Write(dir);
        CHECK(wr.ok && wr.dispatches == 1 && wr.skipped == 0 && wr.boundariesSkipped == 0 && wr.directory == dir.string());
        SetFiles set;
        std::string error;
        CHECK(LoadSet(dir, set, error) && CheckComplete(set.manifest).empty());
    }
    fs::remove_all(root);
}
} // namespace

int wmain()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kProbe, std::strlen(kProbe), "probe", nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        return 2;
    Gpu gpu;
    if (!gpu.Init(code->GetBufferPointer(), code->GetBufferSize()))
    {
        std::printf("no software adapter\n");
        return 2;
    }
    gpu.allocator->Reset();
    gpu.list->Reset(gpu.allocator.Get(), nullptr);

    // the frame the pass would be working on: a typeless half-float colour, a depth+stencil buffer and an output
    std::vector<uint8_t> src(static_cast<size_t>(kRowBytes) * H);
    for (size_t i = 0; i < src.size() / 2; ++i)
    {
        const uint16_t h = FloatToHalf(static_cast<float>((i * 37 + 11) % 100) / 64.0f);
        std::memcpy(&src[i * 2], &h, 2);
    }
    ComPtr<ID3D12Resource> source = gpu.Texture(W, H, DXGI_FORMAT_R16G16B16A16_TYPELESS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> depth = gpu.Texture(W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> output = gpu.Texture(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::vector<uint8_t> depthPixels(static_cast<size_t>(W) * 4 * H, 0);
    for (size_t i = 0; i < depthPixels.size() / 4; ++i)
    {
        const float f = static_cast<float>(i % 97) / 97.0f;
        std::memcpy(&depthPixels[i * 4], &f, 4);
    }
    gpu.Fill(source.Get(), src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Fill(depth.Get(), depthPixels, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // ---- nothing is recorded unless the key is on and a capture was asked for ----
    {
        Recorder idle;
        ProbeConstants k { W, H, 0.5f, 5, 3 };
        CHECK(!idle.BeginFrame(true)); // never asked for
        RunDispatch(gpu, idle, source.Get(), depth.Get(), output.Get(), k);
        CHECK(idle.ImageCount() == 0 && idle.DispatchCount() == 0);
        CHECK(idle.Request(2));
        CHECK(!idle.Request(2)); // a second request while one is waiting is ignored
        CHECK(!idle.BeginFrame(false)); // asked for, key off
        RunDispatch(gpu, idle, source.Get(), depth.Get(), output.Get(), k);
        CHECK(idle.ImageCount() == 0 && idle.DispatchCount() == 0 && !idle.Recording());
        CHECK(idle.BeginFrame(true)); // key on: recording starts
        idle.EndFrame(10);
        CHECK(idle.BeginFrame(true));
        idle.EndFrame(11);
        CHECK(!idle.Requested() && idle.WritePending() && idle.FramesRecorded() == 2);
        CHECK(!idle.BeginFrame(true)); // two frames asked for, two recorded
        CHECK(!idle.ReadyToWrite(11 + kReadbackDelayFrames - 1) && idle.ReadyToWrite(11 + kReadbackDelayFrames));
        CHECK(!idle.Request(2)); // a capture waiting to be written blocks a new one
    }

    // ---- a capture of two frames ----
    Recorder rec;
    CHECK(rec.Request(2));
    ProbeConstants k0 { W, H, 0.5f, 5, 3 }, k1 { W, H, 0.25f, 9, 4 };
    CHECK(rec.BeginFrame(true) && rec.Recording());
    RunDispatch(gpu, rec, source.Get(), depth.Get(), output.Get(), k0, false);
    const size_t afterFirst = rec.ImageCount();
    // the source is bound to five slots, the output to two, the depth (not readable) to one: sources and the output are copied once each
    CHECK(rec.DispatchCount() == 1 && afterFirst == 2);
    // reading the same unchanged source again copies nothing for it; the output is copied after the dispatch
    RunDispatch(gpu, rec, source.Get(), depth.Get(), output.Get(), k0, false);
    CHECK(rec.DispatchCount() == 2 && rec.ImageCount() == afterFirst + 1);
    // after a write is noted the source is copied again
    rec.NoteWrite(source.Get());
    RunDispatch(gpu, rec, source.Get(), depth.Get(), output.Get(), k0, false);
    CHECK(rec.ImageCount() == afterFirst + 1 + 2);
    rec.AddBoundary(gpu.list.Get(), gpu.device.Get(), "game_frame", source.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CHECK(rec.BoundaryCount() == 1 && rec.ImageCount() == afterFirst + 1 + 2); // the cached copy is reused
    rec.EndFrame(100);
    CHECK(!rec.WritePending() && rec.Requested());

    CHECK(rec.BeginFrame(true));
    RunDispatch(gpu, rec, source.Get(), depth.Get(), output.Get(), k1, true); // the depth plane is readable this time
    rec.AddBoundary(gpu.list.Get(), gpu.device.Get(), "final_frame", output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Wait();
    rec.EndFrame(101);
    CHECK(rec.WritePending() && !rec.ReadyToWrite(100) && rec.ReadyToWrite(101 + kReadbackDelayFrames));

    const fs::path dir = fs::temp_directory_path() / "optiscaler-nr-replay-capture-test";
    fs::remove_all(dir);
    const WriteResult wr = rec.Write(dir);
    CHECK(wr.ok && wr.directory == dir.string() && wr.dispatches == 4 && wr.skipped == 0 && wr.boundariesSkipped == 0);
    CHECK(rec.ImageCount() == 0 && rec.DispatchCount() == 0 && !rec.WritePending()); // everything is freed
    std::printf("    wrote %u dispatches, %u distinct images from %u copies, %llu bytes\n", wr.dispatches, wr.images, wr.copies,
                static_cast<unsigned long long>(wr.bytes));
    CHECK(wr.images < wr.copies); // the same content is stored once

    // ---- the set reads back and says what was recorded ----
    SetFiles set;
    std::string error;
    CHECK(LoadSet(dir, set, error));
    if (!error.empty())
        std::printf("    %s\n", error.c_str());
    const Manifest& m = set.manifest;
    CHECK(m.frames == 2 && m.dispatches.size() == 4 && m.boundaries.size() == 2);
    // nothing was left out: the manifest says how many dispatches each frame had, and the set is complete
    CHECK(m.frameInfos.size() == 2 && m.frameInfos[0].dispatches == 3 && m.frameInfos[1].dispatches == 1);
    CHECK(m.frameInfos[0].skipped == 0 && m.frameInfos[1].skipped == 0 && m.frameInfos[0].boundariesSkipped == 0 && m.frameInfos[1].boundariesSkipped == 0);
    CHECK(CheckComplete(m).empty());
    CHECK(m.dispatches[0].frame == 0 && m.dispatches[0].index == 0 && m.dispatches[1].index == 1 && m.dispatches[2].index == 2);
    CHECK(m.dispatches[3].frame == 1 && m.dispatches[3].index == 0);
    const Dispatch& d0 = m.dispatches[0];
    CHECK(d0.kind == "probe" && d0.shader == "probe" && d0.shaderHash == 0x1234 && d0.groups[0] == 5 && d0.groups[1] == 3);
    ProbeConstants back;
    std::memcpy(&back, d0.constants.data(), sizeof(back));
    CHECK(back.px == 5 && back.py == 3 && back.delta == 0.5f && d0.constants[255] == 0);
    CHECK(d0.bindings.size() == 8);
    CHECK(d0.resources.size() == 3); // source, depth, output: the aliasing of slots is kept as one resource each
    int srv = 0, uav = 0;
    for (const Binding& b : d0.bindings)
    {
        (b.uav ? uav : srv)++;
        CHECK(b.uav ? (b.viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT && b.state == kStateUnorderedAccess)
                    : (b.state == kStateNonPixelShaderResource));
        CHECK(b.mips == 1 && !b.array);
    }
    CHECK(srv == 6 && uav == 2);
    // the typeless source is stored with its declared format and read in the float format
    const Resource& rs = d0.resources[0];
    CHECK(rs.resourceFormat == DXGI_FORMAT_R16G16B16A16_TYPELESS && rs.copyFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(rs.width == W && rs.height == H && rs.before != kNoImage && rs.after == kNoImage);
    CHECK(d0.bindings[4].viewFormat == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    // the depth plane that was not readable has no image; where it was readable it has
    CHECK(d0.resources[1].resourceFormat == DXGI_FORMAT_R32G8X24_TYPELESS && d0.resources[1].before == kNoImage);
    CHECK(m.dispatches[3].resources[1].before != kNoImage);
    const Image* depthImage = FindImage(m, m.dispatches[3].resources[1].before);
    CHECK(depthImage != nullptr && depthImage->rowBytes == W * 4 && depthImage->height == H);
    CHECK(depthImage != nullptr && depthImage->srcRowPitch >= depthImage->rowBytes);
    // the output was written: it has an image after, none before
    CHECK(d0.resources[2].before == kNoImage && d0.resources[2].after != kNoImage);

    // image content: the source as uploaded, the output as the dispatch wrote it, the depth plane as uploaded
    std::vector<uint8_t> img;
    CHECK(ReadImage(set, rs.before, img, error) && img == src);
    CHECK(ReadImage(set, m.dispatches[3].resources[1].before, img, error) && img == depthPixels);
    CHECK(ReadImage(set, d0.resources[2].after, img, error));
    {
        std::vector<uint8_t> expected = src;
        uint16_t red;
        std::memcpy(&red, &expected[(3 * W + 5) * 8], 2);
        const uint16_t moved = FloatToHalf(HalfToFloat(red) + 0.5f);
        std::memcpy(&expected[(3 * W + 5) * 8], &moved, 2);
        CHECK(img == expected);
    }
    // the boundary names the same picture as the source, by content (stored once)
    CHECK(m.boundaries[0].name == "game_frame" && m.boundaries[0].image == rs.before && m.boundaries[0].frame == 0);
    CHECK(m.boundaries[1].name == "final_frame" && m.boundaries[1].frame == 1);

    // ---- the on-demand before/after capture: the "before" is the frame that was handed in, and a pair is one frame ----
    {
        capture::FrameCapture fc;
        fc.request(1);
        fc.recordAfter(gpu.list.Get(), gpu.device.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CHECK(fc.progress() == 0); // an "after" with no "before" is not a pair
        fc.recordBefore(gpu.list.Get(), gpu.device.Get(), source.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CHECK(fc.progress() == 0 && !fc.readyToWrite()); // the frame counts once both halves are in
        fc.recordAfter(gpu.list.Get(), gpu.device.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CHECK(fc.progress() == 1 && fc.readyToWrite());
        gpu.Wait();
        const fs::path pairDir = fs::temp_directory_path() / "optiscaler-nr-capture-pair-test";
        fs::remove_all(pairDir);
        CHECK(!fc.write(pairDir).empty());
        std::vector<uint8_t> beforeRaw, afterRaw;
        CHECK(ReadFile(pairDir / "before_00.raw", beforeRaw) && ReadFile(pairDir / "after_00.raw", afterRaw));
        // the dump is the readback as copied: rows padded to 256 bytes, the last row not padded
        const size_t pitch = (kRowBytes + 255) / 256 * 256;
        bool beforeIsSource = beforeRaw.size() == pitch * (H - 1) + kRowBytes;
        bool afterIsOutput = afterRaw.size() == beforeRaw.size();
        std::vector<uint8_t> expectedOut = src;
        uint16_t red;
        std::memcpy(&red, &expectedOut[(4 * W + 9) * 8], 2);
        const uint16_t moved = FloatToHalf(HalfToFloat(red) + 0.25f);
        std::memcpy(&expectedOut[(4 * W + 9) * 8], &moved, 2);
        for (uint32_t y = 0; beforeIsSource && afterIsOutput && y < H; ++y)
        {
            beforeIsSource = beforeIsSource && std::memcmp(&beforeRaw[y * pitch], &src[y * kRowBytes], kRowBytes) == 0;
            afterIsOutput = afterIsOutput && std::memcmp(&afterRaw[y * pitch], &expectedOut[y * kRowBytes], kRowBytes) == 0;
        }
        CHECK(beforeIsSource); // the frame handed to recordBefore, byte for byte
        CHECK(afterIsOutput);  // the frame handed to recordAfter
        CHECK(beforeRaw != afterRaw);
        fs::remove_all(pairDir);
    }

    // ---- and it replays: what was recorded from the software adapter replays to the same bytes ----
    Replayer replayer;
    CHECK(replayer.Init(error));
    RunOptions opt;
    opt.setDir = dir;
    opt.outDir = dir / "out";
    opt.commit = "capture-test";
    opt.warp = GetWarpInfo();
    opt.bless = ParseBless("all");
    opt.shaders = [&](const std::string& id, Blob& b) {
        if (id != "probe")
            return false;
        b = { code->GetBufferPointer(), code->GetBufferSize() };
        return true;
    };
    RunReport r = RunReplaySet(opt, replayer);
    for (const auto& l : r.lines)
        if (!r.ok)
            std::printf("    %s\n", l.c_str());
    CHECK(r.ok && r.blessed == 4);
    CHECK(r.comparedToCaptured >= 4 && r.maxErrorVsCaptured == 0.0); // the same adapter wrote them: not one bit apart
    opt.bless = {};
    r = RunReplaySet(opt, replayer);
    CHECK(r.ok && r.matched == 4);

    fs::remove_all(dir);

    TestNothingIsLeftOutSilently(gpu, source.Get(), depth.Get(), output.Get());
    TestWriteFailuresAreReported(gpu, source.Get(), depth.Get(), output.Get());
    std::printf(failures == 0 ? "nr_replay_capture: all checks passed\n" : "nr_replay_capture: %d check(s) failed\n", failures);
    return failures;
}
