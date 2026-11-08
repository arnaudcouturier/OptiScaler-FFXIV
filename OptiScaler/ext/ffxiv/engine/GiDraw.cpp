#include "pch.h"

#include "ext/ffxiv/engine/Gi.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/diag/ShaderBindings.h" // the reflection's plain rows: what a bytecode parse produced
#include "ext/ffxiv/engine/ChainHooks.h"   // the chain's stage hooks, which this module registers
#include "ext/ffxiv/engine/EnginePass.h"   // OwnWork: this module's own draws are never re-classified
#include "ext/ffxiv/engine/EnginePassRules.h"
#include "ext/ffxiv/engine/GiPass.h"
#include "ext/ffxiv/engine/RecogRules.h" // FrameStep: one count per rendered frame, the step line
#include "ext/ffxiv/engine/ShaftsSun.h" // the tone map parameter ring (ToneReader): the frame's white point
#include "ext/hdr/scene/FfxivSceneHdr.h" // the scene HDR path replaces the tone chain this stage draws before
#include "ext/present/PresentSeams.h"    // the output-frame counter, the scene-HDR sample's token

#include "ext/ffxiv/diag/StageDump.h" // the per-stage image dump: this stage reports its run and its pictures on the capture frame

#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <thread>

// Screen-space light bounce: the compose on the tone-map owner chain, the draw path, the frame's captures and the module's session (Gi.h). The
// pure decisions are in GiRules.h, the frame's captures and the camera ring in this file, the three draws in GiPass.cpp, the shaders in
// shaders/Gi.hlsl + shaders/GiMarch.hlsl, the card in GiMenu.cpp.
//
// What it costs while it is off: the engine module's one call per draw into OnSeamDraw, which returns at two anchor-bit tests - no state query,
// no allocation and no log line. While it is on, per frame: the composite's draw is measured for the pass's own views (the normal target, the
// albedo carrier, the material pack, the view-position buffer and the camera table), the fog post's draw for the scene depth, and at the tone
// map the stage's three draws run and the chain binds this stage's copy of the scene at t0 for that one draw (the owner role) or takes this
// stage's output as its own (the chained role, the march's stage calling with the chain's current output). When no earlier stage and the march
// took the tone map, this stage is the chain's fallback owner from the seam's entry.
//
// Nothing of the game is written: the scene is copied into an owned texture, and the G-buffer family is read, for that one draw, through the
// views the game itself bound at the composite - no own SRV is created on a game resource, because the game already binds one, and for that
// same reason no owned copy is needed. The tone map's own binding is put back after its one draw, also when that draw throws.
//
// The frame's count opens at the frame's first composite after the previous frame's tone map (the game draws the composite twice a
// frame), which also re-captures: the composite is the one draw a rendered frame reliably makes before its tone map, and every read the stage makes at the tone map takes the bytes the frame before it wrote (the march's own ring ages).
//
// The stamping contract: the chain's shared state - the composite's capture stamp the puddle stage keeps - is file-local to that stage's
// translation unit and unreachable from here. This stage's own answered-tracking is its own rendered-frame counter (giFrame), and a decline
// writes nothing but this module's own counters and log.
namespace ext::ffxiv::engine::gi
{
namespace
{
using Microsoft::WRL::ComPtr;
using ext::ffxiv::engine::Knob;
using ext::ffxiv::engine::kConsecutiveDeclineLimit;

// The render targets a draw is measured at: D3D11's own maximum.
inline constexpr uint32_t kMaxRenderTargets = 8;

// The tone map's own parameter buffer (`cCommonTexParam`) sits in its pixel shader's first constant slot, the march's own slot for the same
// buffer.
inline constexpr uint32_t kToneMapSlot = 0;

inline constexpr uint32_t kTextureSlots = engine::kShaderResourceSlots; // D3D11's 128 shader-resource slots
inline constexpr uint32_t kConstantBufferSlots = 14;
inline constexpr uint8_t kSlotMissing = 0xFF;

// A read that has not landed for this long is not a GPU that is behind any more: one line says so, and the frames are skipped (the march's own
// staleness rule, this stage's own instance).
inline constexpr uint64_t kMaxReadbackAge = shafts::kMaxReadbackAge;

// The four names this stage reads at the frame's composite: the slots they are bound at are stamped on the composite's own shader once at
// creation (NoteShader, below). Two are names EnginePassRules.h's shared table already carries; the third is the view-position buffer's row;
// the fourth is the camera table the whole family reads; the fifth is the material pack the apply shader's gates read. Five slots, five times
// eight bits, in the one word a stamp carries.
inline constexpr const char* kNameNormal = "g_SamplerGBuffer";
inline constexpr const char* kNameAlbedo = "g_SamplerGBuffer2";
inline constexpr const char* kNameMaterial = "g_SamplerGBuffer1";
inline constexpr const char* kNameViewPos = "g_SamplerViewPosition";
inline constexpr const char* kNameCamera = "g_CameraParameter";

// The formats the frame's own resources carry, as the views report them (the AO pass's own identifiers for the same family).
inline constexpr uint32_t kBytes8BitFormat = 87; // DXGI_FORMAT_B8G8R8A8_UNORM, the G-buffer draws' own 8-bit targets
inline constexpr uint32_t kViewPosFormat = 10;   // DXGI_FORMAT_R16G16B16A16_FLOAT, the view positions and the scene

// A composite this stage keeps needs the scene, the G-buffer views and the depth to belong to one frame's own allocation, and the shapes the
// draw measured: the composite's render target is that scene, so its size is the allocation's bound the tone map's window sits in.
inline constexpr uint32_t kMinCaptureWidth = 64;

// ---- the composite shader's stamp (the AO pass's pattern, this stage's five slots) ------------------------------------

// What one composite's shader declares, as this stage carries it. A slot of 0xFF is a name the shader does not have; the whole word of 0 is
// "not this stage's shader", which is every other shader of the game.
struct StampInputs
{
    uint8_t normal = kSlotMissing;
    uint8_t albedo = kSlotMissing;
    uint8_t material = kSlotMissing;
    uint8_t viewPos = kSlotMissing;
    uint8_t camera = kSlotMissing;

    constexpr bool Complete() const noexcept
    {
        return normal < kTextureSlots && albedo < kTextureSlots && material < kTextureSlots && viewPos < kTextureSlots &&
               camera < kConstantBufferSlots;
    }
};

constexpr StampInputs UnpackStamp(uint64_t packed) noexcept
{
    StampInputs inputs;
    const uint32_t normal = uint32_t(packed & 0xFFu);
    const uint32_t albedo = uint32_t((packed >> 8) & 0xFFu);
    const uint32_t material = uint32_t((packed >> 16) & 0xFFu);
    const uint32_t viewPos = uint32_t((packed >> 24) & 0xFFu);
    const uint32_t camera = uint32_t((packed >> 32) & 0xFFu);
    inputs.normal = normal == 0 ? kSlotMissing : uint8_t(normal - 1);
    inputs.albedo = albedo == 0 ? kSlotMissing : uint8_t(albedo - 1);
    inputs.material = material == 0 ? kSlotMissing : uint8_t(material - 1);
    inputs.viewPos = viewPos == 0 ? kSlotMissing : uint8_t(viewPos - 1);
    inputs.camera = camera == 0 ? kSlotMissing : uint8_t(camera - 1);
    return inputs;
}

constexpr uint64_t PackStamp(const StampInputs& inputs) noexcept
{
    const uint64_t normal = inputs.normal < kTextureSlots ? uint64_t(inputs.normal) + 1u : 0u;
    const uint64_t albedo = inputs.albedo < kTextureSlots ? uint64_t(inputs.albedo) + 1u : 0u;
    const uint64_t material = inputs.material < kTextureSlots ? uint64_t(inputs.material) + 1u : 0u;
    const uint64_t viewPos = inputs.viewPos < kTextureSlots ? uint64_t(inputs.viewPos) + 1u : 0u;
    const uint64_t camera = inputs.camera < kConstantBufferSlots ? uint64_t(inputs.camera) + 1u : 0u;
    return normal | (albedo << 8) | (material << 16) | (viewPos << 24) | (camera << 32);
}

// The one private-data tag this module writes on a game object: the other pass families' stamps carry their own GUIDs, and a GUID this
// specific does not collide with them.
constexpr GUID kStampTag { 0x7c1f2a68, 0x6f31, 0x4d90, { 0xa3, 0x47, 0x51, 0x2e, 0x8b, 0x64, 0xd9, 0x05 } };

// The slots one shader declares, from the binding table a bytecode parse produced: only a shader that declares all five is a composite this
// stage keeps, because a draw that carries four of the frame's five members (a material draw with its own G-buffer picks, say) must not be
// answered as the composite (the AO pass's multi-reader rule, drawn here as one refusal instead of many).
StampInputs StampOfShader(const void* code, size_t size)
{
    StampInputs inputs;
    if (code == nullptr || size < 32)
        return inputs;
    ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
    const size_t count = ext::ffxiv::diag::ParseBindings(code, size, parsed, ext::ffxiv::diag::kMaxBindings);
    const uint32_t normal = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::Texture, kNameNormal);
    const uint32_t albedo = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::Texture, kNameAlbedo);
    const uint32_t material = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::Texture, kNameMaterial);
    const uint32_t viewPos = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::Texture, kNameViewPos);
    const uint32_t camera = ext::ffxiv::diag::BindPointOf(parsed, count, ext::ffxiv::diag::BindKind::ConstantBuffer, kNameCamera);
    if (normal < kTextureSlots)
        inputs.normal = uint8_t(normal);
    if (albedo < kTextureSlots)
        inputs.albedo = uint8_t(albedo);
    if (material < kTextureSlots)
        inputs.material = uint8_t(material);
    if (viewPos < kTextureSlots)
        inputs.viewPos = uint8_t(viewPos);
    if (camera < kConstantBufferSlots)
        inputs.camera = uint8_t(camera);
    if (!inputs.Complete())
        return StampInputs {}; // an incomplete shader takes no stamp: a partial frame's shader must never be answered as the composite's
    return inputs;
}

