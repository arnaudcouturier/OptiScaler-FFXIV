#include "pch.h"

#include "ext/ffxiv/engine/GodraysSun.h"

#include <cstring>

namespace ext::ffxiv::godrays
{
using Microsoft::WRL::ComPtr; // in scope for the member function definitions below (the anonymous namespace's using does not extend to them)

namespace
{
// A byte range of a constant buffer as the box a partial copy takes: for a buffer the box is in bytes (the radial's own readers' helper).
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
// still in the queue keeps the values it already had (the radial's own MapRead, with the no-wait flag in the flags argument).
bool MapRead(ID3D11DeviceContext* context, ID3D11Buffer* staging, void* bytes, uint32_t count, bool block)
{
    if (context == nullptr || staging == nullptr || bytes == nullptr)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    // The product's read never waits: a map that would block is a map of a copy the GPU has not finished, and the frame keeps the values
    // it had. The blocking form exists for the tests only; Map takes the flag on the context itself.
    const UINT flags = block ? 0u : UINT(D3D11_MAP_FLAG_DO_NOT_WAIT);
    const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, flags, &mapped);
    if (hr != S_OK || mapped.pData == nullptr)
        return false;
    std::memcpy(bytes, mapped.pData, count);
    context->Unmap(staging, 0);
    return true;
}

// One staging texture's first row, or false when the GPU has not finished the copy that fills it (the map never waits unless a test asks).
bool MapReadTexture(ID3D11DeviceContext* context, ID3D11Texture2D* staging, void* bytes, uint32_t count, bool block)
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

// Whether a staging buffer this ring made was created on `device` (the radial's own rule: a pair of another device cannot receive this
// context's copy, and CopySubresourceRegion refuses it silently).
bool MadeOn(ID3D11Buffer* buffer, ID3D11Device* device)
{
    if (buffer == nullptr || device == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> owner;
    buffer->GetDevice(&owner);
    return owner && owner.Get() == device;
}

} // namespace

bool FrameReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* light, ID3D11Buffer* camera, uint64_t frame)
{
    if (context == nullptr || light == nullptr || camera == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[shafts::RingIndex(frame)];
    if (!MadeOn(slot.light.Get(), device.Get()) || !MadeOn(slot.camera.Get(), device.Get()))
    {
        slot = Slot {};
        if (!EnsureStaging(device.Get(), shafts::kLightBytes, slot.light) || !EnsureStaging(device.Get(), shafts::kCameraBytes, slot.camera))
            return false;
    }
    const D3D11_BOX lightBox = ByteBox(0, shafts::kLightBytes);
    const D3D11_BOX cameraBox = ByteBox(0, shafts::kCameraBytes); // the whole camera: the inverse projection and the inverse view ride with it
    context->CopySubresourceRegion(slot.light.Get(), 0, 0, 0, 0, light, 0, &lightBox);
    context->CopySubresourceRegion(slot.camera.Get(), 0, 0, 0, 0, camera, 0, &cameraBox);
    slot.source = light;
    slot.frame = frame;
    slot.written = true;
    return true;
}

FrameRead FrameReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    FrameRead result;
    if (context == nullptr)
        return result;
    // Newest first: the copy the frame before this one made is the one that belongs to this frame's picture, and an older one is taken
    // only when it has not landed - the radial's own age rule, so a GPU a frame or two behind still answers.
    bool attempted = false;
    bool cameraLanded = false;
    unsigned char light[shafts::kLightBytes] {};
    unsigned char camera[shafts::kCameraBytes] {};
    for (uint32_t age = shafts::kNewestReadAge; age < shafts::kStagingRing; ++age)
    {
        if (frame < age)
            break;
        Slot& slot = _ring[shafts::SlotForAge(frame, age)];
        if (!shafts::ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        if (!MapRead(context, slot.light.Get(), light, shafts::kLightBytes, block))
        {
            attempted = true;
            continue;
        }
        std::memcpy(result.facts.position, light + 0x00, sizeof result.facts.position);
        std::memcpy(result.facts.direction, light + shafts::kLightDirectionAt, sizeof result.facts.direction);
        std::memcpy(result.facts.diffuse, light + shafts::kLightDiffuseAt, sizeof result.facts.diffuse);
        result.landed = true;
        result.writtenFrame = slot.frame;
        _haveRead = true;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        _lastReadFrame = frame;
        _lastLight = slot.source;
        ++_reads;
        cameraLanded = MapRead(context, slot.camera.Get(), camera, shafts::kCameraBytes, block);
        break;
    }
    if (result.landed && cameraLanded)
    {
        // The inverse projection at +0x0E0 is what the march's rays are reconstructed through, the inverse view's second row at +0x040 is
        // what turns a view point's world height out, and the projection at +0x120 is what places the sun's screen position for the auto
        // colour. All three come from the same copy as the light, so a light and a camera compared by the rules are one frame's.
        std::memcpy(result.inverseProjection, camera + kInverseProjectionAt, sizeof result.inverseProjection);
        std::memcpy(result.inverseViewRow1, camera + kInverseViewRow1At, sizeof result.inverseViewRow1);
        std::memcpy(result.projection, camera + shafts::kProjectionOffset, sizeof result.projection);
        // The view matrix's three rows, from the same copy: the elevation's world up and the held light's world direction both read them.
        std::memcpy(result.view, camera + shafts::kViewAt, sizeof result.view);
        result.haveView = true;
        // The camera's own guard is the radial's: the projection's shape is the buffer's, and the view-projection beside it is what a held
        // state is dropped on. The march's rays are only as good as this buffer.
        float viewProjection[shafts::kProjectionRows] {};
        std::memcpy(viewProjection, camera + shafts::kViewProjectionAt, sizeof viewProjection);
        result.camera = shafts::ProjectionOk(result.projection) && shafts::ViewProjectionOk(viewProjection);
    }
    else if (result.landed)
    {
        result.notReady = true; // the light's bytes are in, the camera's are still in the queue
        _lastCameraRefused = false;
    }
    else
    {
        result.notReady = attempted;
    }
    if (result.landed && cameraLanded)
        _lastCameraRefused = !result.camera; // the bytes arrived and are not this build's camera: a build mismatch
    if (result.notReady)
        ++_notReady;
    return result;
}

void FrameReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
    _reads = 0;
    _notReady = 0;
    _lastLight = nullptr;
    _lastCameraRefused = false;
}

