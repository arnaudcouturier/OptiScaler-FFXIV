#include "pch.h"

#include "ext/ffxiv/engine/DofCamera.h"

#include <cstring>

// Depth of field, Route B: the frame's captures (DofCamera.h). The march's own pattern, this stage's own needs: the
// camera the light draws bind (the projection +0x120 and the inverse projection +0x0E0 the CoC's metres reconstruct
// through), the fog post's 32-byte pixel-to-NDC row (the pack's reconstruction and the focus centre's read walk it) and
// the scene depth kept as a reference. Every read is a map the GPU has already finished or is not given at all - the
// no-wait rule in the flags argument - and nothing of the game is ever written.
namespace ext::ffxiv::dof
{
namespace
{
// A byte range of a constant buffer as the box a partial copy takes (the march's own helper, this module's own copy: the
// march's helper is file-local to GodraysSun.cpp and the modules share no link in the tests).
D3D11_BOX ByteBox(uint32_t offset, uint32_t bytes)
{
    D3D11_BOX box {};
    box.left = offset;
    box.top = 0;
    box.front = 0;
    box.right = offset + bytes;
    box.bottom = 1;
    box.back = 1;
    return box;
}

// One staging buffer's bytes, or false when the GPU has not finished the copy that fills it. The map never waits, so a
// frame whose copy is still in the queue keeps the values it already had (the march's own MapRead, the no-wait flag in
// the flags argument).
bool MapRead(ID3D11DeviceContext* context, ID3D11Buffer* staging, void* bytes, uint32_t count, bool block)
{
    if (context == nullptr || staging == nullptr || bytes == nullptr)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    const UINT flags = block ? 0u : UINT(D3D11_MAP_FLAG_DO_NOT_WAIT);
    const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, flags, &mapped);
    if (hr != S_OK || mapped.pData == nullptr)
        return false;
    std::memcpy(bytes, mapped.pData, count);
    context->Unmap(staging, 0);
    return true;
}

bool EnsureStaging(ID3D11Device* device, uint32_t bytes, Microsoft::WRL::ComPtr<ID3D11Buffer>& target)
{
    if (target)
        return true;
    if (device == nullptr)
        return false;
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Buffer> created;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &created)) || !created)
        return false;
    target = created;
    return true;
}

// Whether a staging buffer this ring made was created on `device` (the march's own rule: a pair of another device cannot
// receive this context's copy, and CopySubresourceRegion refuses it silently).
bool MadeOn(ID3D11Buffer* buffer, ID3D11Device* device)
{
    if (buffer == nullptr || device == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> owner;
    buffer->GetDevice(&owner);
    return owner && owner.Get() == device;
}

constexpr uint32_t kCameraWindowBytes = shafts::kCameraBytes; // the camera window the march's own decode names: 0x160
constexpr uint32_t kInverseProjectionAt = 0x0E0;             // g_CameraParameter +0x0E0 (the march's own decode)
constexpr uint32_t kViewportParamBytes = 0x20;               // cDynamicViewportResolutionParam: the march's own size

inline float FloatAt(const unsigned char* bytes, uint32_t offset)
{
    float value = 0.0f;
    std::memcpy(&value, bytes + offset, sizeof value);
    return value;
}
} // namespace