bool StampShader(ID3D11DeviceChild* shader, uint64_t packed)
{
    if (shader == nullptr || packed == 0)
        return false; // 0 is "nothing read": writing it would only make an unstamped shader indistinguishable at the same cost
    return SUCCEEDED(shader->SetPrivateData(kStampTag, sizeof packed, &packed));
}

uint64_t ReadStampRaw(ID3D11DeviceChild* shader)
{
    if (shader == nullptr)
        return 0;
    uint64_t packed = 0;
    UINT bytes = sizeof packed;
    if (FAILED(shader->GetPrivateData(kStampTag, &bytes, &packed)) || bytes != sizeof packed)
        return 0;
    return packed;
}

StampInputs ReadStamp(ID3D11DeviceChild* shader) { return UnpackStamp(ReadStampRaw(shader)); }

// ---- what one bound texture answers about its shape ----------------------------------------------------------------

struct Shape
{
    bool twoD = false;
    uint32_t format = 0, width = 0, height = 0, samples = 0;
    const void* resource = nullptr;
};

Shape Describe(ID3D11ShaderResourceView* view)
{
    Shape shape;
    if (view == nullptr)
        return shape;
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
    view->GetDesc(&viewDesc);
    shape.twoD = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    shape.format = uint32_t(viewDesc.Format);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource.As(&texture)) || texture == nullptr)
        return shape;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    shape.resource = texture.Get();
    shape.width = desc.Width;
    shape.height = desc.Height;
    shape.samples = desc.SampleDesc.Count;
    return shape;
}

// The frame's kept views: a single-sample 2D texture of a known format and a real size, one image per draw this stage reads. The view reference
// the game already bound is kept with the reference's own count (PSGetShaderResources add-refs what it returns, and the ComPtr owns exactly
// that reference): no own SRV on a game resource, no owned copy - the game's texture is read through the slot view the game itself bound, and
// the pass's draws run between an explicit save and restore of the state they touch so no game binding is left changed.
constexpr bool UsableView(const Shape& shape, uint32_t wantedFormat) noexcept
{
    return shape.twoD && shape.format == wantedFormat && shape.samples == 1 && shape.width >= kMinCaptureWidth &&
           shape.height >= kMinCaptureWidth;
}

struct Kept
{
    ComPtr<ID3D11ShaderResourceView> view;
    uint32_t format = 0, width = 0, height = 0;

    void Keep(const ComPtr<ID3D11ShaderResourceView>& bound, const Shape& shape)
    {
        view = bound;
        format = shape.format;
        width = shape.width;
        height = shape.height;
    }

    void Clear()
    {
        view.Reset();
        format = width = height = 0;
    }

    bool Present() const noexcept { return view != nullptr; }
};

// What one frame's composite yielded for the pass: the four views it binds (behind the stamp, so nothing is guessed at the draw), and the
// composite's own render target - the frame's lit scene, whose identity the tone map's own t0 test needs.
struct CompositeCapture
{
    Kept normal, albedo, material, viewPos;
    const void* sceneResource = nullptr;
    uint32_t sceneWidth = 0, sceneHeight = 0, sceneFormat = 0;
    uint64_t sceneFrames = 0;

    bool ViewsComplete() const noexcept
    {
        return normal.Present() && albedo.Present() && material.Present() && viewPos.Present();
    }

    void Reset()
    {
        normal.Clear();
        albedo.Clear();
        material.Clear();
        viewPos.Clear();
        sceneResource = nullptr;
        sceneWidth = sceneHeight = sceneFormat = 0;
        sceneFrames = 0;
    }
};

struct DepthKeep
{
    ComPtr<ID3D11ShaderResourceView> view;
    uint32_t format = 0, width = 0, height = 0;

    void Clear() { view.Reset(); format = width = height = 0; }
    bool Present() const noexcept { return view != nullptr; }
};

// ---- the camera ring (the tone-map exposure ring's sibling, this stage's own instance) ----------------------------------

// One read of the frame's camera, from a staging ring of four: copied at the composite (the one draw of the frame that binds the camera table
// the composite itself samples), mapped at a later frame's tone map, never waiting for the GPU - the same four-slot age rule the light-buffer
// rings use, because a copy is never mapped at the frame that made it.
struct CameraRead
{
    bool landed = false;     // the copy was mapped
    uint64_t writtenFrame = 0; // the rendered frame the copy was made in
    float projection[16] {}; // g_CameraParameter +0x120: the projection the march's rays project through
    float view[12] {};       // +0x000: the view matrix's three rows, the world normal's view-space ones
};

class CameraReader
{
  public:
    // Copies this frame's camera bytes into the ring slot for `frame`. False when the draw bound no camera-shaped buffer, or the staging
    // buffer could not be created.
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* camera, uint64_t frame);

    // Maps the newest copy the ring still holds of the frames before this one, without waiting - the same age rule the tone map's ring uses.
    CameraRead Read(ID3D11DeviceContext* context, uint64_t frame);

    bool HaveRead() const noexcept { return _haveRead; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }
    void Reset();

  private:
    struct Slot
    {
        ComPtr<ID3D11Buffer> staging;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[shafts::kStagingRing];
    bool _haveRead = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
};

// A byte range of a constant buffer as the box a copy takes: for a buffer the box is in bytes (ShaftsSun.cpp's own helper, this stage's copy).
D3D11_BOX ByteBox(uint32_t offset, uint32_t bytes)
{
    D3D11_BOX box {};
    box.left = offset;
    box.right = offset + bytes;
    box.top = 0;
    box.front = 0;
    box.bottom = 1;
    box.back = 1;
    return box;
}

bool EnsureStaging(ID3D11Device* device, uint32_t bytes, ComPtr<ID3D11Buffer>& target)
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
    ComPtr<ID3D11Buffer> created;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &created)) || !created)
        return false;
    target = created;
    return true;
}

// Whether a staging buffer this ring made was created on `device`. A buffer of another device cannot receive this context's copy:
// CopySubresourceRegion returns void and refuses it silently, every frame, and the read then maps bytes nobody wrote - so a buffer that
// answers no is rebuilt instead of reused.
bool MadeOn(ID3D11Buffer* buffer, ID3D11Device* device)
{
    if (buffer == nullptr || device == nullptr)
        return false;
    ComPtr<ID3D11Device> owner;
    buffer->GetDevice(&owner);
    return owner && owner.Get() == device;
}