bool FogReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* fogParam, ID3D11Buffer* profile, ID3D11Buffer* viewportParam,
                     uint64_t frame)
{
    if (context == nullptr || fogParam == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[shafts::RingIndex(frame)];
    if (!MadeOn(slot.fogParam.Get(), device.Get()))
    {
        slot = Slot {};
        if (!EnsureStaging(device.Get(), kFogParamBytes, slot.fogParam))
            return false;
    }
    const D3D11_BOX fogBox = ByteBox(0, kFogParamBytes);
    context->CopySubresourceRegion(slot.fogParam.Get(), 0, 0, 0, 0, fogParam, 0, &fogBox);
    // The height profile: the one buffer the caller picked by the slot the fog shader names it at, copied whole. No profile this frame
    // clears the slot's flag, so a read never decodes a copy from an older frame as this one's.
    slot.hasProfile = false;
    if (profile != nullptr)
    {
        if (!slot.profile || !MadeOn(slot.profile.Get(), device.Get()))
        {
            slot.profile.Reset();
            if (!EnsureStaging(device.Get(), kExpHeightFogBytes, slot.profile))
                return false;
        }
        const D3D11_BOX profileBox = ByteBox(0, kExpHeightFogBytes);
        context->CopySubresourceRegion(slot.profile.Get(), 0, 0, 0, 0, profile, 0, &profileBox);
        slot.hasProfile = true;
    }
    if (viewportParam != nullptr)
    {
        if (!slot.viewport || !MadeOn(slot.viewport.Get(), device.Get()))
        {
            slot.viewport.Reset();
            if (!EnsureStaging(device.Get(), kViewportParamBytes, slot.viewport))
                return false;
        }
        const D3D11_BOX viewportBox = ByteBox(0, kViewportParamBytes);
        context->CopySubresourceRegion(slot.viewport.Get(), 0, 0, 0, 0, viewportParam, 0, &viewportBox);
    }
    slot.frame = frame;
    slot.written = true;
    return true;
}

FogRead FogReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    FogRead result;
    if (context == nullptr)
        return result;
    bool attempted = false;
    unsigned char fog[kFogParamBytes] {};
    unsigned char profileBytes[kExpHeightFogBytes] {};
    unsigned char viewport[kViewportParamBytes] {};
    bool profileLanded = false;
    bool viewportLanded = false;
    for (uint32_t age = shafts::kNewestReadAge; age < shafts::kStagingRing; ++age)
    {
        if (frame < age)
            break;
        Slot& slot = _ring[shafts::SlotForAge(frame, age)];
        if (!shafts::ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        if (!MapRead(context, slot.fogParam.Get(), fog, kFogParamBytes, block))
        {
            attempted = true;
            continue;
        }
        profileLanded = slot.hasProfile && slot.profile && MapRead(context, slot.profile.Get(), profileBytes, kExpHeightFogBytes, block);
        viewportLanded = slot.viewport && MapRead(context, slot.viewport.Get(), viewport, kViewportParamBytes, block);
        result.landed = true;
        result.writtenFrame = slot.frame;
        _haveRead = true;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        _lastReadFrame = frame;
        break;
    }
    if (!result.landed)
    {
        result.landed = false;
        return result;
    }
    // The fog's own sun, from the 128-byte copy alone (the night curve does not depend on the profile being usable).
    result.sunLanded = FogSunOf(fog, result.sun);
    // The height profile: the copy of the buffer at the stamped slot. Its declared 48 bytes are checked before it is decoded, so a copy
    // taken from the wrong buffer (the clip-to-world matrix beside it) is refused and never read as a fog.
    result.profile = profileLanded;
    if (profileLanded)
    {
        if (ProfileRowUsable(profileBytes))
        {
            result.fog = DecodeFog(fog, profileBytes);
            result.usable = FogStateUsable(result.fog);
        }
        else
        {
            result.profileRefused = true;
        }
    }
    if (viewportLanded)
    {
        // The viewport buffer's second float4, raw, for the log line that sets it next to the mapping the march derives from the window.
        result.viewportLanded = true;
        result.ndcScale[0] = FloatAt(viewport, 0x10);
        result.ndcScale[1] = FloatAt(viewport, 0x14);
        result.ndcOffset[0] = FloatAt(viewport, 0x18);
        result.ndcOffset[1] = FloatAt(viewport, 0x1C);
    }
    return result;
}

void FogReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
}

