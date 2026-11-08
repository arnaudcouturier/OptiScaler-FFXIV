#pragma once
// God rays: the volumetric march (ext/ffxiv/engine): the frame's captures (GodraysSun.cpp).
//
// The march reads five things a frame, all copied GPU side at the draws that bind them and mapped at a later frame's tone map, never
// waiting for the GPU:
//  - the light's parameters and the camera's matrices, at the frame's sun draw (the same draws the radial shafts capture; the march
//    instantiates its own readers, so it works with the radial off);
//  - the fog state (cFogParam, cExpHeightFogParam and the viewport buffer), the scene depth and the sky/LUT views, at the engine's fog
//    post draw;
//  - the four per-cascade shadow parameter instances and the atlas view, at the assembly draws that sample the atlas;
//  - the tone map's own exposure multiplier, at the tone map (the frame's white point).
//
// The views the march steals (the atlas, the sky, the LUT) are secured once: an own SRV is created on the game's resource after its
// description is checked for the shader-resource bind flag (the SSR/AO rule), and an owned copy is made when the check refuses. Nothing
// of the game is ever written.
//
// The classes are device work only (no product state), so tests/ext/ffxiv/engine drives them on a software adapter with real buffers.

#include "ext/ffxiv/engine/GodraysRules.h"
#include "ext/ffxiv/engine/ShaftsSun.h" // the ring cadence and the tone map's reader, reused as types

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::godrays
{
// One frame's light and camera, read back from the ring: the light's facts (the rules judge them), the inverse projection the march's
// rays are reconstructed through, the inverse view's second row (the world y of a view point), the projection (the sun's screen position
// for the auto colour) and the uv mapping row the engine's own fog post uses. Everything comes from one copy, so a light and a camera
// compared by the rules are one frame's.
struct FrameRead
{
    bool landed = false;          // the light's bytes were mapped
    bool camera = false;          // the camera's bytes were mapped, and they hold this build's camera
    bool notReady = false;        // a copy the read was allowed to take had not landed (the GPU is behind)
    shafts::LightFacts facts;
    float inverseProjection[16] {};
    float inverseViewRow1[4] {};
    float projection[16] {};
    float view[shafts::kViewRows] {}; // the view matrix's three rows, from the same copy: the elevation's world up and the held light's world direction
    bool haveView = false;
    uint64_t writtenFrame = 0;    // the rendered frame the copy belongs to
};

// The light and camera reader: one staging pair per ring slot (the ShaftsSun pattern, the march's own instance), the whole camera read
// so the inverse projection and the inverse view's row are in the same copy as the light the rules judge.
class FrameReader
{
  public:
    // Copies this frame's light and camera bytes into the ring slot for `frame`. False when the draw bound no light or camera buffer,
    // or the staging pair could not be created.
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* light, ID3D11Buffer* camera, uint64_t frame);

    // Reads the newest copy the ring still holds of the frames before this one, newest first and without waiting for the GPU (the same
    // age rule the radial's readers use). `block` waits instead; only a test asks for it.
    FrameRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    bool HaveRead() const noexcept { return _haveRead; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }
    uint32_t Reads() const noexcept { return _reads; }
    uint32_t NotReady() const noexcept { return _notReady; }
    // The game's own light buffer the last successful read came from: a light the rules then refuse is remembered by it, so the frames
    // after it look at the next light draw instead of the same one (the radial's own rule).
    const void* LastLight() const noexcept { return _lastLight; }
    // Whether the last read's camera bytes arrived and were not this build's camera: a build mismatch, counted toward the stand-down.
    bool LastCameraRefused() const noexcept { return _lastCameraRefused; }
    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> light;
        Microsoft::WRL::ComPtr<ID3D11Buffer> camera;
        const void* source = nullptr;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[shafts::kStagingRing];
    bool _haveRead = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
    uint32_t _reads = 0;
    uint32_t _notReady = 0;
    const void* _lastLight = nullptr;
    bool _lastCameraRefused = false;
};

// One frame's fog state, read back from the ring: the decoded FogState and the viewport buffer's second row (logged, never used to march),
// from the copies the fog post's draw made.
struct FogRead
{
    bool landed = false;
    bool usable = false;          // the profile passed ProfileRowUsable and the decode passed FogStateUsable
    bool profile = false;         // the copy held a height profile (the draw bound a buffer at the stamped slot)
    bool profileRefused = false;  // ... and its declared 48 bytes are not a height profile (the wrong buffer was bound there)
    float profileFields[kProfileFieldCount] {}; // the eleven fields the profile check judged (filled whenever a profile copy landed): the refusal line names them
    FogState fog;
    // The fog post's own sun (cFogParam[2].xyz, world space), decoded from the 128-byte copy ALONE: it needs neither the profile nor a usable march
    // state, so the night curve can read it on the frames where "cb2 is not a height profile".
    bool sunLanded = false;
    float sun[3] {};
    bool viewportLanded = false;  // the 32-byte viewport buffer's copy landed with this read
    float ndcScale[2] {};         // that buffer's second float4, raw: logged next to the mapping the march derives, never used to march
    float ndcOffset[2] {};
    uint64_t writtenFrame = 0;
};