// One staging buffer's bytes, or false when the GPU has not finished the copy that fills it. The map never waits: it takes the no-wait flag in
// its flags argument, and there is no other form.
bool MapRead(ID3D11DeviceContext* context, ID3D11Buffer* staging, void* bytes, uint32_t count)
{
    if (context == nullptr || staging == nullptr || bytes == nullptr)
        return false;
    D3D11_MAPPED_SUBRESOURCE mapped {};
    const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr != S_OK || mapped.pData == nullptr)
        return false;
    std::memcpy(bytes, mapped.pData, count);
    context->Unmap(staging, 0);
    return true;
}

constexpr uint32_t RingIndex(uint64_t frame) noexcept { return uint32_t(frame % shafts::kStagingRing); }

constexpr uint32_t SlotForAge(uint64_t frame, uint32_t age) noexcept { return RingIndex(frame >= age ? frame - age : 0); }

constexpr bool ReadCandidate(bool written, uint64_t writtenFrame, uint64_t frame, bool haveAccepted, uint64_t lastAcceptedFrame) noexcept
{
    if (!written || writtenFrame >= frame)
        return false; // never a copy of the frame that is still being drawn, and never an unwritten slot
    if (!haveAccepted)
        return true; // the first read takes anything older than the frame it runs at
    return writtenFrame >= lastAcceptedFrame && writtenFrame < frame;
}

bool CameraReader::Copy(ID3D11DeviceContext* context, ID3D11Buffer* camera, uint64_t frame)
{
    if (context == nullptr || camera == nullptr)
        return false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    Slot& slot = _ring[RingIndex(frame)];
    if (!MadeOn(slot.staging.Get(), device.Get()))
    {
        slot = Slot {}; // a buffer of another device, or the first copy: make this slot's staging buffer
        if (!EnsureStaging(device.Get(), shafts::kCameraBytes, slot.staging))
            return false;
    }
    const D3D11_BOX box = ByteBox(0, shafts::kCameraBytes);
    context->CopySubresourceRegion(slot.staging.Get(), 0, 0, 0, 0, camera, 0, &box);
    slot.frame = frame;
    slot.written = true;
    return true;
}

CameraRead CameraReader::Read(ID3D11DeviceContext* context, uint64_t frame)
{
    CameraRead result;
    if (context == nullptr)
        return result;
    unsigned char bytes[shafts::kCameraBytes] {};
    for (uint32_t age = shafts::kNewestReadAge; age < shafts::kStagingRing; ++age)
    {
        if (frame < age)
            break; // the ring has not seen that many frames yet
        Slot& slot = _ring[SlotForAge(frame, age)];
        if (!ReadCandidate(slot.written, slot.frame, frame, _haveAccepted, _lastAcceptedFrame))
            continue;
        if (!MapRead(context, slot.staging.Get(), bytes, shafts::kCameraBytes))
            continue; // this copy is still in the GPU's queue: try the next older one
        result.landed = true;
        std::memcpy(result.projection, bytes + shafts::kProjectionOffset, sizeof result.projection);
        std::memcpy(result.view, bytes + shafts::kViewAt, sizeof result.view);
        result.writtenFrame = slot.frame;
        _haveRead = true;
        _haveAccepted = true;
        _lastAcceptedFrame = slot.frame;
        _lastReadFrame = frame;
        break;
    }
    return result;
}

void CameraReader::Reset()
{
    for (Slot& slot : _ring)
        slot = Slot {};
    _haveRead = false;
    _haveAccepted = false;
    _lastAcceptedFrame = 0;
    _lastReadFrame = 0;
}

// ---- the session ------------------------------------------------------------------------------------------------------------

// One accepted frame state, in the camera the pass draws with, held across gaps with the march's own fade: the camera of a frame whose own read
// misses is the last accepted state held for at most kHoldFrames rendered frames, and a camera cut empties the hold (its own line, the march's
// own rule, because a state from before a cut does not belong to the new picture).
struct CameraHold
{
    bool have = false;
    float projection[16] {};
    float view[12] {};
    uint64_t frame = 0;

    void Accept(const CameraRead& read, uint64_t at)
    {
        std::memcpy(projection, read.projection, sizeof projection);
        std::memcpy(view, read.view, sizeof view);
        have = true;
        frame = at;
    }

    void Drop()
    {
        have = false;
        frame = 0;
    }

    uint32_t Age(uint64_t now) const noexcept { return have && now >= frame ? uint32_t(now - frame) : uint32_t(shafts::kHoldFrames + 1u); }
    float Weight(uint64_t now) const noexcept { return have ? shafts::HoldFade(Age(now)) : 0.0f; }
};

struct State
{
    Pass pass;
    ext::ffxiv::engine::Session session;
    LogGate log;
    Params params;
    bool enabled = false;
    CameraReader cameraReader; // the camera ring at the composite, read at the tone map (never waiting)
    shafts::ToneReader tone;   // the tone map's own exposure, read a frame late: the frame's white point
    CompositeCapture capture;  // the frame's kept views and its scene identity, reset at the next composite
    DepthKeep depth;           // the fog post's own t0, what the rays march against
    float lastCamera[shafts::kProjectionRows] {}; // the newest camera a read landed with: the projection a camera cut reads against
    bool haveLastCamera = false;
    bool cameraRefused = false; // the last read's bytes arrived and are not this build's camera (a build mismatch, counted)
    CameraHold hold;            // the last accepted camera state, held across gaps with the march's own fade
    uint64_t frames = 0;        // the module's own count of rendered frames, opened at the frame's composite (FrameStep)
    shafts::FrameStep step;
    shafts::ChangeLine stepLine; // the step line's own once-per-state memory
    uint32_t cameraAge = shafts::kRingNotLanded; // the age of the copy the last read-back took at the tone map, in counts
    uint64_t stallBase = 0;     // the count the camera ring was last reset at: the "never landed" line counts from there
    // The stage's own answered-tracking, never the chain's shared stamp (the stamping contract).
    uint64_t giFrame = 0;
    bool haveGiFrame = false;
    // What the card shows and what the log says.
    uint32_t runs = 0;
    uint32_t heldRuns = 0;
    uint32_t declined = 0;
    uint32_t declineds[kDeclineReasonCount] {};
    CardCounts skips;
    uint32_t refusals = 0;    // the camera refusals (a build mismatch), counted toward the stand-down
    float whitePoint = shafts::kFallbackWhitePoint;
    bool haveWhitePoint = false;
    uint64_t whiteFrame = 0;
    uint64_t nextCountsLog = 0;
    bool loggedOff = false;
    bool loggedCost = false;
    bool loggedCameraRefused = false;
    bool loggedStale = false;
    bool loggedStall = false;
    bool loggedCameraCut = false;
    bool loggedComposite = false;
    bool loggedDepth = false;
    ext::ffxiv::shafts::SceneHdrWatch hdr;
    uint64_t hdrSampleFrame = 0;
    bool haveHdrSample = false;
    std::thread::id renderThread;
    bool haveRenderThread = false;
    uint32_t lastWindowWidth = 0, lastWindowHeight = 0;
    uint32_t lastGiWidth = 0, lastGiHeight = 0;
    // The capture frame's two numbers as the stage line's words, kept for the call that puts them on the line. The line's values string is read when
    // a capture window opens, BEFORE this frame's stage has run, so StageValues() does not carry them (it would print the previous capture's numbers
    // under this one's line); the frame-time call that does (the DoF branch's StageNoteText) is made where the numbers are computed.
    std::string captureText;
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// Whether the stage wants to see a draw at all (defined below, beside the other entries): the key is on, the strength is above zero, the
// module has not stood down, and the scene HDR path is off.
bool ArmedInternal(State& state);

bool SceneHdrRequested() { return ext::hdr::FfxivSceneHdr::Requested(); }

// ---- the counters and the one line a change of reason writes ---------------------------------------------------------------

void NoteDecline(State& state, Decline reason, Outcome outcome)
{
    // A later tone-shaped draw of the frame (the engine's own post-tone copy) is not a decision about the capture frame: it never replaces the real reason.
    if (reason != Decline::NotToneMap)
        ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Gi, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));
    state.declined++;
    if (uint32_t(reason) < kDeclineReasonCount)
        ++state.declineds[uint32_t(reason)];
    if (CountsTowardsStandDown(reason))
    {
        if (reason == Decline::NoCamera)
            ++state.refusals;
        if (state.session.NoteDecline(Knob::Gi) && !state.loggedOff)
        {
            state.loggedOff = true;
            LOG_WARN("{}", FormatSwitchedOff(kConsecutiveDeclineLimit));
            ext::SetActive(ext::F::Gi, false); // the draw path is back to two bit tests
        }
    }
    if (state.log.Take(uint32_t(reason) | (uint32_t(outcome) << 8)))
        LOG_INFO("{}", FormatDecline(reason, outcome));
}

