// Light shafts before the tone map: the frame's light and camera bytes, the sky's own sun parameters and the tone map's exposure
// (ext/ffxiv/engine/ShaftsSun.h).
//
// The copies at a draw - the light's parameters, the camera's view, view-projection and projection, the sky's sun parameter row and the tone map's
// exposure multiplier -, one map of each at a later frame, and never a wait: the rings are what makes the delay a delay instead of a stall, and the
// age rule is what makes a read belong to the draw it came from rather than to whichever one happened to be last.
#include "pch.h"

#include "ext/ffxiv/engine/ShaftsSun.h"

#include <cstring>

namespace ext::ffxiv::shafts
{
namespace
{
// A byte range of a constant buffer as the box a partial copy takes: for a buffer the box is in bytes.
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

// One staging buffer's bytes, or false when the GPU has not finished the copy that fills it. The map never waits, so a frame whose copy is
// still in the queue keeps the values it already had.
bool MapRead(ID3D11DeviceContext* context, ID3D11Buffer* staging, void* bytes, uint32_t count, bool block)
{
    if (context == nullptr || staging == nullptr || bytes == nullptr)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    // The product's read never waits: a map that would block is a map of a copy the GPU has not finished, and the frame keeps the values it had.
    // The blocking form exists for the tests only (see ShaftsSun.h); Map takes the flag on the context itself, so no ID3D11DeviceContext1 is
    // needed for the no-wait form.
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

// Whether a staging buffer this ring made was created on `device`. A pair made on the previous device cannot receive this context's copy:
// CopySubresourceRegion returns void and refuses it silently, every frame, and the read then maps bytes the old device wrote - so a pair that
// answers no is rebuilt instead of reused.
bool MadeOn(ID3D11Buffer* buffer, ID3D11Device* device)
{
    if (buffer == nullptr || device == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> owner;
    buffer->GetDevice(&owner); // ID3D11DeviceChild::GetDevice answers with the reference alone, no HRESULT
    return owner && owner.Get() == device;
}
} // namespace

bool SunReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* light, ID3D11Buffer* camera, uint64_t frame, ID3D11Buffer* const* shadows,
                     uint32_t shadowCount)
{
    if (context == nullptr || light == nullptr || camera == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[RingIndex(frame)];
    if (!MadeOn(slot.light.Get(), device.Get()) || !MadeOn(slot.camera.Get(), device.Get()))
    {
        slot = Slot {}; // a pair (or half of one) of another device: this frame's copy makes a new one, ages included
        if (!EnsureStaging(device.Get(), kLightBytes, slot.light) || !EnsureStaging(device.Get(), kCameraBytes, slot.camera))
            return false;
    }
    const D3D11_BOX lightBox = ByteBox(0, kLightBytes);
    const D3D11_BOX cameraBox = ByteBox(0, kCameraBytes); // the view, the view-projection and the projection: one copy of the camera
    context->CopySubresourceRegion(slot.light.Get(), 0, 0, 0, 0, light, 0, &lightBox);
    context->CopySubresourceRegion(slot.camera.Get(), 0, 0, 0, 0, camera, 0, &cameraBox);
    // The light draw's 128-byte parameter buffers: the frame's directional shadow among them, and possibly a neighbour's. They are copied
    // whole, so the rules read the shadow matrix's third row out of the same frame as the light's direction.
    const uint32_t candidates = shadowCount < uint32_t(kShadowCandidates) ? shadowCount : uint32_t(kShadowCandidates);
    if (slot.shadowCount != candidates)
    {
        for (Microsoft::WRL::ComPtr<ID3D11Buffer>& target : slot.shadow)
            target.Reset();
        slot.shadowCount = candidates;
    }
    if (candidates != 0)
    {
        const D3D11_BOX shadowBox = ByteBox(0, kShadowBytes);
        for (uint32_t i = 0; i < candidates; ++i)
        {
            if (shadows == nullptr || shadows[i] == nullptr)
                continue;
            if (!slot.shadow[i] || !MadeOn(slot.shadow[i].Get(), device.Get()))
            {
                slot.shadow[i].Reset();
                if (!EnsureStaging(device.Get(), kShadowBytes, slot.shadow[i]))
                    continue;
            }
            context->CopySubresourceRegion(slot.shadow[i].Get(), 0, 0, 0, 0, shadows[i], 0, &shadowBox);
        }
    }
    slot.source = light;
    slot.frame = frame;
    slot.written = true;
    return true;
}

SunRead SunReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    SunRead result;
    if (context == nullptr)
        return result;

    // Newest first: the copy the frame before this one made is the one that belongs to this frame's picture, and an older one is taken only
    // when it has not landed (a GPU two or three frames behind) - which is what keeps a GPU-bound session reading at all.
    bool attempted = false;
    bool cameraLanded = false;
    unsigned char light[kLightBytes] {};
    unsigned char camera[kCameraBytes] {};
    for (uint32_t age = kNewestReadAge; age < kStagingRing; ++age)
    {
        if (frame < age)
            break; // the ring has not seen that many frames yet
        Slot& slot = _ring[SlotForAge(frame, age)];
        if (!ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        if (!MapRead(context, slot.light.Get(), light, kLightBytes, block))
        {
            attempted = true; // this copy is still in the GPU's queue: try the next older one
            continue;
        }
        std::memcpy(_facts.position, light + 0x00, sizeof _facts.position);
        std::memcpy(_facts.direction, light + kLightDirectionAt, sizeof _facts.direction);
        std::memcpy(_facts.diffuse, light + kLightDiffuseAt, sizeof _facts.diffuse);
        _haveFacts = true;
        _haveRead = true;
        _lastLight = slot.source;
        _lastReadFrame = frame;
        _lastAcceptedFrame = slot.frame;
        _haveAccepted = true;
        ++_reads;
        result.light = true;
        // The directional shadow's rows from the same copy: the frame's own shadow matrix is one of the light draw's 128-byte parameter
        // buffers, and the rules pick the row that is this light's axis (ShaftsRules.h JudgeShadow). A candidate the GPU has not finished is
        // simply not among the rows - the light's direction alone then decides, and the caller says so once.
        _shadowCount = 0;
        for (uint32_t i = 0; i < slot.shadowCount; ++i)
        {
            unsigned char shadow[kShadowBytes] {};
            if (!slot.shadow[i] || !MapRead(context, slot.shadow[i].Get(), shadow, kShadowBytes, block))
                continue;
            // The shape rule reads the candidate as floats: the mapped bytes are copied into a float array first, so a byte array is never
            // reinterpreted at an alignment the buffer does not promise.
            float asFloats[kShadowFloats] {};
            std::memcpy(asFloats, shadow, sizeof asFloats);
            ShadowCandidate& candidate = _shadowCandidates[_shadowCount];
            candidate.shaped = ShadowMatrixShape(asFloats);
            std::memcpy(candidate.row, shadow + kShadowRowAt, sizeof candidate.row);
            ++_shadowCount;
        }
        // The camera's bytes come from the same slot, so they belong to the same frame; a camera this frame did not get is not a refusal, and
        // the camera of the last good read stays until a newer one lands.
        cameraLanded = MapRead(context, slot.camera.Get(), camera, kCameraBytes, block);
        break;
    }
    if (result.light)
    {
        if (cameraLanded)
        {
            std::memcpy(_viewProjection, camera + kViewProjectionAt, sizeof _viewProjection);
            std::memcpy(_projection, camera + kProjectionOffset, sizeof _projection);
            std::memcpy(_view, camera + kViewAt, sizeof _view);
            // Both halves of the camera are checked: the projection's own shape is the guard on the buffer, and the view-projection is read
            // beside it so a held sun can be dropped on a camera cut, so a buffer whose +0x060 does not hold it is refused as NoCamera instead
            // of lighting the frame from a wrong screen position. The view is only trusted with the same copy that passed the guard: its second
            // row is what says whether the light's view-space direction is above the world's horizon.
            _haveProjection = ProjectionOk(_projection) && ViewProjectionOk(_viewProjection);
            _haveView = _haveProjection;
            _projectionRefused = !_haveProjection;
            result.projection = _haveProjection;
            result.projectionRefused = _projectionRefused;
        }
        else
        {
            // The camera's map has not landed: that is the GPU behind, not a refused projection. A refusal an earlier copy left would make the
            // caller count this frame as one, so it is cleared here.
            _projectionRefused = false;
            result.notReady = true; // the light's bytes are in, the camera's are still in the queue
        }
    }
    else
    {
        result.notReady = attempted;
    }
    if (result.notReady)
        ++_notReady;
    return result;
}

void SunReader::Forget()
{
    _haveFacts = false;
    _haveProjection = false;
    _haveView = false;
    _haveRead = false;
    _projectionRefused = false;
    _lastLight = nullptr;
    _facts = LightFacts {};
    _shadowCount = 0;
    // The copy the refused read took is not an accepted one: the next frame's read may take any age the ring still holds (ReadCandidate), so a light
    // that was refused does not cost the frames whose copy has not landed yet.
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
}

void SunReader::Reset()
{
    Forget();
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
    _reads = 0;
    _notReady = 0;
}

bool ToneReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* params, uint64_t frame)
{
    if (context == nullptr || params == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[RingIndex(frame)];
    if (!MadeOn(slot.staging.Get(), device.Get()))
    {
        slot = Slot {}; // a buffer of another device, or the first copy: make this slot's staging buffer
        if (!EnsureStaging(device.Get(), kToneMapBytes, slot.staging))
            return false;
    }
    const D3D11_BOX box = ByteBox(0, kToneMapBytes);
    context->CopySubresourceRegion(slot.staging.Get(), 0, 0, 0, 0, params, 0, &box);
    slot.frame = frame;
    slot.written = true;
    return true;
}

ToneRead ToneReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    ToneRead result;
    if (context == nullptr)
        return result;
    // The same age rule the light's ring uses: the copy the frame before this one made first, then an older one while the ring still holds it,
    // and never one older than the last copy a read took. A GPU a frame or two behind therefore still answers, where a map of one fixed age would
    // fail on every frame of such a session.
    for (uint32_t age = kNewestReadAge; age < kStagingRing; ++age)
    {
        if (frame < age)
            break; // the ring has not seen that many frames yet
        Slot& slot = _ring[SlotForAge(frame, age)];
        if (!ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        unsigned char bytes[kToneMapBytes] {};
        if (!MapRead(context, slot.staging.Get(), bytes, kToneMapBytes, block))
            continue; // this copy is still in the GPU's queue: try the next older one
        result.landed = true;
        std::memcpy(&result.exposure, bytes + kToneMapExposureAt, sizeof result.exposure);
        _haveRead = true;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        _lastReadFrame = frame;
        break;
    }
    return result;
}

void ToneReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
}

bool SunUvReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* params, uint64_t frame)
{
    if (context == nullptr || params == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[RingIndex(frame)];
    if (!MadeOn(slot.staging.Get(), device.Get()))
    {
        slot = Slot {}; // a buffer of another device, or the first copy: make this slot's staging buffer
        if (!EnsureStaging(device.Get(), kSunParamBytes, slot.staging))
            return false;
    }
    const D3D11_BOX box = ByteBox(0, kSunParamBytes);
    context->CopySubresourceRegion(slot.staging.Get(), 0, 0, 0, 0, params, 0, &box);
    slot.frame = frame;
    slot.written = true;
    return true;
}

SunUvRead SunUvReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    SunUvRead result;
    if (context == nullptr)
        return result;
    // The tone map's own ring rule: the copy the frame before this one made first, then an older one while the ring still holds it, and never one
    // older than the last copy a read took. A GPU two or three frames behind therefore still answers, where a map of one fixed age would fail on
    // every frame of such a session.
    for (uint32_t age = kNewestReadAge; age < kStagingRing; ++age)
    {
        if (frame < age)
            break; // the ring has not seen that many frames yet
        Slot& slot = _ring[SlotForAge(frame, age)];
        if (!ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        unsigned char bytes[kSunParamBytes] {};
        if (!MapRead(context, slot.staging.Get(), bytes, kSunParamBytes, block))
            continue; // this copy is still in the GPU's queue: try the next older one
        result.landed = true;
        result.writtenFrame = slot.frame;
        std::memcpy(result.uv, bytes, sizeof result.uv);
        _haveRead = true;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        _lastReadFrame = frame;
        break;
    }
    return result;
}

void SunUvReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
}
} // namespace ext::ffxiv::shafts