// The fog reader: three staging buffers per ring slot (the fog constants, the height profile and the viewport buffer), copied at the fog
// post's draw. The height profile is the one buffer the caller picked by the slot the fog shader names it at (GodraysRules.h
// ProfileSlotFor); the reader does not look for it, and the read checks the copy's declared 48 bytes before it decodes it.
class FogReader
{
  public:
    // Copies this frame's fog bytes into the ring slot for `frame`. `profile` is the draw's buffer at the profile's slot, or null when the
    // draw has none (the read then reports no profile). False when the draw bound no fog buffer or the staging buffers could not be created.
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* fogParam, ID3D11Buffer* profile, ID3D11Buffer* viewportParam, uint64_t frame);

    FogRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    bool HaveRead() const noexcept { return _haveRead; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }
    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> fogParam;
        Microsoft::WRL::ComPtr<ID3D11Buffer> profile;
        Microsoft::WRL::ComPtr<ID3D11Buffer> viewport;
        bool hasProfile = false;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[shafts::kStagingRing];
    bool _haveRead = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
};

// The stats texture's read-back: the pass's 2x1 RGBA32F stats (sky ceilings, the term's peak, the sky sample count) copied into a small staging ring at the
// end of a run and mapped a frame or two later without waiting for the GPU. The values are for the stage dump (the term's peak the run line used to print as
// a literal 0) and for tests; the composite reads the texture itself.
class StatsReader
{
  public:
    // Copies the pass's stats texture into the ring slot for `frame`. False when the texture is not a 2x1 RGBA32F or the staging could not be created.
    bool Copy(ID3D11DeviceContext* context, ID3D11Texture2D* stats, uint64_t frame);

    // Reads the newest copy the ring still holds of the frames before this one, newest first and without waiting. `block` waits instead; only a test asks.
    StatsRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[shafts::kStagingRing];
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
};

// The four per-cascade shadow parameter instances of one frame, read back from the ring: pointer-keyed per frame (one copy per distinct
// buffer the assembly draws bound, four at most), so a window's four cascades stay apart the way the diag's own cadence keeps them.
struct CascadeRead
{
    bool landed = false;
    uint32_t count = 0; // the distinct instances this frame's copies hold, decoded
    Cascade cascades[kCascadeCount] {};
    uint64_t writtenFrame = 0;
};

class CascadeReader
{
  public:
    // Copies one 128-byte parameter buffer into this frame's set, keyed by the buffer's own pointer: a buffer already copied this frame
    // is skipped, so the 20 assembly draws of a frame cost four copies among them. False when the set is full or the staging buffer
    // could not be created.
    bool Copy(ID3D11DeviceContext* context, ID3D11Buffer* shadow, uint64_t frame);

    CascadeRead Read(ID3D11DeviceContext* context, uint64_t frame, bool block = false);

    bool HaveRead() const noexcept { return _haveRead; }
    uint64_t LastReadFrame() const noexcept { return _lastReadFrame; }
    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging[kCascadeCount];
        const void* source[kCascadeCount] {};
        uint32_t count = 0;
        uint64_t frame = 0;
        bool written = false;
    };

    Slot _ring[shafts::kStagingRing];
    bool _haveRead = false;
    bool _haveAccepted = false;
    uint64_t _lastAcceptedFrame = 0;
    uint64_t _lastReadFrame = 0;
};

// The views the march steals, secured once per source: an own SRV on the game's resource after the resource's own description is checked
// for the shader-resource bind flag (the SSR/AO rule), else an owned copy of the same description. The game's resource is only ever read.
class StolenViews
{
  public:
    // Secures the atlas (a typeless plane the engine samples as R16_UNORM), the sky (FP16) or the weight LUT (R16G16) from the SRV the
    // draw bound. Returns the secured view, or null when the source could not be secured (the march then skips, one line says so).
    // The context is needed for the owned-copy fallback (CopySubresourceRegion is a context method).
    ID3D11ShaderResourceView* SecureAtlas(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, bool& ownView, uint32_t& width,
                                          uint32_t& height, uint32_t& format);
    ID3D11ShaderResourceView* SecureSky(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, bool& ownView, uint32_t& width,
                                        uint32_t& height, uint32_t& format);
    ID3D11ShaderResourceView* SecureLut(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, bool& ownView, uint32_t& width,
                                        uint32_t& height, uint32_t& format);

    // The scene depth, as the fog post binds it: an own view is not needed (the pass reads it for one draw inside its own state), so the
    // reference is kept the way the radial keeps the sun draw's depth.
    void KeepDepth(ID3D11ShaderResourceView* source, uint32_t format, uint32_t width, uint32_t height);
    ID3D11ShaderResourceView* Depth() const noexcept { return _depth.Get(); }
    uint32_t DepthFormat() const noexcept { return _depthFormat; }
    uint32_t DepthWidth() const noexcept { return _depthWidth; }
    uint32_t DepthHeight() const noexcept { return _depthHeight; }

    // The secured views, for the march's inputs (null until the capture that secures them has run).
    ID3D11ShaderResourceView* AtlasView() const noexcept { return _atlas.Get(); }
    ID3D11ShaderResourceView* SkyView() const noexcept { return _sky.Get(); }
    ID3D11ShaderResourceView* LutView() const noexcept { return _lut.Get(); }

    void Reset();

  private:
    ID3D11ShaderResourceView* Secure(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, uint32_t viewFormat, bool& ownView,
                                     uint32_t& width, uint32_t& height, uint32_t& format, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view,
                                     Microsoft::WRL::ComPtr<ID3D11Texture2D>& copy);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _atlas, _sky, _lut;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _atlasCopy, _skyCopy, _lutCopy;
    const void* _atlasSource = nullptr;
    const void* _skySource = nullptr;
    const void* _lutSource = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _depth;
    uint32_t _depthFormat = 0, _depthWidth = 0, _depthHeight = 0;
};
} // namespace ext::ffxiv::godrays