// One frame left as it was without counting anything against the module. Not a failure - the streak is untouched, and the line says why.
void NoteSkip(State& state, SkipReason reason)
{
    ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::Gi, ext::ffxiv::diag::StageOutcome::Declined,
                                  DeclineName(reason == SkipReason::NoGBuffer ? Decline::NoGBuffer
                                              : reason == SkipReason::NoDepth ? Decline::NoDepth
                                                                              : Decline::NoCamera));
    CountSkip(state.skips, reason);
    if (state.log.Take(0x20000010u | uint32_t(reason)))
        LOG_INFO("{}", FormatSkip(reason));
}

// The card's numbers into the log about once a minute of rendered frames, from the frame's close (the march's own cadence).
inline constexpr uint32_t kCountsLogFrames = 1200;

void LogCountsIfDue(State& state)
{
    if (state.frames < state.nextCountsLog)
        return;
    state.nextCountsLog = state.frames + kCountsLogFrames;
    CardNumbers numbers;
    numbers.strength = state.params.strength;
    numbers.frames = state.frames;
    numbers.runs = state.runs;
    numbers.held = state.heldRuns;
    numbers.declined = state.declined;
    numbers.skipped = state.skips.skipped;
    numbers.noGBuffer = state.skips.noGBuffer;
    numbers.noDepth = state.skips.noDepth;
    numbers.noCamera = state.skips.noCamera;
    numbers.refused = state.refusals;
    numbers.windowWidth = state.lastWindowWidth;
    numbers.windowHeight = state.lastWindowHeight;
    numbers.giWidth = state.lastGiWidth;
    numbers.giHeight = state.lastGiHeight;
    LOG_INFO("{}", FormatCard(numbers));
}

// ---- the frame's composite capture -------------------------------------------------------------------------------------------

// The frame's composite: the one draw a rendered frame reliably makes before its tone map, and the stage's frame boundary. The count opens
// here (once a frame, FrameStep), the composite's render target is the scene identity, and the composite's own bind list is read for the pass's
// own views and the camera table. A draw whose shader the stamp leaves unstamped is not a composite this stage keeps anything for: the frames
// before its first stamped draw have the pass's views from no drawn frame at all, and the tone map's skip line says which.
void CaptureComposite(ID3D11DeviceContext* context, State& state)
{
    // The count opens once a frame, at the first composite after the previous frame's tone map: the captured frame draws the composite
    // twice in a row with identical bindings, and a count that moved at both draws stepped by two a frame. The second draw is captured as
    // well (the same views, the same camera table, the same ring slot), which changes nothing.
    state.step.OpenAtComposite(state.frames);

    // The scene identity, first: exactly one render target, whose texture is the frame's lit scene - the tone map's t0 test's resource, its
    // format and its size (the march's own record at this same draw; this stage keeps its own).
    bool haveScene = false;
    {
        ID3D11RenderTargetView* views[2] { nullptr, nullptr };
        context->OMGetRenderTargets(2, views, nullptr);
        ComPtr<ID3D11Texture2D> scene;
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
        if (views[0] != nullptr && views[1] == nullptr)
        {
            views[0]->GetDesc(&viewDesc);
            ComPtr<ID3D11Resource> resource;
            views[0]->GetResource(&resource);
            if (resource != nullptr && SUCCEEDED(resource.As(&scene)) && scene != nullptr)
            {
                D3D11_TEXTURE2D_DESC desc {};
                scene->GetDesc(&desc);
                if (desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && desc.Width >= kMinCaptureWidth && desc.Height >= kMinCaptureWidth &&
                    shafts::IsSceneFormat(uint32_t(viewDesc.Format)))
                    haveScene = true;
            }
        }
        for (ID3D11RenderTargetView*& view : views)
            if (view != nullptr)
            {
                view->Release();
                view = nullptr;
            }
        if (haveScene)
        {
            D3D11_TEXTURE2D_DESC desc {};
            scene->GetDesc(&desc);
            state.capture.sceneResource = scene.Get();
            state.capture.sceneWidth = desc.Width;
            state.capture.sceneHeight = desc.Height;
            state.capture.sceneFormat = uint32_t(viewDesc.Format);
            state.capture.sceneFrames = state.frames;
        }
        else
        {
            // A composite whose render target is not the frame's scene empties the scene identity: a tone map of this frame is then not one
            // this stage answers (composing onto a picture that belongs to an earlier frame is exactly what the reset prevents).
            state.capture.sceneResource = nullptr;
            state.capture.sceneWidth = state.capture.sceneHeight = 0;
            state.capture.sceneFormat = 0;
            state.capture.sceneFrames = 0;
        }
    }

    // The pass's own views, at the slots the shader object's stamp names (never a parse at the draw itself).
    ComPtr<ID3D11PixelShader> shader;
    {
        ID3D11PixelShader* bound = nullptr;
        context->PSGetShader(&bound, nullptr, nullptr);
        if (bound != nullptr)
            shader.Attach(bound);
    }
    const StampInputs slots = ReadStamp(shader.Get());
    if (!slots.Complete())
        return; // an unstamped composite: not this stage's draw; the log notices once, the capture keeps nothing this frame

    Kept normal, albedo, material, viewPos;
    Kept* const kept[4] { &normal, &albedo, &material, &viewPos };
    const uint8_t textureSlots[4] { slots.normal, slots.albedo, slots.material, slots.viewPos };
    for (uint32_t which = 0; which < 4; ++which)
    {
        ID3D11ShaderResourceView* bound = nullptr;
        context->PSGetShaderResources(textureSlots[which], 1, &bound);
        if (bound == nullptr)
            continue; // the slot's view is empty: the frame's set is incomplete and stays that way
        ComPtr<ID3D11ShaderResourceView> owned;
        owned.Attach(bound); // the read add-ref'd: either carried for the frame below or let go now
        const Shape shape = Describe(owned.Get());
        const uint32_t wantedFormat = which == 3 ? kViewPosFormat : kBytes8BitFormat;
        if (!UsableView(shape, wantedFormat))
            continue;
        if (state.capture.sceneWidth != 0 && (shape.width != state.capture.sceneWidth || shape.height != state.capture.sceneHeight))
            continue; // a texture of another size: another draw's binding (the march's own rule)
        kept[which]->Keep(owned, shape);
    }

    // The camera table: the slot the stamp named, else the shape rule's answer among the draw's own buffers (the march's CaptureLightDraw's
    // own pick, bounded above so a routing buffer of another family cannot win).
    {
        ID3D11Buffer* buffers[kConstantBufferSlots] {};
        context->PSGetConstantBuffers(0, kConstantBufferSlots, buffers);
        ComPtr<ID3D11Buffer> bound[kConstantBufferSlots];
        for (uint32_t i = 0; i < kConstantBufferSlots; ++i)
            if (buffers[i] != nullptr)
                bound[i].Attach(buffers[i]);
        ComPtr<ID3D11Buffer> camera;
        uint32_t cameraBytes = 0;
        // The stamp's slot first: the table the composite shader itself declares at that slot is the camera table this frame's draws bind.
        if (slots.camera < kConstantBufferSlots && bound[slots.camera])
        {
            D3D11_BUFFER_DESC desc {};
            bound[slots.camera]->GetDesc(&desc);
            if (desc.ByteWidth >= shafts::kCameraBytes)
            {
                cameraBytes = desc.ByteWidth;
                camera = bound[slots.camera];
            }
        }
        // The shape rule, when the stamp named no slot: the biggest camera-shaped buffer among the draw's own (the march's own pick, bounded
        // above so a routing buffer of another family cannot win).
        for (const ComPtr<ID3D11Buffer>& buffer : bound)
        {
            if (!buffer || buffer.Get() == camera.Get())
                continue;
            D3D11_BUFFER_DESC desc {};
            buffer->GetDesc(&desc);
            if (desc.ByteWidth < shafts::kCameraBytes || desc.ByteWidth > 0x400 || desc.ByteWidth <= cameraBytes)
                continue;
            cameraBytes = desc.ByteWidth;
            camera = buffer;
        }
        // The camera's bytes, one copy a frame: the ring never maps the copy the frame that made it is still drawing.
        if (camera != nullptr)
            state.cameraReader.Copy(context, camera.Get(), state.frames);
    }

    // The views, one set a frame: whatever the pass fails to complete, the previous frame's complete set is not touched.
    if (normal.Present() && albedo.Present() && material.Present() && viewPos.Present())
    {
        state.capture.normal = std::move(normal);
        state.capture.albedo = std::move(albedo);
        state.capture.material = std::move(material);
        state.capture.viewPos = std::move(viewPos);
        if (!state.loggedComposite)
        {
            state.loggedComposite = true;
            LOG_INFO("{}", FormatSecured("G-buffer views", false, state.capture.normal.width, state.capture.normal.height,
                                         state.capture.normal.format));
        }
    }
}