bool StatsReader::Copy(ID3D11DeviceContext* context, ID3D11Texture2D* stats, uint64_t frame)
{
    if (context == nullptr || stats == nullptr)
        return false;
    D3D11_TEXTURE2D_DESC source {};
    stats->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_R32G32B32A32_FLOAT || source.Width != kStatsTexels || source.Height != 1)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[shafts::RingIndex(frame)];
    if (slot.staging)
    {
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        slot.staging->GetDevice(&owner);
        if (owner.Get() != device.Get())
            slot = Slot {};
    }
    if (!slot.staging)
    {
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &slot.staging)) || !slot.staging)
            return false;
    }
    context->CopyResource(slot.staging.Get(), stats);
    slot.frame = frame;
    slot.written = true;
    return true;
}

StatsRead StatsReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    StatsRead result;
    if (context == nullptr)
        return result;
    float raw[kStatsTexels * 4] {};
    for (uint32_t age = shafts::kNewestReadAge; age < shafts::kStagingRing; ++age)
    {
        if (frame < age)
            break;
        Slot& slot = _ring[shafts::SlotForAge(frame, age)];
        if (!shafts::ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        if (!MapReadTexture(context, slot.staging.Get(), raw, sizeof raw, block))
            continue;
        DecodeStats(raw, result);
        result.writtenFrame = slot.frame;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        break;
    }
    return result;
}

void StatsReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
}

bool CascadeReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* shadow, uint64_t frame)
{
    if (context == nullptr || shadow == nullptr)
        return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[shafts::RingIndex(frame)];
    // A slot a later frame reuses starts that frame's set over. The game's four cascade buffers keep their addresses from frame to frame, so
    // the keys of an earlier frame would skip every copy of the next one, and the slot would keep the earlier frame's number and age out of the
    // ring's read window for good; the staging buffers stay for reuse, and the keys past the count are never read.
    if (slot.written && slot.frame != frame)
    {
        slot.count = 0;
        slot.written = false;
    }
    // Pointer-keyed per frame: a buffer already copied this frame is skipped, so the 20 assembly draws of a frame cost four copies among
    // them (the diag's own per-pointer cadence, in product form).
    for (uint32_t i = 0; i < slot.count; ++i)
        if (slot.source[i] == shadow)
            return true;
    if (slot.count >= kCascadeCount)
        return false; // this frame's four are taken: a fifth distinct pointer is another draw's buffer, and the march needs no more
    if (!slot.staging[slot.count] || !MadeOn(slot.staging[slot.count].Get(), device.Get()))
    {
        slot.staging[slot.count].Reset();
        if (!EnsureStaging(device.Get(), kCascadeBytes, slot.staging[slot.count]))
            return false;
    }
    const D3D11_BOX box = ByteBox(0, kCascadeBytes);
    context->CopySubresourceRegion(slot.staging[slot.count].Get(), 0, 0, 0, 0, shadow, 0, &box);
    slot.source[slot.count] = shadow;
    ++slot.count;
    slot.frame = frame;
    slot.written = true;
    return true;
}

CascadeRead CascadeReader::Read(ID3D11DeviceContext* context, uint64_t frame, bool block)
{
    CascadeRead result;
    if (context == nullptr)
        return result;
    for (uint32_t age = shafts::kNewestReadAge; age < shafts::kStagingRing; ++age)
    {
        if (frame < age)
            break;
        Slot& slot = _ring[shafts::SlotForAge(frame, age)];
        if (!shafts::ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        result.count = 0;
        for (uint32_t i = 0; i < slot.count; ++i)
        {
            unsigned char bytes[kCascadeBytes] {};
            if (!slot.staging[i] || !MapRead(context, slot.staging[i].Get(), bytes, kCascadeBytes, block))
                continue; // this copy is still in the GPU's queue: the cascade it holds is simply not among this read's
            float asFloats[kCascadeBytes / 4] {};
            std::memcpy(asFloats, bytes, sizeof asFloats);
            Cascade& cascade = result.cascades[result.count];
            std::memcpy(cascade.matrix, asFloats, sizeof cascade.matrix);
            std::memcpy(cascade.fade, asFloats + 24, sizeof cascade.fade); // @0x60: the writer's cb3[6]
            cascade.distanceFar = asFloats[18];                            // @0x40.z: the shadow distance
            cascade.rampScale = asFloats[22];                              // @0x50.z: the distance ramp's scale
            cascade.rampTarget = asFloats[23];                             // @0x50.w: what the ramp lifts toward
            cascade.mapRowX = asFloats[28];                                // @0x70.x: the shared map row's first float
            cascade.shaped = ShadowMatrixShape(asFloats);
            ++result.count;
        }
        result.landed = result.count > 0;
        result.writtenFrame = slot.frame;
        if (result.landed)
        {
            _haveRead = true;
            _haveAccepted = true;
            _lastAcceptedFrame = slot.frame;
            _lastReadFrame = frame;
        }
        break;
    }
    return result;
}

void CascadeReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
}