bool CameraReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* camera, uint64_t frame)
{
    if (context == nullptr || camera == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    // One copy per ring slot per frame: the caller (the draw path) takes the first light draw of the frame only, so the
    // ring slot carries the one camera buffer the frame's light pass bound, whatever the draw count behind it.
    Slot& slot = _ring[shafts::RingIndex(frame)];
    if (!MadeOn(slot.camera.Get(), device.Get()))
    {
        slot = Slot {};
        if (!EnsureStaging(device.Get(), kCameraWindowBytes, slot.camera))
            return false;
    }
    const D3D11_BOX box = ByteBox(0, kCameraWindowBytes);
    context->CopySubresourceRegion(slot.camera.Get(), 0, 0, 0, 0, camera, 0, &box);
    slot.source = camera;
    slot.frame = frame;
    slot.written = true;
    return true;
}

CameraRead CameraReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    CameraRead result;
    if (context == nullptr)
        return result;
    // Newest first: the copy the frame before this one made is the one that belongs to this frame's picture, and an
    // older one is taken only when it has not landed - the ring's age rule, so a GPU a frame or two behind still answers.
    bool attempted = false;
    unsigned char camera[kCameraWindowBytes] {};
    for (uint32_t age = shafts::kNewestReadAge; age < shafts::kStagingRing; ++age)
    {
        if (frame < age)
            break;
        CameraReader::Slot& slot = _ring[shafts::SlotForAge(frame, age)];
        if (!shafts::ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        if (!MapRead(context, slot.camera.Get(), camera, kCameraWindowBytes, block))
        {
            attempted = true;
            continue;
        }
        std::memcpy(result.projection, camera + shafts::kProjectionOffset, sizeof result.projection);
        std::memcpy(result.inverseProjection, camera + kInverseProjectionAt, sizeof result.inverseProjection);
        result.landed = true;
        result.writtenFrame = slot.frame;
        _haveRead = true;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        _lastReadFrame = frame;
        break;
    }
    if (!attempted && !result.landed)
        return result;
    if (!result.landed)
    {
        result.notReady = attempted;
        ++_notReady;
        return result;
    }
    // The camera's own guard is the contact pass's: the projection's shape is the buffer's, and the band derives its
    // planes from the z rows both matrices carry. A buffer whose rows do not answer the planes a real camera has is a
    // build mismatch, and the caller keeps the last accepted camera for the frames it held (the march's hold, one unit
    // up) and counts the refusal toward the stand-down.
    result.band = CameraDepthBand(result.inverseProjection, result.projection);
    result.usable = result.band.usable;
    if (result.usable)
        _haveUsable = true;
    return result;
}

void CameraReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveUsable = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
    _reads = 0;
    _notReady = 0;
}

bool RowReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* row, uint64_t frame)
{
    if (context == nullptr || row == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[shafts::RingIndex(frame)];
    if (!MadeOn(slot.row.Get(), device.Get()))
    {
        slot = Slot {};
        if (!EnsureStaging(device.Get(), kViewportParamBytes, slot.row))
            return false;
    }
    const D3D11_BOX box = ByteBox(0, kViewportParamBytes);
    context->CopySubresourceRegion(slot.row.Get(), 0, 0, 0, 0, row, 0, &box);
    slot.frame = frame;
    slot.written = true;
    return true;
}

RowRead RowReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    RowRead result;
    if (context == nullptr)
        return result;
    unsigned char row[kViewportParamBytes] {};
    for (uint32_t age = shafts::kNewestReadAge; age < shafts::kStagingRing; ++age)
    {
        if (frame < age)
            break;
        Slot& slot = _ring[shafts::SlotForAge(frame, age)];
        if (!shafts::ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        if (!MapRead(context, slot.row.Get(), row, kViewportParamBytes, block))
            continue;
        // The buffer's second float4 as the module first assumed it (a scale in .xy and an offset in .zw). The fog post's bytecode never reads
        // it, and the one shader that does reads it as a positive uv scale, so the stage no longer maps pixels to NDC through it; the bytes are
        // kept for the log line that settles what they hold
        // (the march's own decode of the same 32-byte buffer).
        result.ndcScale[0] = FloatAt(row, 0x10);
        result.ndcScale[1] = FloatAt(row, 0x14);
        result.ndcOffset[0] = FloatAt(row, 0x18);
        result.ndcOffset[1] = FloatAt(row, 0x1C);
        result.landed = true;
        result.usable = result.ndcScale[0] == result.ndcScale[0] && result.ndcScale[0] > 0.0f &&
                        result.ndcScale[1] < 0.0f; // the engine's own flip: y's scale is negative (y down the texture)
        result.writtenFrame = slot.frame;
        _haveRead = true;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        _lastReadFrame = frame;
        break;
    }
    return result;
}

void RowReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
}

void DepthKeep::Keep(ID3D11ShaderResourceView* source, uint32_t format, uint32_t width, uint32_t height)
{
    _view = source;
    _format = format;
    _width = width;
    _height = height;
}

void DepthKeep::Reset()
{
    _view.Reset();
    _format = _width = _height = 0;
}
} // namespace ext::ffxiv::dof