// The engine fog post: the one draw a frame binds the scene depth at (the march's own capture, this stage's own). The depth is a view of the
// frame's own allocation, in the reversed-Z the march family reads, and the stage keeps the reference the game bound (no own SRV, no copy).
void CaptureDepthAtFogPost(ID3D11DeviceContext* context, State& state)
{
    ID3D11ShaderResourceView* textureViews[8] {};
    context->PSGetShaderResources(0, 8, textureViews);
    ComPtr<ID3D11ShaderResourceView> textures[8];
    for (uint32_t i = 0; i < 8; ++i)
        if (textureViews[i] != nullptr)
            textures[i].Attach(textureViews[i]);
    for (const ComPtr<ID3D11ShaderResourceView>& texture : textures)
    {
        if (!texture)
            continue;
        const Shape shape = Describe(texture.Get());
        if (!shape.twoD || !shafts::IsDepthFormat(shape.format) || shape.samples != 1)
            continue;
        if (state.capture.sceneWidth != 0 && (shape.width != state.capture.sceneWidth || shape.height != state.capture.sceneHeight))
            continue; // a depth of another size than the scene the composite wrote is another draw's binding, and the stage keeps the last one
        state.depth.view = texture;
        state.depth.format = shape.format;
        state.depth.width = shape.width;
        state.depth.height = shape.height;
        if (!state.loggedDepth)
        {
            state.loggedDepth = true;
            LOG_INFO("{}", FormatSecured("scene depth", false, state.depth.width, state.depth.height, state.depth.format));
        }
        return;
    }
}

// The frame's white point, from the tone map's own parameters at this draw (the march's own read, this stage's own instance).
void ReadToneMapParams(ID3D11DeviceContext* context, State& state)
{
    ID3D11Buffer* buffer = nullptr;
    context->PSGetConstantBuffers(kToneMapSlot, 1, &buffer);
    ComPtr<ID3D11Buffer> params;
    if (buffer != nullptr)
        params.Attach(buffer);
    if (params)
        state.tone.Copy(context, params.Get(), state.frames);
    const shafts::ToneRead read = state.tone.Read(context, state.frames);
    if (read.landed && shafts::ExposureUsable(read.exposure))
    {
        const float whitePoint = shafts::WhitePointFromExposure(read.exposure);
        if (shafts::WhitePointUsable(whitePoint))
        {
            state.whitePoint = whitePoint;
            state.whiteFrame = state.frames;
            state.haveWhitePoint = true;
            return;
        }
    }
    if (shafts::WhitePointStale(state.haveWhitePoint, state.whiteFrame, state.frames))
    {
        state.haveWhitePoint = false;
        state.whitePoint = shafts::kFallbackWhitePoint;
    }
}

// What one frame's reads came to, and what the pass draws with: the newest copy the ring still holds, the last accepted state held across gaps
// with the march's own fade, and a camera cut dropping the hold (a state from before a cut does not belong to the new picture). False when the
// frame has no camera state in it.
bool Readback(ID3D11DeviceContext* context, State& state, float& fade, bool& cameraRefused)
{
    cameraRefused = false;
    const CameraRead read = state.cameraReader.Read(context, state.frames);
    state.cameraAge = read.landed ? shafts::AgeOf(state.frames, read.writtenFrame) : shafts::kRingNotLanded;
    const bool cameraUsable = read.landed && shafts::ProjectionOk(read.projection) && ViewRowsOk(read.view);
    if (read.landed && !cameraUsable)
    {
        // A read that arrived and was refused: the last accepted camera stands while it is young enough (RefusedCameraUsesHold), like a read that has not landed;
        // only a frame with nothing to stand on is a counted refusal.
        const bool holdStands = RefusedCameraUsesHold(state.hold.have, state.hold.Age(state.frames), state.hold.Weight(state.frames));
        if (!state.loggedCameraRefused)
        {
            state.loggedCameraRefused = true;
            LOG_INFO("{}", holdStands ? kCameraRefusedHeld : kCameraRefused);
        }
        if (!holdStands)
        {
            cameraRefused = true;
            return false;
        }
        fade = state.hold.Weight(state.frames);
        return true;
    }
    if (read.landed)
    {
        if (state.haveLastCamera && shafts::CameraCut(state.lastCamera, read.projection))
        {
            state.hold.Drop();
            if (!state.loggedCameraCut)
            {
                state.loggedCameraCut = true;
                LOG_INFO("{}", kCameraCut);
            }
        }
        for (uint32_t i = 0; i < shafts::kProjectionRows; ++i)
            state.lastCamera[i] = read.projection[i];
        state.haveLastCamera = true;
        state.hold.Accept(read, state.frames);
    }
    else if (!state.cameraReader.HaveRead() && state.frames - state.stallBase > kMaxReadbackAge / 2 && !state.loggedStall)
    {
        state.loggedStall = true;
        LOG_INFO("{}", kReadbackNotLanding);
    }
    if (state.cameraReader.HaveRead() && state.frames >= state.cameraReader.LastReadFrame() &&
        state.frames - state.cameraReader.LastReadFrame() > kMaxReadbackAge && !state.loggedStale)
    {
        state.loggedStale = true;
        LOG_INFO("{}", kReadbackStale);
    }
    const uint32_t age = state.hold.Age(state.frames);
    fade = state.hold.Weight(state.frames);
    if (!state.hold.have || age > shafts::kHoldFrames || !(fade > 0.0f))
        return false;
    return true;
}

// The tone map's own shape, measured at the draw (the march's own measurement, this stage's own instance).
shafts::ToneMapFacts MeasureToneMap(ID3D11DeviceContext* context, const State& state, ComPtr<ID3D11ShaderResourceView>& scene)
{
    shafts::ToneMapFacts facts;
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    facts.renderTargets = (views[0] != nullptr ? 1u : 0u) + (views[1] != nullptr ? 1u : 0u);
    if (views[0] != nullptr && views[1] == nullptr)
    {
        ComPtr<ID3D11Resource> resource;
        views[0]->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
            views[0]->GetDesc(&viewDesc);
            facts.targetWidth = desc.Width;
            facts.targetHeight = desc.Height;
            facts.targetFormat = uint32_t(viewDesc.Format);
            facts.targetIsScene = texture.Get() == state.capture.sceneResource;
        }
    }
    for (ID3D11RenderTargetView*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }

    ID3D11ShaderResourceView* first = nullptr;
    context->PSGetShaderResources(0, 1, &first);
    scene.Attach(first);
    if (scene != nullptr)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
        scene->GetDesc(&viewDesc);
        facts.sceneSampled = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        facts.sceneFormat = uint32_t(viewDesc.Format);
        ComPtr<ID3D11Resource> resource;
        scene->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource != nullptr && SUCCEEDED(resource.As(&texture)) && texture != nullptr)
        {
            D3D11_TEXTURE2D_DESC desc {};
            texture->GetDesc(&desc);
            facts.sceneWidth = desc.Width;
            facts.sceneHeight = desc.Height;
            facts.sceneIsScene = texture.Get() == state.capture.sceneResource && state.capture.sceneFrames == state.frames;
        }
    }

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    facts.window = { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height };
    facts.replay = ext::ffxiv::chara::t_replay;
    return facts;
}

