#include "pch.h"

#include "ext/ffxiv/engine/CbPatch.h"

#include <CbScale_CS.h> // generated from ext/ffxiv/engine/shaders/CbScale.hlsl by engine.shaders.props

#include <cstring>

// Engine pass overrides: the constant-buffer patch primitive (ext/ffxiv/engine/CbPatch.h). Everything here works on the game's own context,
// reads the game's buffers only through GPU-side copies and puts back every binding it takes, on every path.
namespace ext::ffxiv::engine
{
using Microsoft::WRL::ComPtr;

namespace
{
// The register slots and the parameter layout of ext/ffxiv/engine/shaders/CbScale.hlsl. Change them together.
constexpr uint32_t kSlotSource = 0;  // b0: the copy of the game's buffer, bound as this shader's own constant buffer
constexpr uint32_t kSlotParams = 1;  // b1: the scale table
constexpr uint32_t kSlotOutput = 0;  // u0: the raw buffer the scaled float4s land in
static_assert(kSlotSource + 1 == kSlotParams, "the two constant buffers of the scale shader are adjacent");

// The largest buffer the compute shader can read as a constant buffer: 1,024 float4s, the ambient table's bound size.
constexpr uint32_t kMaxBytes = 16384;
constexpr uint32_t kThreads = 64; // CSMain's [numthreads] x

// The parameter block of CbScale.hlsl, byte for byte: a uint4 of info, then the per-entry multipliers, addresses and set values.
struct ParamsBuffer
{
    uint32_t info[4];           // x = scale entry count, y = byte count to write, z = set entry count, w = 0
    float multiplier[8][4];     // one multiplier per scale entry
    uint32_t address[8][4];     // one address per scale entry: x = first byte, y = stride, z = count
    uint32_t setValue[8][4];    // one value per set entry: the bits a masked component is replaced with
    uint32_t setMask[8][4];     // one mask per set entry: x = the component mask (bit 0..3 = .x,.y,.z,.w), yzw = 0
    uint32_t setAddress[8][4];  // one address per set entry: x = first byte, y = stride, z = count
};
static_assert(sizeof(ParamsBuffer) == 16 + 5 * 8 * 16, "the parameter block the shader declares is 656 bytes");
static_assert(sizeof(ParamsBuffer) % 16 == 0, "a constant buffer is a multiple of 16 bytes");

// The state the dispatch changes, put back when the guard goes out of scope: the compute shader (with its class instances), its two constant
// buffer slots and the one unordered-access slot.
class ComputeStateRestore
{
  public:
    ComputeStateRestore(ID3D11DeviceContext* context, uint32_t constantBuffers, uint32_t unorderedViews) : _context(context)
    {
        _context->CSGetConstantBuffers(0, constantBuffers, _buffers);
        _context->CSGetUnorderedAccessViews(0, unorderedViews, _views);
        // The class-instance count comes back in `_count`; the array is an input capacity, so a stage with more instances than this keeps
        // the first ones rather than writing past the array.
        _count = kMaxInstances;
        _context->CSGetShader(&_shader, _instances, &_count);
        if (_count > kMaxInstances)
            _count = kMaxInstances;
    }

    ~ComputeStateRestore()
    {
        if (_shader)
            _context->CSSetShader(_shader.Get(), _count > 0 ? _instances : nullptr, _count);
        else
            _context->CSSetShader(nullptr, nullptr, 0);
        _context->CSSetConstantBuffers(0, kMaxBuffers, _buffers);
        _context->CSSetUnorderedAccessViews(0, kMaxViews, _views, nullptr);
        for (uint32_t i = 0; i < _count; ++i)
            if (_instances[i] != nullptr)
                _instances[i]->Release();
        for (ID3D11Buffer* buffer : _buffers)
            if (buffer != nullptr)
                buffer->Release();
        for (ID3D11UnorderedAccessView* view : _views)
            if (view != nullptr)
                view->Release();
    }

    ComputeStateRestore(const ComputeStateRestore&) = delete;
    ComputeStateRestore& operator=(const ComputeStateRestore&) = delete;