ID3D11ShaderResourceView* StolenViews::Secure(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, uint32_t viewFormat, bool& ownView,
                                              uint32_t& width, uint32_t& height, uint32_t& format,
                                              Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view,
                                              Microsoft::WRL::ComPtr<ID3D11Texture2D>& copy)
{
    ownView = false;
    width = height = format = 0;
    if (context == nullptr || source == nullptr)
        return nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sourceDesc {};
    source->GetDesc(&sourceDesc); // GetDesc returns void, always succeeds for a valid view
    if (sourceDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
        return nullptr;
    ComPtr<ID3D11Resource> resource;
    source->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture)) || texture == nullptr)
        return nullptr;
    D3D11_TEXTURE2D_DESC textureDesc {};
    texture->GetDesc(&textureDesc);
    if (textureDesc.MipLevels != 1 || textureDesc.ArraySize != 1 || textureDesc.SampleDesc.Count != 1)
        return nullptr;
    width = textureDesc.Width;
    height = textureDesc.Height;
    format = uint32_t(sourceDesc.Format);
    // The steal is legal only when the resource itself carries the shader-resource bind flag (the SSR/AO rule): an own SRV on the game's
    // resource, else an owned copy of the same description. Nothing of the game is ever written either way.
    if ((textureDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC own {};
        own.Format = DXGI_FORMAT(viewFormat);
        own.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        own.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), &own, &view)) || !view)
            return nullptr;
        ownView = true;
        return view.Get();
    }
    D3D11_TEXTURE2D_DESC copyDesc {};
    copyDesc.Width = textureDesc.Width;
    copyDesc.Height = textureDesc.Height;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.Format = textureDesc.Format;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &copy)) || !copy)
        return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC own {};
    own.Format = DXGI_FORMAT(viewFormat);
    own.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    own.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(copy.Get(), &own, &view)) || !view)
    {
        copy.Reset();
        return nullptr;
    }
    context->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
    return view.Get();
}

ID3D11ShaderResourceView* StolenViews::SecureAtlas(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, bool& ownView, uint32_t& width,
                                                   uint32_t& height, uint32_t& format)
{
    // The atlas is a typeless plane the engine samples as R16_UNORM (the S0 slice's answer): the march's own view takes that format, and a
    // source whose view is already R16_UNORM is re-created on the resource so the march owns its view of it.
    ID3D11ShaderResourceView* secured = Secure(context, source, kAtlasSampledFormat, ownView, width, height, format, _atlas, _atlasCopy);
    if (secured != nullptr)
        _atlasSource = source;
    return secured;
}

ID3D11ShaderResourceView* StolenViews::SecureSky(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, bool& ownView, uint32_t& width,
                                                 uint32_t& height, uint32_t& format)
{
    ID3D11ShaderResourceView* secured = Secure(context, source, shafts::kFp16Format, ownView, width, height, format, _sky, _skyCopy); // FP16
    if (secured != nullptr)
        _skySource = source;
    return secured;
}

ID3D11ShaderResourceView* StolenViews::SecureLut(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, bool& ownView, uint32_t& width,
                                                 uint32_t& height, uint32_t& format)
{
    ID3D11ShaderResourceView* secured = Secure(context, source, kR16G16Format, ownView, width, height, format, _lut, _lutCopy); // R16G16_FLOAT
    if (secured != nullptr)
        _lutSource = source;
    return secured;
}

void StolenViews::KeepDepth(ID3D11ShaderResourceView* source, uint32_t format, uint32_t width, uint32_t height)
{
    _depth = source;
    _depthFormat = format;
    _depthWidth = width;
    _depthHeight = height;
}

void StolenViews::Reset()
{
    _atlas.Reset();
    _sky.Reset();
    _lut.Reset();
    _atlasCopy.Reset();
    _skyCopy.Reset();
    _lutCopy.Reset();
    _atlasSource = _skySource = _lutSource = nullptr;
    _depth.Reset();
    _depthFormat = _depthWidth = _depthHeight = 0;
}
} // namespace ext::ffxiv::godrays