// One run of the stage, whichever role reached it: the pass on the scene it was handed, the counters and the one line a session gets.
ID3D11ShaderResourceView* RunStage(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, float fade,
                                   ID3D11ShaderResourceView* scene)
{
    Inputs inputs;
    inputs.scene = scene;
    inputs.depth = state.depth.view.Get();
    inputs.viewPos = state.capture.viewPos.view.Get();
    inputs.normal = state.capture.normal.view.Get();
    inputs.albedo = state.capture.albedo.view.Get();
    inputs.material = state.capture.material.view.Get();
    inputs.sceneFormat = facts.sceneFormat;
    inputs.sceneWidth = facts.sceneWidth;
    inputs.sceneHeight = facts.sceneHeight;
    inputs.depthFormat = state.depth.format;
    inputs.depthWidth = state.depth.width;
    inputs.depthHeight = state.depth.height;
    inputs.window = facts.window;
    std::memcpy(inputs.view, state.hold.view, sizeof inputs.view);
    std::memcpy(inputs.projection, state.hold.projection, sizeof inputs.projection);
    inputs.params = state.params;
    inputs.whitePoint = state.whitePoint;
    inputs.fade = fade;
    inputs.captureStats = ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Gi);

    ID3D11ShaderResourceView* ours = nullptr;
    {
        ext::ffxiv::chara::ReplayScope own;
        ext::ffxiv::engine::OwnWork ownWork;
        ours = state.pass.Run(context, inputs);
    }
    if (ours != nullptr && state.pass.HasCaptureStats())
    {
        // The capture frame only: the outlier share and the mean light, in the log and (the next snapshot of) the stage line.
        state.captureText = FormatCaptureStats(state.pass.LastCaptureStats());
        LOG_INFO("{}", FormatCaptureLine(state.pass.LastCaptureStats()));
    }
    if (ours == nullptr)
    {
        const char* failure = state.pass.Failure();
        if (failure != nullptr && state.log.Take(0x20000001u))
            LOG_INFO("Light bounce: the pass did not run ({})", failure);
    }
    return ours;
}

// The stage after this one (the atmosphere, then depth of field, the chain's last member): composes on this stage's own answer when its chained hook is
// installed, and the stage's answer is what t0 binds. A null from the hook is this stage's own answer unchanged (the stage being off is today's
// picture). The hooks are the chain's own pointers (ChainHooks.h), null in every build that does not link the stage; chainAfterGiScene asks the
// atmosphere when it is linked and depth of field otherwise.
void ComposeChainNext(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView*& ours)
{
    if (ours == nullptr)
        return; // this stage has nothing to hand on: today's picture (a dispatcher that finds no stage linked answers null: the same)
    if (ID3D11ShaderResourceView* composed = ext::ffxiv::engine::chainAfterGiScene(context, facts, ours))
        ours = composed;
}

// The frame's tone map closes the frame (RecogRules.h ClosesFrame): the next frame's first composite then opens the next count. It runs at
// the top of every entry that can see the tone map, before the stage's one-answer gate and whatever the stage then does with the draw, so a
// frame the stage declines or skips still moves the count on.
void CloseFrameAtToneMap(State& state, const shafts::ToneMapFacts& facts, bool chained)
{
    if (shafts::ClosesFrame(facts, chained))
        state.step.CloseAtToneMap();
}

// The one line a live session needs to see the mechanism: how often the count moved for the frame that just closed, and the age of the
// camera copy its read-back took.
void NoteFrameStep(State& state)
{
    if (!state.step.HaveSample())
        return;
    const shafts::RingAge rings[] { { "camera copy", state.cameraAge } };
    const uint32_t advances = state.step.AdvancesPerFrame();
    const uint32_t composites = state.step.CompositesPerFrame();
    if (state.stepLine.Take(shafts::StepKey(advances, composites, rings, 1)))
        LOG_INFO("{}", shafts::FormatFrameStep("Light bounce", advances, composites, rings, 1));
}

// One at-one-tone-map run, either role. The game's draw runs exactly once, whoever owns it: the chained role returns this stage's texture (or
// null, which leaves the chain's hold standing), and the owner role binds its output at t0 for the one draw, and puts the game's own binding
// back, also when that draw throws.
ID3D11ShaderResourceView* StageAtToneMap(State& state, ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                         ID3D11ShaderResourceView* scene, bool owner, DrawFn draw, void* user, bool& drew)
{
    const Outcome declineOutcome = owner ? Outcome::EngineOwn : Outcome::HoldStands;
    // The scene HDR path replays the engine's own tone chain in its own branch, so the scene this stage would shade is not the picture that
    // path presents. The key is read live, so a session that switches HDR off gets the stage back without arming anything again.
    if (shafts::SceneHdrInactive(state.enabled, SceneHdrRequested()))
    {
        NoteDecline(state, Decline::SceneHdr, declineOutcome);
        return nullptr;
    }
    if (facts.replay)
    {
        NoteDecline(state, Decline::CharaReplay, declineOutcome);
        return nullptr;
    }
    const shafts::Decline shape = shafts::ClassifyToneMap(facts);
    if (shape != shafts::Decline::None)
    {
        Decline reason = Decline::NotToneMap;
        if (shape == shafts::Decline::RenderTarget)
            reason = Decline::RenderTarget;
        else if (shape == shafts::Decline::TargetFormat)
            reason = Decline::TargetFormat;
        else if (shape == shafts::Decline::SceneFormat)
            reason = Decline::SceneFormat;
        else if (shape == shafts::Decline::SizeMismatch)
            reason = Decline::SizeMismatch;
        else if (shape == shafts::Decline::Window)
            reason = Decline::Window;
        NoteDecline(state, reason, declineOutcome);
        return nullptr;
    }
    // The frame's state: the newest copy the camera ring still holds, the last accepted state held across gaps.
    float fade = 1.0f;
    bool cameraRefused = false;
    const bool ready = Readback(context, state, fade, cameraRefused);
    NoteFrameStep(state);
    // The three things a frame needs before the stage has anything to add to it: the pass's own views, the scene depth, and the camera. A
    // frame without one of them is a frame without the stage in it, not a refusal (the march's skip discipline; the camera's refusal is, and
    // it is counted).
    if (!state.capture.ViewsComplete())
    {
        NoteSkip(state, SkipReason::NoGBuffer);
        return nullptr;
    }
    if (!state.depth.Present())
    {
        NoteSkip(state, SkipReason::NoDepth);
        return nullptr;
    }
    if (cameraRefused)
    {
        NoteDecline(state, Decline::NoCamera, declineOutcome);
        return nullptr;
    }
    if (!ready)
    {
        NoteSkip(state, SkipReason::NoCamera);
        return nullptr;
    }
    // The depth has to be this frame's scene's own: a depth of another size is another draw's binding, and the rays would end at the wrong
    // surfaces. The same shape check covers the four views the capture kept: one grid, one frame, and a half-written capture is not a frame
    // the pass evaluates.
    if (state.depth.width != facts.sceneWidth || state.depth.height != facts.sceneHeight ||
        state.capture.normal.width != facts.sceneWidth || state.capture.normal.height != facts.sceneHeight ||
        state.capture.albedo.width != facts.sceneWidth || state.capture.albedo.height != facts.sceneHeight ||
        state.capture.material.width != facts.sceneWidth || state.capture.material.height != facts.sceneHeight ||
        state.capture.viewPos.width != facts.sceneWidth || state.capture.viewPos.height != facts.sceneHeight)
    {
        NoteDecline(state, Decline::SizeMismatch, declineOutcome);
        return nullptr;
    }

    ID3D11ShaderResourceView* ours = RunStage(state, context, facts, fade, scene);
    if (ours == nullptr)
    {
        NoteDecline(state, Decline::PassFailed, declineOutcome);
        return nullptr;
    }
    // The capture frame's pictures of this stage: the scene it was handed and the answer it made, copied now (its pass is over and its guards
    // are gone) and before the stage after it composes on the answer.
    ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Gi, context, scene, ours,
                               ext::ffxiv::diag::StageWindow { facts.window.x, facts.window.y, facts.window.width, facts.window.height, state.whitePoint });
    // The frame's white point and the fade this run used (the dump's inputs do not hold them; the replay lab needs them). Capture frame only.
    if (ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Gi))
    {
        ext::ffxiv::diag::StageFrameValues values;
        values.haveWhite = true;
        values.white = state.whitePoint;
        values.fade = fade;
        ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Gi, values);
        // The capture frame's own numbers (the outlier control's share of the traced texels, the filtered bounce's mean light), set by RunStage for
        // this run: on the line through the late hook, since the values snapshot is read before the stage runs.
        if (!state.captureText.empty())
            ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Gi, state.captureText);
    }

    // Answered: the counters and the one line a session gets. The chained role hands its texture back for the caller to bind; the owner role
    // binds it here, for exactly one draw, and puts the game's own binding back.
    state.runs++;
    if (fade < 1.0f)
        state.heldRuns++;
    state.lastWindowWidth = uint32_t(facts.window.width);
    state.lastWindowHeight = uint32_t(facts.window.height);
    state.lastGiWidth = GiExtent(facts.window.width, state.params.resolution);
    state.lastGiHeight = GiExtent(facts.window.height, state.params.resolution);
    state.session.NoteRun(Knob::Gi);
    if (state.log.Take(0x20000002u | ((facts.sceneWidth & 0x7FFFu) << 11)))
        LOG_INFO("{}", FormatRun(state.params, state.lastWindowWidth, state.lastWindowHeight, state.lastGiWidth, state.lastGiHeight, fade,
                                 state.whitePoint));
    // This stage's own answer is counted above; the stage after it (depth of field) composes on it now when its chained hook is installed, and
    // a null from it is this stage's own answer unchanged. The composition runs in both roles, so the owner's bind and the chained return both
    // carry the stage after this one's answer when it has one.
    ComposeChainNext(context, facts, ours);
    if (!state.loggedCost && state.pass.HasGpuMilliseconds())
    {
        state.loggedCost = true;
        LOG_INFO("{}", FormatCost(!owner, state.pass.GpuMilliseconds(), state.frames, state.runs, state.declined));
    }
    if (!owner)
        return ours;

    // The owner's one bind: this stage's copy of the scene at t0 for the one draw, and the game's own view back afterwards, also when that
    // draw throws. The chained role's caller does the same for its own texture.
    ID3D11ShaderResourceView* original = nullptr;
    context->PSGetShaderResources(0, 1, &original);
    context->PSSetShaderResources(0, 1, &ours);
    struct Restore
    {
        ID3D11DeviceContext* context = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        ~Restore()
        {
            context->PSSetShaderResources(0, 1, &view);
            if (view != nullptr)
                view->Release(); // the reference the read above took
        }
    } restore { context, original };
    draw(user);
    drew = true;
    return ours;
}

