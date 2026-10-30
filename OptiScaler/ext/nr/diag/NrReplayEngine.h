// Replays the dispatches of a recorded NR frame on the Direct3D 12 software adapter, and checks them against the blessed
// images. Used by the tests only; the game never includes this file.
//
// A replay recreates, for each dispatch on its own, the resources it bound (contents as they were before the dispatch),
// the constants and the views, runs the compute shader it names, and reads the outputs back. The root signature is the one
// the pass builds: one table of six shader views, two unordered views and one constant buffer, and a clamped linear sampler.
// Each dispatch starts from its recorded inputs, not from the previous replay's outputs, so one changed shader shows up in
// the dispatches that run it and not in every later one.
//
// A set that is not complete (a dispatch left out, an index missing, an empty frame) is refused before anything runs, and a
// replay that times out on the GPU abandons the rest of the set: the objects the GPU may still be using are kept alive and
// never reused.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "ext/nr/diag/NrReplayCompare.h"
#include "ext/nr/diag/NrReplayD3D.h"

#include <cstdarg>
#include <ctime>
#include <functional>

#pragma comment(lib, "version.lib")

namespace ext::nr::replay
{
inline constexpr UINT kAppendRange = 0xffffffff; // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND_FROM_PREVIOUS_RANGE_END

// ---- a set on disk ---------------------------------------------------------------------------------------------------

struct SetFiles
{
    std::filesystem::path dir;
    Manifest manifest;
};

inline bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const std::streamsize size = f.tellg();
    f.seekg(0);
    out.resize(static_cast<size_t>(size));
    return size == 0 || static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

// False when the folder could not be made, the file could not be opened, or any byte did not reach the disk (the stream is
// checked after it is closed, which is where a full disk shows).
inline bool WriteFile(const std::filesystem::path& path, const void* data, size_t size)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    f.close();
    return !f.fail();
}

inline bool WriteText(const std::filesystem::path& path, const std::string& text)
{
    return WriteFile(path, text.data(), text.size());
}

inline bool LoadSet(const std::filesystem::path& dir, SetFiles& set, std::string& error)
{
    std::vector<uint8_t> bytes;
    if (!ReadFile(dir / "manifest.txt", bytes))
    {
        error = "no manifest.txt in " + dir.string();
        return false;
    }
    const ParseResult parsed = ParseManifest(std::string(bytes.begin(), bytes.end()));
    if (!parsed.ok)
    {
        error = "manifest: " + parsed.error;
        return false;
    }
    set.dir = dir;
    set.manifest = parsed.manifest;
    return true;
}

inline const Image* FindImage(const Manifest& m, uint32_t id)
{
    for (const Image& i : m.images)
        if (i.id == id)
            return &i;
    return nullptr;
}

// The tightly packed rows of an image, checked against its recorded size and hash.
inline bool ReadImage(const SetFiles& set, uint32_t id, std::vector<uint8_t>& out, std::string& error)
{
    const Image* image = FindImage(set.manifest, id);
    if (image == nullptr)
    {
        error = "unknown image " + std::to_string(id);
        return false;
    }
    if (!ReadFile(set.dir / image->file, out) || out.size() != image->bytes)
    {
        error = "image file " + image->file + " is missing or has the wrong size";
        return false;
    }
    if (HashRows(out.data(), image->rowBytes, image->rowBytes, image->height) != image->hash)
    {
        error = "image file " + image->file + " does not match its recorded hash";
        return false;
    }
    return true;
}

// ---- the software adapter ------------------------------------------------------------------------------------------

// The version and the hash of the software adapter's file. When either cannot be read the result says why (error) and is never
// a match for anything. The path is a parameter so a test can name a file that is not there.
inline WarpInfo GetWarpInfo(const std::filesystem::path& file)
{
    WarpInfo info;
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
    if (size != 0)
    {
        std::vector<uint8_t> data(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(file.c_str(), 0, size, data.data()) && VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) &&
            fixed != nullptr)
            info.version = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
                           std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
    }
    std::vector<uint8_t> bytes;
    if (ReadFile(file, bytes) && !bytes.empty())
        info.hash = Fnv1a64(bytes.data(), bytes.size());
    if (info.version.empty())
        info.version = "unknown";
    if (info.hash == 0)
        info.error = "cannot read " + file.string();
    else if (info.version == "unknown")
        info.error = "cannot read the version of " + file.string();
    return info;
}

