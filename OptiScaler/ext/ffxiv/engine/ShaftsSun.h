#pragma once
// Light shafts before the tone map: the frame's light and camera bytes (ext/ffxiv/engine/ShaftsSun.cpp).
//
// The sun draw binds `g_LightParam` (the light's position, direction and colour) and `g_CameraParameter` (the camera the pass projects with) on
// the game's own context, and this module reads a small range of each on the CPU. Mapping those buffers at the draw that binds them would wait
// for the GPU, so the copy is issued at the draw and read at the tone map of a later frame from a ring of kStagingRing staging pairs. Each read
// tries the newest copy the ring still holds (the frame before this one) and an older one when that has not landed, so a GPU a frame or two
// behind costs a later read rather than a stall, and the last good values stay until a newer read lands.
//
// The class is device work only (no pch, no product state), so tests/ext/ffxiv/engine drives it on a software adapter with real buffers.

#include "ext/ffxiv/engine/ShaftsRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::shafts
{
// What one read of the ring came to. Nothing here is a refusal of the feature: a not-ready copy is a GPU that is behind, and a camera buffer
// that does not hold this build's camera is reported so the caller can name that reason.
struct SunRead
{
    bool light = false;              // the light's bytes were mapped
    bool projection = false;         // the camera's bytes were mapped, and they hold this build's projection, view and view-projection
    bool notReady = false;           // a copy the read was allowed to take had not landed (the GPU is behind)
    bool projectionRefused = false;  // the camera's bytes arrived and are not this build's camera
};

class SunReader
{
  public:
    // Copies this frame's light, camera and directional-shadow bytes into the ring slot for `frame`. `shadows` are the light draw's 128-byte
    // constant buffers (the frame's `g_DirectionalShadowParameter` among them); the read takes the row at +0x20 of each of them and the rules
    // pick the one that is the light's own axis (ShaftsRules.h JudgeShadow). False when the draw bound no light or camera buffer, or the staging
    // pair could not be created; a shadow candidate that cannot be copied only leaves fewer rows to judge.
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* light, ID3D11Buffer* camera, uint64_t frame,
              ID3D11Buffer* const* shadows = nullptr, uint32_t shadowCount = 0);

    // Reads the newest copy the ring still holds of the frames before `frame` (`ReadCandidate`: age kNewestReadAge up to the ring's depth),
    // newest first and without waiting for the GPU. A copy that is still in the GPU's queue is tried again on a later frame while the ring
    // holds it, and an older copy is taken when the newer one has not landed - but never one older than the last a read took, so the values
    // only ever move forward. Reads nothing when the ring holds no such copy (the frames that wrote them did not draw the sun): a read that
    // has nothing to say is not a not-ready one.
    //
    // `block` waits for the copy instead. Only a test asks for it: a software adapter never satisfies a no-wait map, so the test that checks which
    // frame's bytes come back has to let the copy finish. The product never sets it.
    SunRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    bool HaveFacts() const noexcept { return _haveFacts; }
    const LightFacts& Facts() const noexcept { return _facts; }
    // The camera's own matrices, from the same copy as the light's bytes: the projection the light's view-space direction is projected through,
    // the view whose second row is the world's up in view space (the direction's elevation, and the world vector the frame's shadow carries), and
    // the view-projection, whose fourth row is the camera's own forward (what a held sun is dropped on).
    bool HaveProjection() const noexcept { return _haveProjection; }
    const float* ViewProjection() const noexcept { return _viewProjection; }
    const float* Projection() const noexcept { return _projection; }
    const float* View() const noexcept { return _view; }
    bool HaveView() const noexcept { return _haveView; }

    // The camera's bytes arrived and do not hold this build's camera: the counted reason a session reads when a build moved it. A camera map
    // that has not landed yet is not this, so a GPU that is behind never counts against the module.
    bool ProjectionRefused() const noexcept { return _projectionRefused; }

    // The game's own buffer the last successful read came from: a light the rules then refuse is remembered by it, so the frames after it look
    // at the next light draw instead of the same one.
    const void* LastLight() const noexcept { return _lastLight; }

    // The directional-shadow candidates of the copy the last read took: `ShadowCount()` of them were mapped and kept, each with the shadow matrix's
    // own shape (ShaftsRules.h ShadowMatrixShape) and its third row. They belong to the same copy as the light and the camera, so a light and a
    // shadow compared by the rules are one frame's.
    uint32_t ShadowCount() const noexcept { return _shadowCount; }
    const ShadowCandidate* Shadows() const noexcept { return _shadowCandidates; }

    bool HaveRead() const noexcept { return _haveRead; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }
    uint32_t Reads() const noexcept { return _reads; }
    uint32_t NotReady() const noexcept { return _notReady; }

    // Drops what was read (a light that turned out not to be the sun), keeping the counters and the frame the last read took. The copy the read took
    // stops being the oldest one a read may take: the light was refused, not accepted, so the next frame's read may take whichever age the ring still
    // holds - otherwise a refused read costs every frame until a copy two or three frames old has landed, which is half of a session's skipped frames.
    void Forget();

    // Drops the facts and the staging pairs (a device change, the end of the session).
    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> light;
        Microsoft::WRL::ComPtr<ID3D11Buffer> camera;
        Microsoft::WRL::ComPtr<ID3D11Buffer> shadow[kShadowCandidates];
        uint32_t shadowCount = 0;
        const void* source = nullptr; // the game's g_LightParam this slot was copied from
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[kStagingRing];
    bool _haveFacts = false;
    bool _haveProjection = false;
    bool _haveRead = false;
    bool _projectionRefused = false;
    LightFacts _facts;
    float _viewProjection[kProjectionRows] {};
    float _projection[kProjectionRows] {};
    float _view[kViewRows] {};
    bool _haveView = false;
    ShadowCandidate _shadowCandidates[kShadowCandidates] {};
    uint32_t _shadowCount = 0;
    const void* _lastLight = nullptr;
    bool _haveAccepted = false;      // a read has taken a copy: only a newer one may replace it
    uint64_t _lastAcceptedFrame = 0; // the frame that copy belongs to
    uint64_t _lastReadFrame = 0;
    uint32_t _reads = 0;
    uint32_t _notReady = 0;
};
// One read of the tone map's own parameters (`cCommonTexParam`), which is where the frame's white point comes from: its second float is the
// engine's exposure multiplier, and the scene value that reaches display-white is its reciprocal (ShaftsRules.h WhitePointFromExposure). The copy is
// taken at the tone map draw, where the buffer is bound, and read at a later frame's tone map from a ring of kStagingRing staging buffers - one
// frame's copy is written while an older one is mapped, so a single buffer would overwrite the bytes before they could be read, and a GPU two or
// three frames behind would never answer a map of one fixed age (the same reason the light's ring has four slots). The map never waits.
struct ToneRead
{
    bool landed = false;   // the copy was mapped
    float exposure = 0.0f; // the exposure multiplier it reported (0 when the bytes were not a number this pass can use)
};