// The stage as the chain's fallback owner: the entry the shafts' chain and the seam's own call reach when neither earlier stage nor the march
// answered.
bool RunToneMapAsOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    State& state = S();
    bool drew = false;
    try
    {
        LogCountsIfDue(state);
        StageAtToneMap(state, context, facts, scene, true, draw, user, drew);
    }
    catch (...)
    {
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
        if (!drew)
            draw(user); // the game's draw, once, and not twice
        return true;
    }
    if (drew)
        return true;
    // This stage's own decline is the chain's next stage's owner turn: the atmosphere (else depth of field) owns the game's draw on exactly the
    // contract this entry carries (true only when it drew; its own declines stay its own, and false leaves the game's draw to the caller). It is asked
    // outside the try on purpose: that stage catches everything itself, and a throw after it drew must not reach this stage's catch, which would draw
    // the game's call a second time.
    return ext::ffxiv::engine::toneMapAfterGiOwner(context, facts, scene, draw, user);
}

// The stage as a chain member: the entry the march's stage calls with the chain's current output. A texture comes back for the caller to bind
// at t0; null means the stage has nothing for this frame, and the caller's own texture - the chain's last good picture - stays at t0 (the hold
// stands, and no unlit scene is ever bound over it).
ID3D11ShaderResourceView* ChainSceneInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts,
                                             ID3D11ShaderResourceView* scene)
{
    State& state = S();
    if (context == nullptr || scene == nullptr)
        return nullptr;
    ID3D11ShaderResourceView* ours = nullptr;
    // The close needs no armed test: a stage that is off never opens a count, so its frame is closed already and the close is a no-op.
    CloseFrameAtToneMap(state, facts, true);
    // This stage's own armed test and its own one-answer gate guard only this stage's attempt: the stage after it (depth of field) is reachable
    // whether this stage is off, has answered the frame already, declines or throws.
    if (ArmedInternal(state) && !(state.haveGiFrame && state.giFrame == state.frames))
    {
        if (!state.haveRenderThread)
        {
            state.renderThread = std::this_thread::get_id();
            state.haveRenderThread = true;
        }
        // The chained role answers at most one tone map a frame, by this stage's own counter: a second tone-shaped draw of the frame is the
        // engine's own post-tone copy, and the game's own call (the march's own rule).
        state.haveGiFrame = true;
        state.giFrame = state.frames;
        try
        {
            bool drew = false; // the chained role never draws: the caller binds and draws
            ours = StageAtToneMap(state, context, facts, scene, false, nullptr, nullptr, drew);
        }
        catch (...)
        {
            LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::HoldStands));
            ours = nullptr; // the chain's hold stands
        }
    }
    // This stage answered: the stage after it was already handed that answer inside the stage (ComposeChainNext), so its result is final.
    if (ours != nullptr)
        return ours;
    // This stage has no answer - its key is off, it answered this frame already, it declined or it threw - and the chain's hold stands. Every
    // one of those exits hands the stage after it (ChainHooks.h's chainAfterGiScene: the atmosphere, else depth of field) the texture this call was
    // given, the earlier stages' picture, so that stage is reachable with the march and this stage off; it applies its own armed test and its own
    // one-answer gate. A null from it is this stage's null: the caller's texture stays at t0.
    return ext::ffxiv::engine::chainAfterGiScene(context, facts, scene);
}

// The stage as the chain's fallback owner, through the chain's own hook: the march's fallback-owner entry reaches here when the march declined
// and the stage is armed.
bool ToneMapOwnerInternal(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw,
                          void* user)
{
    State& state = S();
    // Both early exits hand the stage after this one (ChainHooks.h's toneMapAfterGiOwner: the atmosphere, else depth of field) the game's scene: this
    // stage is off by default, and the chain's fallback-owner branch reaches this entry whether it is armed or not, so a stage placed after these
    // returns would never run. The stage after owns the game's draw on exactly the contract this entry carries (true only when it drew; false obliges
    // the caller to draw the game's call once) and applies its own armed test and its own one-answer gate. With it not linked both exits are today's: false.
    if (context == nullptr || scene == nullptr || draw == nullptr || !ArmedInternal(state))
        return ext::ffxiv::engine::toneMapAfterGiOwner(context, facts, scene, draw, user);
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    CloseFrameAtToneMap(state, facts, true);
    if (state.haveGiFrame && state.giFrame == state.frames)
    {
        // this frame's tone map was already answered, by this stage
        return ext::ffxiv::engine::toneMapAfterGiOwner(context, facts, scene, draw, user);
    }
    state.haveGiFrame = true;
    state.giFrame = state.frames;
    return RunToneMapAsOwner(context, facts, scene, draw, user);
}

// Whether the stage wants to see a draw at all: the key is on, the strength is above zero, the module has not stood down, and the scene HDR
// path is off (read live, so switching it never leaves the stage armed and inert).
bool ArmedInternal(State& state)
{
    if (!Wanted(state.enabled, state.params) || !state.session.Armed())
        return false;
    return !shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
}

// The engine's per-frame sample of the scene HDR state, the march's own pattern: the HDR path takes every game draw before the draw seam hands
// this stage one, so the sample is taken on a pixel-shader bind, once an output frame.
void SampleSceneHdrInternal(State& state)
{
    const uint64_t token = ext::present::FrameCount();
    if (state.haveHdrSample && state.hdrSampleFrame == token)
        return;
    state.haveHdrSample = true;
    state.hdrSampleFrame = token;
    if (state.hdr.Sample(state.enabled, SceneHdrRequested()))
        LOG_INFO("{}", kSceneHdrInactive);
}