  private:
    static constexpr uint32_t kMaxBuffers = 2;
    static constexpr uint32_t kMaxViews = 1;
    static constexpr uint32_t kMaxInstances = 64;
    ID3D11DeviceContext* _context = nullptr;
    ComPtr<ID3D11ComputeShader> _shader;
    ID3D11ClassInstance* _instances[kMaxInstances] {};
    uint32_t _count = 0;
    ID3D11Buffer* _buffers[kMaxBuffers] { nullptr, nullptr };
    ID3D11UnorderedAccessView* _views[kMaxViews] { nullptr };
};} // namespace

BoundConstantBuffer::BoundConstantBuffer(ID3D11DeviceContext* context, uint32_t slot, ID3D11Buffer* replacement)
    : _context(context), _slot(slot)
{
    if (_context == nullptr || replacement == nullptr)
        return;
    _context->PSGetConstantBuffers(_slot, 1, &_original);
    _context->PSSetConstantBuffers(_slot, 1, &replacement);
    _bound = true;
}

BoundConstantBuffer::~BoundConstantBuffer()
{
    if (!_bound)
        return;
    _context->PSSetConstantBuffers(_slot, 1, &_original);
    if (_original != nullptr)
        _original->Release();
}

void CbPatch::Release()
{
    std::lock_guard lock(_mutex);
    ReleaseLocked();
}

void CbPatch::ReleaseLocked()
{
    // A caller that holds a patched buffer pointer learns here that it is no longer this instance's (Generation).
    _generation.fetch_add(1, std::memory_order_relaxed);
    _sizes.clear();
    _slots.clear();
    _params.Reset();
    _shader.Reset();
    _end.Reset();
    _begin.Reset();
    _disjoint.Reset();
    _device.Reset();
    _timingPending = false;
    _haveGpuMs = false;
    _gpuMs = 0.0;
}

bool CbPatch::Init(ID3D11Device* device)
{
    if (device == nullptr)
    {
        _failure = "the draw has no device";
        return false;
    }
    if (_device.Get() == device && _shader && _params)
        return true;
    ReleaseLocked();
    _device = device;

    if (FAILED(device->CreateComputeShader(CbScale_CS_cso, sizeof CbScale_CS_cso, nullptr, &_shader)) || !_shader)
    {
        _failure = "the scale shader could not be created";
        return false;
    }
    D3D11_BUFFER_DESC params {};
    params.ByteWidth = sizeof(ParamsBuffer);
    params.Usage = D3D11_USAGE_DYNAMIC;
    params.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    params.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&params, nullptr, &_params)) || !_params)
    {
        _failure = "the scale parameter buffer could not be created";
        return false;
    }
    // The timestamp pair of the GPU time report. A device that refuses the queries is not a failure: the patch runs untimed and the run line
    // says so.
    D3D11_QUERY_DESC disjointDesc { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC timestampDesc { D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(device->CreateQuery(&disjointDesc, &_disjoint)))
        _disjoint.Reset();
    if (FAILED(device->CreateQuery(&timestampDesc, &_begin)))
        _begin.Reset();
    if (FAILED(device->CreateQuery(&timestampDesc, &_end)))
        _end.Reset();
    return true;
}

CbPatch::SizeCache* CbPatch::EnsureSize(ID3D11Device* device, uint32_t byteWidth)
{
    for (SizeCache& cache : _sizes)
        if (cache.byteWidth == byteWidth && cache.source && cache.work && cache.destination)
            return &cache;

    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = byteWidth;
    desc.Usage = D3D11_USAGE_DEFAULT;

    SizeCache cache;
    cache.byteWidth = byteWidth;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &cache.source)) || !cache.source)
    {
        _failure = "the copy of the game's buffer could not be created";
        return nullptr;
    }
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &cache.work)) || !cache.work)
    {
        _failure = "the scale work buffer could not be created";
        return nullptr;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC view {};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.FirstElement = 0;
    view.Buffer.NumElements = byteWidth / 4;
    view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(device->CreateUnorderedAccessView(cache.work.Get(), &view, &cache.workView)) || !cache.workView)
    {
        _failure = "the scale work buffer's raw view could not be created";
        return nullptr;
    }
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.MiscFlags = 0;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &cache.destination)) || !cache.destination)
    {
        _failure = "the patched buffer the draw reads could not be created";
        return nullptr;
    }
    if (_sizes.size() >= 4)
    {
        // The evicted entry's buffers go with it, including one a caller may still hold: the generation says so.
        _sizes.erase(_sizes.begin()); // the engine binds two sizes; a third is a changed build, not a growth path
        _generation.fetch_add(1, std::memory_order_relaxed);
    }
    _sizes.push_back(std::move(cache));
    return &_sizes.back();
}