class ToneReader
{
  public:
    // Copies this frame's tone map parameters into the ring. False when the draw bound no such buffer or the staging buffer could not be made.
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* params, uint64_t frame);

    // Maps the newest copy the ring still holds of the frames before this one, without waiting - the same age rule the light's ring uses
    // (ReadCandidate): the frame before this one first, an older copy when that has not landed, never one older than the last copy a read took.
    // False (`landed`) when none has landed yet: the last good white point stays, and the frame draws with it - the exposure moves over seconds,
    // not frames.
    ToneRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    void Reset();

    // The rendered frame the last landed copy was written at: a white point that has not landed for a while is not this frame's exposure any
    // more (ShaftsRules.h WhitePointStale).
    bool HaveRead() const noexcept { return _haveRead; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[kStagingRing];
    bool _haveRead = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
};

// One read of the sky's own sun parameters (`cSunParam`), which is where the sun a player sees is drawn: its first row carries the disk's screen
// UV. The copy is taken at that draw and read at a later frame's tone map from a ring of kStagingRing staging buffers - the same four-slot age rule
// the tone map's parameters use, because a copy is never mapped at the frame that made it. The map never waits.
struct SunUvRead
{
    bool landed = false;       // the copy was mapped
    uint64_t writtenFrame = 0; // the rendered frame the copy was written at
    float uv[4] {};            // its first row: (aspect, 1.0, sunUvX, sunUvY), which SunParamUvUsable judges
};

class SunUvReader
{
  public:
    // Copies this frame's sun parameters into the ring. False when the draw bound no such buffer or the staging buffer could not be made.
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* params, uint64_t frame);

    // Maps the newest copy the ring still holds of the frames before this one, without waiting - the same age rule the tone map's ring uses.
    // False (`landed`) when none has landed yet: the sun the pass last placed the shafts with keeps its place until a newer read arrives.
    SunUvRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    void Reset();

    bool HaveRead() const noexcept { return _haveRead; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[kStagingRing];
    bool _haveRead = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
};
} // namespace ext::ffxiv::shafts