inline WarpInfo GetWarpInfo()
{
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    return GetWarpInfo(std::filesystem::path(dir) / L"d3d10warp.dll");
}

// One output of a replayed dispatch: the tightly packed rows of a unordered-access resource after the dispatch.
struct Output
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; // the copy format
    uint32_t rowBytes = 0;
    std::vector<uint8_t> bytes;
};

// The root signature of the NR pass: six shader views, two unordered views and one constant buffer in one table, and the
// clamped linear sampler.
inline bool CreateRootSignature(ID3D12Device* device, ComPtr<ID3D12RootSignature>& out)
{
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, kAppendRange };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, kAppendRange };
    ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, kAppendRange };
    D3D12_ROOT_PARAMETER param {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = { 3, ranges };
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs {};
    rs.NumParameters = 1;
    rs.pParameters = &param;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out))))
        return false;
    return true;
}

class Replayer
{
  public:
    bool Init(std::string& error)
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))))
            return Fail(error, "no software adapter");
        DXGI_ADAPTER_DESC adapter {};
        if (FAILED(warp->GetDesc(&adapter)) || !AdapterIsWarp(adapter.VendorId, adapter.DeviceId))
            return Fail(error, "the adapter is not the software adapter; a replay is only comparable on that one");
        if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))))
            return Fail(error, "cannot create a device on the software adapter");

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_))) ||
            FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_))) ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
            return Fail(error, "cannot create the command objects");
        list_->Close();
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        if (!CreateRootSignature(device_.Get(), root_))
            return Fail(error, "cannot create the root signature");
        return true;
    }

    ~Replayer()
    {
        if (event_ != nullptr)
            CloseHandle(event_);
    }

    ID3D12Device* Device() const { return device_.Get(); }

    // After a GPU timeout, or a removed device, nothing more is replayed: the objects of the failed dispatch may still be in
    // use by the GPU, so the command objects are not reused.
    bool Abandoned() const { return abandoned_; }

    // The longest the GPU may take for one dispatch. WARP at 3840x2160 needs far longer than a small test image does.
    static constexpr DWORD kWaitMilliseconds = 10 * 60 * 1000;

    // Test hook: the Nth replay from now on is treated as a timeout (after the GPU finished it, so nothing is destroyed while
    // in use). 0 turns it off.
    void SimulateTimeoutAtRun(uint32_t nth) { simulateAt_ = nth; }

    // Replays one dispatch with the given compute shader. Outputs are keyed by unordered-access slot.
    bool Run(const SetFiles& set, const Dispatch& d, const void* code, size_t codeSize, std::map<uint32_t, Output>& outputs,
             std::string& error)
    {
        outputs.clear();
        if (abandoned_)
            return Fail(error, "the replay of this set was abandoned after a GPU timeout or a removed device");
        ID3D12PipelineState* pso = Pipeline(code, codeSize);
        if (pso == nullptr)
            return Fail(error, "the compute shader would not load");

        // Which resources are written, which are read.
        std::map<uint32_t, bool> written;
        for (const Binding& b : d.bindings)
            written[b.res] = written[b.res] || b.uav;

        struct Live
        {
            ComPtr<ID3D12Resource> texture, upload, readback;
            Footprint fp;
            bool uav = false;
        };
        std::map<uint32_t, Live> live;

        if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
            return Fail(error, "cannot reset the command list");
        // A replay that stops early leaves the list closed, so the next one can reset it.
        struct CloseList
        {
            ID3D12GraphicsCommandList* list;
            bool open = true;
            ~CloseList()
            {
                if (open)
                    list->Close();
            }
        } closer { list_.Get() };

        for (const Resource& r : d.resources)
        {
            Live& l = live[r.local];
            l.uav = written[r.local];
            D3D12_RESOURCE_DESC desc {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = r.width;
            desc.Height = r.height;
            desc.DepthOrArraySize = static_cast<UINT16>(r.arraySize);
            desc.MipLevels = static_cast<UINT16>(r.mips);
            desc.Format = static_cast<DXGI_FORMAT>(r.resourceFormat);
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (l.uav)
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            else if (IsDepthResourceFormat(r.resourceFormat))
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                nullptr, IID_PPV_ARGS(&l.texture));
            if (FAILED(hr))
                return Fail(error, "cannot create resource " + std::to_string(r.local) + " of format " + std::to_string(r.resourceFormat) +
                                       " (hr " + Hex64(static_cast<uint64_t>(static_cast<uint32_t>(hr))).substr(8) + ")");
            if (!QueryFootprint(device_.Get(), desc, l.fp))
                return Fail(error, "no copy footprint for resource " + std::to_string(r.local));

            if (r.before != kNoImage)
            {
                std::vector<uint8_t> pixels;
                std::string why;
                if (!ReadImage(set, r.before, pixels, why))
                    return Fail(error, why);
                if (pixels.size() != static_cast<size_t>(l.fp.rowBytes) * l.fp.rows)
                    return Fail(error, "image " + std::to_string(r.before) + " does not fit resource " + std::to_string(r.local));
                l.upload = CreateBuffer(device_.Get(), l.fp.total, D3D12_HEAP_TYPE_UPLOAD);
                if (!l.upload)
                    return Fail(error, "cannot create an upload buffer");
                uint8_t* mapped = nullptr;
                l.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
                for (UINT y = 0; y < l.fp.rows; ++y)
                    std::memcpy(mapped + l.fp.layout.Offset + static_cast<size_t>(y) * l.fp.layout.Footprint.RowPitch,
                                pixels.data() + static_cast<size_t>(y) * l.fp.rowBytes, static_cast<size_t>(l.fp.rowBytes));
                l.upload->Unmap(0, nullptr);
                CopyIn(list_.Get(), l.texture.Get(), l.upload.Get(), l.fp);
            }
            Transition(list_.Get(), l.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       l.uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // The descriptor table: t0..t5, u0..u1, b0.
        ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
            return Fail(error, "cannot create the descriptor heap");
        const UINT step = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto cpu = [&](UINT i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(i) * step;
            return h;
        };

        for (uint32_t s = 0; s < kSrvSlots; ++s)
        {
            const Binding* b = FindBinding(d, false, s);
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ID3D12Resource* target = nullptr;
            if (b == nullptr)
            {
                v.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Texture2D.MipLevels = 1;
            }
            else
            {
                target = live[b->res].texture.Get();
                v.Format = static_cast<DXGI_FORMAT>(b->viewFormat);
                if (b->array)
                {
                    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    v.Texture2DArray = { b->mip, b->mips, b->firstSlice, b->slices, 0, 0.0f };
                }
                else
                {
                    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    v.Texture2D = { b->mip, b->mips, 0, 0.0f };
                }
            }
            device_->CreateShaderResourceView(target, &v, cpu(s));
        }
        for (uint32_t s = 0; s < kUavSlots; ++s)
        {
            const Binding* b = FindBinding(d, true, s);
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            ID3D12Resource* target = nullptr;
            if (b == nullptr)
            {
                v.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            }
            else
            {
                target = live[b->res].texture.Get();
                v.Format = static_cast<DXGI_FORMAT>(b->viewFormat);
                if (b->array)
                {
                    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    v.Texture2DArray = { b->mip, b->firstSlice, b->slices, 0 };
                }
                else
                {
                    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    v.Texture2D = { b->mip, 0 };
                }
            }
            device_->CreateUnorderedAccessView(target, nullptr, &v, cpu(kSrvSlots + s));
        }

        ComPtr<ID3D12Resource> cbuffer = CreateBuffer(device_.Get(), kConstantBytes, D3D12_HEAP_TYPE_UPLOAD);
        if (!cbuffer)
            return Fail(error, "cannot create the constant buffer");
        void* mappedConstants = nullptr;
        cbuffer->Map(0, nullptr, &mappedConstants);
        std::memcpy(mappedConstants, d.constants.data(), kConstantBytes);
        cbuffer->Unmap(0, nullptr);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv {};
        cv.BufferLocation = cbuffer->GetGPUVirtualAddress();
        cv.SizeInBytes = static_cast<UINT>(kConstantBytes);
        device_->CreateConstantBufferView(&cv, cpu(kSrvSlots + kUavSlots));

        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        list_->SetDescriptorHeaps(1, heaps);
        list_->SetComputeRootSignature(root_.Get());
        list_->SetPipelineState(pso);
        list_->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list_->Dispatch(d.groups[0], d.groups[1], d.groups[2]);

        for (auto& [local, l] : live)
        {
            if (!l.uav)
                continue;
            l.readback = CreateBuffer(device_.Get(), l.fp.total, D3D12_HEAP_TYPE_READBACK);
            if (!l.readback)
                return Fail(error, "cannot create a readback buffer");
            Transition(list_.Get(), l.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            CopyOut(list_.Get(), l.texture.Get(), l.readback.Get(), l.fp);
        }
        list_->Close();
        closer.open = false;

        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fenceValue_;
        queue_->Signal(fence_.Get(), value);
        bool timedOut = false;
        if (fence_->GetCompletedValue() < value)
        {
            fence_->SetEventOnCompletion(value, event_);
            timedOut = WaitForSingleObject(event_, kWaitMilliseconds) != WAIT_OBJECT_0;
        }
        if (simulateAt_ != 0 && --simulateAt_ == 0)
        {
            while (fence_->GetCompletedValue() < value)
                Sleep(1);
            timedOut = true;
        }
        if (timedOut || fence_->GetCompletedValue() < value || FAILED(device_->GetDeviceRemovedReason()))
        {
            // The GPU may still own the resources, the heap and the constants of this dispatch, and the command list: keep
            // them all alive for good and never record on this list again.
            abandoned_ = true;
            for (auto& [local, l] : live)
            {
                graveyard_.push_back(l.texture);
                graveyard_.push_back(l.upload);
                graveyard_.push_back(l.readback);
            }
            graveyard_.push_back(heap);
            graveyard_.push_back(cbuffer);
            return Fail(error, timedOut ? "the replay timed out on the GPU; the rest of the set is abandoned"
                                        : "the device was removed during the replay; the rest of the set is abandoned");
        }

        for (const Binding& b : d.bindings)
        {
            if (!b.uav)
                continue;
            Live& l = live[b.res];
            Output o;
            const Resource* r = nullptr;
            for (const Resource& x : d.resources)
                if (x.local == b.res)
                    r = &x;
            o.width = r->width;
            o.height = r->height;
            o.format = l.fp.copyFormat;
            o.rowBytes = static_cast<uint32_t>(l.fp.rowBytes);
            uint8_t* mapped = nullptr;
            D3D12_RANGE range { 0, static_cast<SIZE_T>(l.fp.total) };
            if (FAILED(l.readback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
                return Fail(error, "cannot map a readback buffer");
            o.bytes = PackRows(mapped + l.fp.layout.Offset, l.fp.layout.Footprint.RowPitch, o.rowBytes, l.fp.rows);
            D3D12_RANGE none { 0, 0 };
            l.readback->Unmap(0, &none);
            outputs[b.slot] = std::move(o);
        }
        return true;
    }

  private:
    static bool Fail(std::string& error, const std::string& what)
    {
        error = what;
        return false;
    }

    static const Binding* FindBinding(const Dispatch& d, bool uav, uint32_t slot)
    {
        for (const Binding& b : d.bindings)
            if (b.uav == uav && b.slot == slot)
                return &b;
        return nullptr;
    }

    // Cached by the size and the content of the shader, never by where it happens to be in memory: a buffer that is reused for
    // another shader must not find the old pipeline.
    ID3D12PipelineState* Pipeline(const void* code, size_t size)
    {
        const std::pair<size_t, uint64_t> key { size, Fnv1a64(code, size) };
        const auto it = pipelines_.find(key);
        if (it != pipelines_.end())
            return it->second.Get();
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root_.Get();
        pd.CS = { code, size };
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))))
            return nullptr;
        return pipelines_.emplace(key, pso).first->second.Get();
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> root_;
    HANDLE event_ = nullptr;
    UINT64 fenceValue_ = 0;
    std::map<std::pair<size_t, uint64_t>, ComPtr<ID3D12PipelineState>> pipelines_;
    bool abandoned_ = false;
    uint32_t simulateAt_ = 0;
    std::vector<ComPtr<IUnknown>> graveyard_;
};

// ---- running a whole set against the blessed images -----------------------------------------------------------------

struct Blob
{
    const void* data = nullptr;
    size_t size = 0;
};
using ShaderLookup = std::function<bool(const std::string& shaderId, Blob& blob)>;

struct RunOptions
{
    std::filesystem::path setDir;
    std::filesystem::path outDir;      // difference pictures are written here; emptied at the start of every run
    BlessSpec bless;
    std::string commit;
    bool dirty = false;                // the working tree had uncommitted changes when the blessing was made
    std::string date;
    WarpInfo warp;
    ShaderLookup shaders;
    std::map<std::string, double> tolerance; // kind -> largest allowed component error; kinds not listed are bit exact
};

struct RunReport
{
    int dispatches = 0, blessed = 0, matched = 0, differing = 0, missing = 0, errors = 0;
    bool noBlessedSet = false;
    bool warpChanged = false;
    bool incomplete = false; // the capture itself is missing dispatches
    bool aborted = false;    // the GPU timed out or the device was removed; the rest of the set was not replayed
    int writeFailures = 0;   // blessed images or the provenance that could not be written
    bool ok = false;
    std::vector<std::string> lines;
    // Informational: the largest error between the replay and the output the game's GPU wrote, over every output that has one.
    double maxErrorVsCaptured = 0.0;
    int comparedToCaptured = 0;
};

inline std::string Format(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

// Empties the folder of difference pictures so that a picture is always from this run. Never the set, never anything the set
// is inside of, never the reference or image folders.
inline bool ClearOutputFolder(const std::filesystem::path& out, const std::filesystem::path& set, std::string& error)
{
    std::error_code ec;
    if (out.empty() || out.filename().empty())
    {
        error = "no folder for the difference pictures";
        return false;
    }
    const std::filesystem::path o = std::filesystem::weakly_canonical(out, ec);
    const std::filesystem::path s = std::filesystem::weakly_canonical(set, ec);
    const std::filesystem::path guarded[] = { s / "reference", s / "images" };
    for (std::filesystem::path q = s;; q = q.parent_path())
    {
        if (q == o)
        {
            error = "the folder for the difference pictures (" + out.string() + ") is the set or holds it; it would be emptied";
            return false;
        }
        if (q == q.parent_path())
            break;
    }
    for (std::filesystem::path q = o;; q = q.parent_path())
    {
        for (const auto& g : guarded)
            if (q == g)
            {
                error = "the folder for the difference pictures (" + out.string() + ") is inside the set's own data";
                return false;
            }
        if (q == q.parent_path())
            break;
    }
    std::filesystem::remove_all(o, ec);
    if (ec)
    {
        error = "cannot empty " + out.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// The bless request checked against every set of a run together (not every scene runs every stage, so a kind may be in only
// some of them). Empty when every named kind is in at least one set; otherwise the message to show, and nothing should be
// written.
inline std::string BlessKindProblems(const BlessSpec& spec, const std::vector<std::filesystem::path>& setDirs)
{
    std::vector<Manifest> manifests(setDirs.size());
    std::vector<const Manifest*> pointers;
    for (size_t i = 0; i < setDirs.size(); ++i)
    {
        SetFiles set;
        std::string error;
        if (LoadSet(setDirs[i], set, error))
            manifests[i] = set.manifest;
        pointers.push_back(&manifests[i]);
    }
    const std::vector<std::string> unmatched = UnmatchedBlessKinds(spec, pointers);
    if (unmatched.empty())
        return {};
    std::string names;
    for (const std::string& k : unmatched)
        names += (names.empty() ? "" : ", ") + k;
    return "the bless request names kind(s) that no dispatch of the replayed set(s) has: " + names + ". Nothing was written.";
}

// Whether every set of a bless run may be written: each manifest loads (format), is complete, and, when a blessed set
// already exists, its provenance loads and accepts this adapter (provenance). Writes nothing. The bless loop must not
// start otherwise: one bad set after a good one would leave the earlier sets rewritten and the later ones not.
inline std::string PrecheckSetsForBless(const std::vector<std::filesystem::path>& setDirs, const BlessSpec& spec, const WarpInfo& warp)
{
    if (!warp.Valid())
        return "the software adapter could not be identified (" +
               (warp.error.empty() ? std::string("no version or hash") : warp.error) + "); nothing was replayed or written";
    for (const std::filesystem::path& dir : setDirs)
    {
        SetFiles set;
        std::string error;
        if (!LoadSet(dir, set, error))
            return dir.string() + ": " + error + ". Nothing was written.";
        const std::vector<std::string> problems = CheckComplete(set.manifest);
        if (!problems.empty())
            return dir.string() + ": the capture is incomplete: " + problems.front() + ". Nothing was written.";
        const std::filesystem::path refDir = dir / "reference";
        if (HasBlessedSet(refDir))
        {
            Provenance prov;
            std::vector<uint8_t> text;
            if (!ReadFile(refDir / "provenance.txt", text) || !ParseProvenance(std::string(text.begin(), text.end()), prov, error))
                return dir.string() + ": the blessed set is unreadable: " + error + ". Nothing was written.";
            if (!AdapterAcceptable(prov, warp, spec))
                return dir.string() + ": " + WarpChangedMessage(prov.warp, warp) + ". Nothing was written.";
        }
    }
    return {};
}

inline RunReport RunReplaySet(const RunOptions& opt, Replayer& replayer)
{
    RunReport rep;
    const auto say = [&](const std::string& s) { rep.lines.push_back(s); };

    // An adapter nobody could identify cannot be compared with or blessed on, whatever the request says.
    if (!opt.warp.Valid())
    {
        ++rep.errors;
        say("FAIL: the software adapter could not be identified (" + (opt.warp.error.empty() ? std::string("no version or hash") : opt.warp.error) +
            "); nothing was replayed or written");
        return rep;
    }

    SetFiles set;
    std::string error;
    if (!LoadSet(opt.setDir, set, error))
    {
        say("FAIL: " + error);
        return rep;
    }

    if (!ClearOutputFolder(opt.outDir, opt.setDir, error))
    {
        ++rep.errors;
        say("FAIL: " + error);
        return rep;
    }

    // A capture that left something out does not describe the frame: compare and bless both refuse it.
    const std::vector<std::string> problems = CheckComplete(set.manifest);
    if (!problems.empty())
    {
        rep.incomplete = true;
        for (const std::string& p : problems)
            say("FAIL: the capture is incomplete: " + p);
        return rep;
    }

    const std::filesystem::path refDir = opt.setDir / "reference";
    Provenance prov;
    const bool hasSet = HasBlessedSet(refDir);
    if (hasSet)
    {
        std::vector<uint8_t> text;
        if (!ReadFile(refDir / "provenance.txt", text) || !ParseProvenance(std::string(text.begin(), text.end()), prov, error))
        {
            say("FAIL: the blessed set is unreadable: " + error);
            return rep;
        }
        if (!AdapterAcceptable(prov, opt.warp, opt.bless))
        {
            rep.warpChanged = true;
            say("FAIL: " + WarpChangedMessage(prov.warp, opt.warp));
            return rep;
        }
    }
    else if (!opt.bless.Any())
    {
        rep.noBlessedSet = true;
        say("FAIL: no blessed set in " + refDir.string() + ". Run once with the bless request set to all, on a build known to be good.");
        return rep;
    }

    if (opt.bless.all)
        prov.dispatches.clear(); // a full bless replaces everything the old one said

    for (const Dispatch& d : set.manifest.dispatches)
    {
        ++rep.dispatches;
        const std::string key = DispatchKey(d.frame, d.index);
        const std::string name = "dispatch " + key + " kind=" + d.kind;

        Blob blob;
        if (!opt.shaders || !opt.shaders(d.shader, blob))
        {
            ++rep.errors;
            say("FAIL: " + name + ": no shader blob for \"" + d.shader + "\"");
            continue;
        }
        const uint64_t blobHash = Fnv1a64(blob.data, blob.size);

        std::map<uint32_t, Output> outputs;
        if (!replayer.Run(set, d, blob.data, blob.size, outputs, error))
        {
            ++rep.errors;
            say("FAIL: " + name + ": " + error);
            if (replayer.Abandoned())
            {
                rep.aborted = true;
                say("FAIL: the replay stopped at " + name + "; the dispatches after it were not replayed");
                break;
            }
            continue;
        }

        bool dispatchDiffers = false, dispatchMissing = false, dispatchWriteFailed = false;
        std::string shaderNote;
        if (hasSet)
        {
            const auto e = prov.dispatches.find(key);
            if (e != prov.dispatches.end())
                shaderNote = e->second.shaderHash == blobHash ? "shader blob is the blessed one" : "shader blob differs from the blessed one";
        }

        for (const auto& [slot, out] : outputs)
        {
            const std::string refFile = ReferenceFileName(d.frame, d.index, slot);

            // Informational comparison with what the game's GPU wrote.
            for (const Binding& b : d.bindings)
            {
                if (!b.uav || b.slot != slot)
                    continue;
                for (const Resource& r : d.resources)
                {
                    std::vector<uint8_t> gpu;
                    std::string why;
                    if (r.local == b.res && r.after != kNoImage && ReadImage(set, r.after, gpu, why))
                    {
                        const Diff g = CompareImages(gpu, out.bytes, out.width, out.height, out.rowBytes, out.format);
                        if (g.sameShape && g.psnrValid)
                        {
                            ++rep.comparedToCaptured;
                            rep.maxErrorVsCaptured = std::max(rep.maxErrorVsCaptured, g.maxAbs);
                        }
                    }
                }
            }

            if (BlessesNow(opt.bless, d.kind))
            {
                if (!WriteFile(refDir / refFile, out.bytes.data(), out.bytes.size()))
                {
                    dispatchWriteFailed = true;
                    ++rep.writeFailures;
                    ++rep.errors;
                    say("FAIL: " + name + " u" + std::to_string(slot) + ": could not write the blessed image " + (refDir / refFile).string());
                }
                continue;
            }

            std::vector<uint8_t> ref;
            if (!ReadFile(refDir / refFile, ref))
            {
                dispatchMissing = true;
                say("FAIL: " + name + " u" + std::to_string(slot) + ": no blessed image (" + refFile + ")");
                continue;
            }
            const Diff diff = CompareImages(ref, out.bytes, out.width, out.height, out.rowBytes, out.format);
            if (!diff.sameShape)
            {
                dispatchDiffers = true;
                say("FAIL: " + name + " u" + std::to_string(slot) + ": the blessed image has another size than the replay");
                continue;
            }
            const auto tol = opt.tolerance.find(d.kind);
            const bool within = tol != opt.tolerance.end() && diff.psnrValid && diff.maxAbs <= tol->second;
            if (diff.identical || within)
                continue;

            dispatchDiffers = true;
            const std::string picture = "diff_f" + std::to_string(d.frame) + "_d" + std::to_string(d.index) + "_u" + std::to_string(slot) + ".ppm";
            WriteDiffPpm(opt.outDir / picture, ref, out.bytes, out.width, out.height, out.rowBytes, out.format, diff);
            say("FAIL: " + name + " u" + std::to_string(slot) + ": differs from the blessed image; first differing pixel (" +
                std::to_string(diff.firstX) + ", " + std::to_string(diff.firstY) + "), " + std::to_string(diff.differingPixels) +
                " pixels differ, " + (diff.psnrValid ? Format("PSNR %.2f dB, max error %.6g", diff.psnr, diff.maxAbs) : std::string("PSNR n/a")) +
                "; " + (shaderNote.empty() ? "no blessed shader hash" : shaderNote) + "; picture " + picture);
        }

        if (BlessesNow(opt.bless, d.kind))
        {
            // The record is the one of this blessing, kept per dispatch, and only for images that were written.
            if (!dispatchWriteFailed)
            {
                prov.dispatches[key] = { d.kind, blobHash, opt.commit, opt.date, opt.dirty, opt.warp };
                ++rep.blessed;
            }
        }
        else if (dispatchMissing)
            ++rep.missing;
        else if (dispatchDiffers)
            ++rep.differing;
        else
            ++rep.matched;
    }

    bool provenanceWritten = true;
    if (opt.bless.Any() && rep.blessed > 0)
    {
        prov.commit = opt.commit;
        prov.date = opt.date;
        prov.dirty = opt.dirty;
        prov.warp = opt.warp;
        provenanceWritten = WriteText(refDir / "provenance.txt", WriteProvenance(prov));
        if (!provenanceWritten)
        {
            ++rep.errors;
            ++rep.writeFailures;
            say("FAIL: could not write " + (refDir / "provenance.txt").string() + "; the blessed images are not recorded");
        }
    }

    rep.ok = rep.errors == 0 && rep.differing == 0 && rep.missing == 0 && !rep.aborted;
    if (rep.blessed > 0 && rep.writeFailures == 0)
        say(BlessBanner(opt.bless, opt.commit, opt.dirty));
    say(Format("%d dispatches: %d blessed, %d identical, %d differ, %d without a blessed image, %d could not run", rep.dispatches,
               rep.blessed, rep.matched, rep.differing, rep.missing, rep.errors));
    return rep;
}
} // namespace ext::nr::replay