bool CbPatch::UploadParams(ID3D11DeviceContext* context, uint32_t byteWidth, const ScaleEntry* scaleEntries, uint32_t scaleCount,
                           const SetEntry* setEntries, uint32_t setCount)
{
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the patch parameters could not be written";
        return false;
    }
    ParamsBuffer block {};
    block.info[0] = scaleCount;
    block.info[1] = byteWidth;
    block.info[2] = setCount;
    for (uint32_t i = 0; i < scaleCount && i < kMaxScaleEntries; ++i)
    {
        block.address[i][0] = scaleEntries[i].byteOffset;
        block.address[i][1] = scaleEntries[i].stride;
        block.address[i][2] = scaleEntries[i].count;
        for (uint32_t c = 0; c < 4; ++c)
            block.multiplier[i][c] = scaleEntries[i].multiplier[c];
    }
    for (uint32_t i = 0; i < setCount && i < kMaxSetEntries; ++i)
    {
        block.setAddress[i][0] = setEntries[i].byteOffset;
        block.setAddress[i][1] = setEntries[i].stride;
        block.setAddress[i][2] = setEntries[i].count;
        block.setMask[i][0] = setEntries[i].mask;
        for (uint32_t c = 0; c < 4; ++c)
            block.setValue[i][c] = setEntries[i].value[c];
    }
    std::memcpy(mapped.pData, &block, sizeof block);
    context->Unmap(_params.Get(), 0);
    return true;
}

bool CbPatch::BeginGpuTiming(ID3D11DeviceContext* context)
{
    if (!_disjoint || !_begin || !_end || _timingPending)
        return false; // the previous pair is still in the queue: this run is not timed and must not end that pair either
    context->Begin(_disjoint.Get());
    context->End(_begin.Get());
    _timingPending = true;
    return true;
}

void CbPatch::EndGpuTiming(ID3D11DeviceContext* context)
{
    if (!_timingPending)
        return;
    // The disjoint query ends with the pair: without its End the runtime never answers GetData, and no measurement would ever land.
    if (_end)
        context->End(_end.Get());
    if (_disjoint)
        context->End(_disjoint.Get());
}

ID3D11Buffer* CbPatch::Scale(ID3D11DeviceContext* context, ID3D11Buffer* game, uint32_t byteWidth, const ScaleEntry* entries, uint32_t count)
{
    return Patch(context, game, byteWidth, entries, count, nullptr, 0);
}

ID3D11Buffer* CbPatch::Set(ID3D11DeviceContext* context, ID3D11Buffer* game, uint32_t byteWidth, const SetEntry* entries, uint32_t count)
{
    return Patch(context, game, byteWidth, nullptr, 0, entries, count);
}