// The seam's own entry: the pass's views are kept at the composite, the depth at the fog post, and the tone map's fallback owner when neither
// earlier stage nor the march took the draw.
bool OnSeamDrawInternal(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    State& state = S();
    if (draw == nullptr)
        return false;
    if (!ArmedInternal(state) || context == nullptr)
        return false;
    if (!state.haveRenderThread)
    {
        state.renderThread = std::this_thread::get_id();
        state.haveRenderThread = true;
    }
    try
    {
        if (ext::ffxiv::engine::IsShaftsCompositeAnchors(anchors))
        {
            CaptureComposite(context, state);
            return false; // the game's own draw still has to run: the engine module's draw path runs it
        }
        if (ext::ffxiv::engine::IsFogPostAnchors(anchors))
        {
            CaptureDepthAtFogPost(context, state);
            return false;
        }
        if (ext::ffxiv::engine::IsShaftsToneMapAnchors(anchors))
        {
            // The shafts, the puddles' stage and the march had their chances above this entry in the dispatch; reaching here with a tone map
            // means none took it, and this stage is the chain's fallback owner. The shape is measured here, and a draw that is not the tone
            // map is the game's own call.
            ComPtr<ID3D11ShaderResourceView> scene;
            const shafts::ToneMapFacts facts = MeasureToneMap(context, state, scene);
            if (shafts::ClassifyToneMap(facts) == shafts::Decline::NotToneMap)
                return false;
            CloseFrameAtToneMap(state, facts, false);
            if (state.haveGiFrame && state.giFrame == state.frames)
                return false;
            state.haveGiFrame = true;
            state.giFrame = state.frames;
            return RunToneMapAsOwner(context, facts, scene.Get(), draw, user);
        }
    }
    catch (...)
    {
        // Fail open: the engine module's draw path runs the game's draw, unchanged.
        LOG_INFO("{}", FormatDecline(Decline::Exception, Outcome::EngineOwn));
    }
    return false;
}

// The keys and the feature bit. The switch-off is the march's own: the strength at 0 puts the draw path back to two bit tests.
void ApplyInternal(bool on)
{
    State& state = S();
    auto& cfg = ext::Cfg().ffxiv;
    state.enabled = on;
    state.params = BuildParams(cfg.giRays.value_or_default(), cfg.giSteps.value_or_default(), cfg.giRange.value_or_default(),
                               cfg.giStrength.value_or_default(), cfg.giResolution.value_or_default());
    if (state.hdr.Sample(state.enabled, SceneHdrRequested()))
        LOG_INFO("{}", kSceneHdrInactive);
    if (!on)
    {
        state.session = ext::ffxiv::engine::Session {};
        state.log = LogGate {};
        state.loggedOff = false;
        state.loggedCost = false;
        state.loggedCameraRefused = false;
        state.loggedStale = false;
        state.loggedStall = false;
        state.loggedCameraCut = false;
        state.loggedComposite = false;
        state.loggedDepth = false;
        state.capture.Reset();
        state.depth.Clear();
        state.cameraReader.Reset();
        state.stallBase = state.frames;
        state.tone.Reset();
        state.step.Reset();
        state.stepLine = shafts::ChangeLine {};
        state.cameraAge = shafts::kRingNotLanded;
        state.haveGiFrame = false;
        state.giFrame = 0;
        state.hold = CameraHold {};
        state.haveLastCamera = false;
        state.haveWhitePoint = false;
        state.whiteFrame = 0;
        state.whitePoint = shafts::kFallbackWhitePoint;
        state.nextCountsLog = 0;
        state.runs = 0;
        state.heldRuns = 0;
        state.declined = 0;
        state.refusals = 0;
        state.skips = CardCounts {};
        state.cameraRefused = false;
    }
    else if (state.session.off)
    {
        state.session = ext::ffxiv::engine::Session {}; // the player turned the switch back on: the counters and the streaks start again
        state.nextCountsLog = 0;
    }
    const bool wanted = Wanted(state.enabled, state.params) && state.session.Armed();
    ext::SetActive(ext::F::Gi, wanted);
    if (!wanted || state.hdr.inactive)
    {
        state.pass.Reset();
        state.capture.Reset();
        state.depth.Clear();
        state.cameraReader.Reset();
        state.stallBase = state.frames;
        state.tone.Reset();
    }
}

void ReleaseInternal(State& state)
{
    state.pass.Reset();
    state.capture.Reset();
    state.depth.Clear();
    state.cameraReader.Reset();
    state.stallBase = state.frames;
    state.tone.Reset();
}
} // namespace

// ---- the module's exports (Gi.h), each forwarding to the frame-local state ------------------------------------------------

bool Armed()
{
    State& state = S();
    return ArmedInternal(state);
}

void SampleSceneHdr()
{
    State& state = S();
    SampleSceneHdrInternal(state);
}

bool OnSeamDraw(ID3D11DeviceContext* context, engine::Anchors anchors, DrawFn draw, void* user)
{
    return OnSeamDrawInternal(context, anchors, draw, user);
}

Status CurrentStatus()
{
    State& state = S();
    Status status;
    status.enabled = state.enabled;
    status.armed = ArmedInternal(state);
    status.hdrInactive = shafts::SceneHdrInactive(state.enabled, SceneHdrRequested());
    status.strength = state.params.strength;
    status.frames = state.frames;
    status.runs = state.runs;
    status.held = state.heldRuns;
    status.declined = state.declined;
    status.skipped = state.skips.skipped;
    status.noGBuffer = state.skips.noGBuffer;
    status.noDepth = state.skips.noDepth;
    status.noCamera = state.skips.noCamera;
    status.refused = state.refusals;
    status.windowWidth = state.lastWindowWidth;
    status.windowHeight = state.lastWindowHeight;
    status.giWidth = state.lastGiWidth;
    status.giHeight = state.lastGiHeight;
    return status;
}

void Publish() { ApplyInternal(ext::Cfg().ffxiv.gi.value_or_default()); }

void OnConfigLoaded(bool on) { ApplyInternal(on); }

void Release()
{
    State& state = S();
    ReleaseInternal(state);
}

// The chained role, behind the march's stage: the entry the march's chain functions call with the chain's current output (ChainHooks.h's own
// hook pointer, this module's registration at start-up).
ID3D11ShaderResourceView* ChainScene(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    try
    {
        return ChainSceneInternal(context, facts, scene);
    }
    catch (...)
    {
        return nullptr; // nothing escapes into the march's entries: the chain's hold stands
    }
}

// The fallback-owner role, behind the march's stage: the entry the march's owner entry reaches when it declined (ChainHooks.h's hook pointer,
// this module's registration at start-up).
bool ToneMapOwner(ID3D11DeviceContext* context, const shafts::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, DrawFn draw, void* user)
{
    // The owner contract: true only when this stage drew the game's call. The internals catch around everything that can run after the draw
    // (RunToneMapAsOwner); an exception that gets out of them happened before it, so false (the caller draws once) is the honest answer.
    try
    {
        return ToneMapOwnerInternal(context, facts, scene, draw, user);
    }
    catch (...)
    {
        return false;
    }
}

// The composite-shader stamp: the five slots the pass's family needs, packed once at creation and read at the frame's composite. A shader whose
// reflection does not name all five takes no stamp (0 is "not this stage's shader"): a draw that binds four of the frame's five members is a
// draw that is not the composite, and guessing the fifth's slot would be the wrong-picture failure no fail-open catches.
void NoteShader(ID3D11DeviceChild* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    try
    {
        StampShader(shader, PackStamp(StampOfShader(code, size)));
    }
    catch (...)
    {
    }
}

// The stage dump's two questions about this stage, asked at the capture frame: is it armed, and what are its values.
std::string StageValues()
{
    const Params& params = S().params;
    return std::format("rays={} steps={} range={:.2f} strength={:.2f}", params.rays, params.steps, double(params.range), double(params.strength));
}

namespace
{
// The registration of the config hook and the chain's stage hooks, set when this DLL's static initialisers run: the [FFXIV] Gi key arms the
// seam's feature bit at every config load, and the march's stage reaches this stage through the pointers the chain's own header declares (null
// in every build that does not link this file, which is this stage being off - the chain runs unchanged without it).
struct RegisterGi
{
    RegisterGi()
    {
        ext::FfxivConfig::giOnLoaded = &ext::ffxiv::engine::gi::OnConfigLoaded;
        ext::ffxiv::engine::chainGiScene = &ext::ffxiv::engine::gi::ChainScene;
        ext::ffxiv::engine::toneMapGiOwner = &ext::ffxiv::engine::gi::ToneMapOwner;
        ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::Gi, &ext::ffxiv::engine::gi::Armed, &ext::ffxiv::engine::gi::StageValues);
    }
} g_registerGi;
} // namespace
} // namespace ext::ffxiv::engine::gi