ID3D11Buffer* CbPatch::Patch(ID3D11DeviceContext* context, ID3D11Buffer* game, uint32_t byteWidth, const ScaleEntry* scaleEntries,
                             uint32_t scaleCount, const SetEntry* setEntries, uint32_t setCount)
{
    std::lock_guard lock(_mutex);
    _failure = nullptr;
    if (context == nullptr || game == nullptr)
    {
        _failure = "the patch request is empty";
        return nullptr;
    }
    if ((scaleCount == 0 && setCount == 0) || scaleCount > kMaxScaleEntries || setCount > kMaxSetEntries ||
        (scaleCount > 0 && scaleEntries == nullptr) || (setCount > 0 && setEntries == nullptr))
    {
        _failure = "the patch request is empty";
        return nullptr;
    }
    if (byteWidth < 16 || byteWidth > kMaxBytes || byteWidth % 16 != 0)
    {
        _failure = "the buffer size is not a constant buffer's";
        return nullptr;
    }
    for (uint32_t i = 0; i < scaleCount; ++i)
        if (!EntryFits(scaleEntries[i], byteWidth))
        {
            _failure = "a scale entry does not fit the buffer";
            return nullptr;
        }
    for (uint32_t i = 0; i < setCount; ++i)
        if (!SetEntryFits(setEntries[i], byteWidth))
        {
            _failure = "a set entry does not fit the buffer";
            return nullptr;
        }

    D3D11_BUFFER_DESC gameDesc {};
    game->GetDesc(&gameDesc);
    if (gameDesc.ByteWidth != byteWidth || (gameDesc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0)
    {
        _failure = "the draw's buffer is not the constant buffer the index recorded";
        return nullptr;
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!Init(device.Get()))
        return nullptr;
    SizeCache* cache = EnsureSize(_device.Get(), byteWidth);
    if (cache == nullptr)
        return nullptr;
    if (!UploadParams(context, byteWidth, scaleEntries, scaleCount, setEntries, setCount))
        return nullptr;

    // The pair of the previous run is collected here, never waited for: a queue that is still busy just means one measurement is missed.
    // D3D11_ASYNC_GETDATA_DONOTFLUSH on every read: a GetData without it can flush the game's own command queue to resolve the query, and
    // this runs on the immediate context inside the frame - the periodic hitch a session reads as a lag.
    constexpr UINT kGetDataFlags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    if (_timingPending)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
        if (context->GetData(_disjoint.Get(), &disjoint, sizeof disjoint, kGetDataFlags) == S_OK)
        {
            UINT64 begin = 0, end = 0;
            const HRESULT haveBegin = _begin ? context->GetData(_begin.Get(), &begin, sizeof begin, kGetDataFlags) : S_FALSE;
            const HRESULT haveEnd = _end ? context->GetData(_end.Get(), &end, sizeof end, kGetDataFlags) : S_FALSE;
            _timingPending = false;
            if (!disjoint.Disjoint && disjoint.Frequency > 0 && haveBegin == S_OK && haveEnd == S_OK && end > begin)
            {
                _gpuMs = double(end - begin) * 1000.0 / double(disjoint.Frequency);
                _haveGpuMs = true;
            }
        }
    }

    // The pair brackets both copies and the dispatch, which is what the run line calls the patch's GPU time.
    const bool timed = BeginGpuTiming(context);
    context->CopyResource(cache->source.Get(), game);
    ID3D11Buffer* sources[2] { cache->source.Get(), _params.Get() };
    ID3D11UnorderedAccessView* views[1] { cache->workView.Get() };
    {
        ComputeStateRestore restore(context, 2, 1);
        context->CSSetShader(_shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(kSlotSource, 2, sources);
        context->CSSetUnorderedAccessViews(kSlotOutput, 1, views, nullptr);
        context->Dispatch((byteWidth / 16 + kThreads - 1) / kThreads, 1, 1);
    } // the game's compute state is back before the result is copied, so nothing of this module outlives the call
    context->CopyResource(cache->destination.Get(), cache->work.Get());
    if (timed)
        EndGpuTiming(context);
    if (scaleCount > 0)
        ++_scales;
    if (setCount > 0)
        ++_sets;
    return cache->destination.Get();
}

ID3D11Buffer* CbPatch::Write(ID3D11DeviceContext* context, const CpuWriteRequest& request)
{
    std::lock_guard lock(_mutex);
    _failure = nullptr;
    if (request.equal)
        return nullptr; // the caller's target is what the engine already has: nothing to swap, nothing to write
    if (context == nullptr || request.name == nullptr || request.bytes == nullptr || request.byteWidth == 0 ||
        request.byteWidth % 16 != 0 || request.byteWidth > kMaxBytes)
    {
        _failure = "the write request is empty";
        return nullptr;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!Init(device.Get()))
        return nullptr;

    CpuSlot* slot = nullptr;
    for (CpuSlot& candidate : _slots)
        if (candidate.byteWidth == request.byteWidth && candidate.name == request.name)
        {
            slot = &candidate;
            break;
        }
    if (slot == nullptr)
    {
        CpuSlot fresh;
        fresh.name = request.name;
        fresh.byteWidth = request.byteWidth;
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = request.byteWidth;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(_device->CreateBuffer(&desc, nullptr, &fresh.buffer)) || !fresh.buffer)
        {
            _failure = "the written buffer could not be created";
            return nullptr;
        }
        _slots.push_back(std::move(fresh));
        slot = &_slots.back();
    }
    if (slot->written && slot->frame == request.frame)
        return slot->buffer.Get(); // this frame already wrote its values: one write per frame, however many draws read them

    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(slot->buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || mapped.pData == nullptr)
    {
        _failure = "the written buffer could not be mapped";
        return nullptr;
    }
    std::memcpy(mapped.pData, request.bytes, request.byteWidth);
    context->Unmap(slot->buffer.Get(), 0);
    slot->frame = request.frame;
    slot->written = true;
    ++_writes;
    return slot->buffer.Get();
}
} // namespace ext::ffxiv::engine
