#pragma once
// Puddles, the tone-map stage: the pass itself (ext/ffxiv/engine/shaders/PuddleReflection.hlsl).
//
// One draw on the game's own immediate context, inside a private device-context state (the pattern the fork's other passes use, so no game
// binding is touched by it): over the part of the scene that lies inside the render window, it reads the frame's view-space positions, its
// G-buffer normal, the camera table and the engine's wetness amount, and writes this module's own copy of the scene with the pool's lit colour
// darkened and a marched reflection of the scene added to it.
//
// The owned target starts as a copy of the engine's own scene, so every texel outside the region keeps the engine's value: the tone map samples
// the whole texture and this module only answers for the part the engine's own composite wrote. The draw's viewport is exactly the window's whole
// texels, so the copy is the one-texel ring around the window - the texels the tone map's own filter reads at the window's edge - and never the
// window's sixteen megabytes; the texture is cleared once when it is created, so a copy that does not land reads as black rather than as the
// previous frame's picture.
//
// The scene it reads is the caller's: the engine's own scene at the tone map, or the light shafts' air-lit copy when that module already owns the
// draw. Either way this pass writes a texture of its own, because a resource cannot be a shader resource and a render target in one draw; what the
// chained order saves is the window's copy, not the texture.
//
// The camera table (g_CameraParameter, 0x160 bytes through the projection) is copied GPU side into a buffer of this pass with a partial
// CopySubresourceRegion: nothing is ever read back to the CPU on this path. Two reads reach the CPU without waiting for the GPU: the pass's own
// GPU time, from a timestamp pair a run later, and - while a draw-diagnostics capture is armed only - the pool and march-hit shares, from a
// no-wait map of an earlier frame's copy of a per-texel statistics target.
//
// Everything the pass creates lives here and is released with it; nothing of the game is written, cleared or rebound by this class.

#include "ext/ffxiv/engine/GlossPuddlesRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::engine::gloss
{
// What one run needs: the tone draw's own scene, the inputs the frame's composite handed over, the render window and the keys.
struct TonePassInputs
{
    ID3D11ShaderResourceView* scene = nullptr;      // the tone map's t0: the scene the frame's composite wrote (read, never written)
    ID3D11Texture2D* sceneTexture = nullptr;        // the resource behind that view, the copy's source
    ID3D11ShaderResourceView* viewPosition = nullptr; // the frame's g_SamplerViewPosition (the view-space point of every pixel)
    ID3D11ShaderResourceView* normal = nullptr;     // the frame's g_SamplerGBuffer (the primary world-space normal, for the up-facing test)
    ID3D11Buffer* camera = nullptr;                 // the frame's g_CameraParameter
    ID3D11ShaderResourceView* wetness = nullptr;    // the gloss pass's own 32-byte wetness buffer: one amount for both stages
    ToneFacts facts;
    Params params;
    // The raindrops' clock, in seconds (GlossPuddlesRules.h RippleTimeOf): the live path reads it from QueryPerformanceCounter, and a host test
    // leaves it at zero, which is the still ripple every reference frame was built with.
    float timeSeconds = 0.0f;
    // How strongly it is raining on the pools, 0..1 (GlossPuddlesRules.h RainStep): the rings' amplitude. A caller that passes a clock and leaves
    // this alone asks for the rings at full strength.
    float rain = 1.0f;
    // Whether this run should write the per-texel statistics (a draw-diagnostics capture is armed). The target exists only once one has asked
    // for it, so a session that never captures never allocates it.
    bool stats = false;
};

class TonePass
{
  public:
    // Runs the pass for one frame and returns the texture to bind at the tone map's t0, or null when it did not run. A null return is the
    // fail-open answer: the caller keeps the game's own scene bound and counts the refusal.
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const TonePassInputs& inputs);

    // Forgets every device object (a device change, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many times the owned targets were created: one set per scene size and format, so a test can pin that a frame does not create D3D
    // objects and that a size change does.
    uint32_t TargetsCreated() const noexcept { return _targetsCreated; }

    // The encoded result of the last run (the texture the tone map reads) and its resource, for the caller's frame memory.
    ID3D11ShaderResourceView* OutputView() const noexcept { return _outSrv.Get(); }
    ID3D11Texture2D* OutputTexture() const noexcept { return _out.Get(); }
    // Whether that result is a whole picture a skipped frame may keep at t0 instead of the game's own scene: the
    // pass's last good answer, whole (the fresh target's cleared texels are not one yet, and a size change drops it
    // with the texture) AND of the frame's own size - a hold bound across a resize would show the last size's measure
    // until the first run of the new one (the review's F5). A run sets it; a reset, a recreate or the first clear unties it.
    bool OutputReady(uint32_t width, uint32_t height) const noexcept { return _outValid && _width == width && _height == height; }
    // Whether the pass's objects belong to `device`: a device-removed frame must not answer a hold with the dead device's texture
    // (the review's device-loss walk).
    bool OnDevice(ID3D11Device* device) const noexcept { return _device.Get() == device; }

    // The statistics of the last run that landed: how much of the render window is pool and how much of that pool the march found a surface
    // for. Filled from a no-wait map of a copy an earlier run took, so it is a frame or two behind, and only while a capture is armed.
    const ToneStats& Stats() const noexcept { return _stats; }
    bool StatsKnown() const noexcept { return _statsKnown; }

    // The GPU time of the draw, measured with timestamp queries and read a run or two later (never waited for).
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }
    bool CanMeasureGpuTime() const noexcept { return _disjoint && _begin && _end; }
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureTargets(ID3D11Device* device, const ToneFacts& facts);
    bool EnsureStatsTarget(ID3D11Device* device, const ToneFacts& facts);
    void CollectGpuTiming(ID3D11DeviceContext* context);
    bool BeginGpuTiming(ID3D11DeviceContext* context);
    void CollectStats(ID3D11DeviceContext* context, const ToneFacts& facts);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params; // the scalars of the draw (GlossPuddlesRules.h's ToneConstants)
    Microsoft::WRL::ComPtr<ID3D11Buffer> _camera; // kCameraBytes: g_CameraParameter +0x000..0x160
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _cameraView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _out;     // the copy of the frame's scene, at the draw's own size and format
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _outRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _outSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _statsTarget; // R16G16_FLOAT: the per-texel pool and hit flags, only while a capture is armed
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _statsRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _statsStaging; // the window's copy of it, mapped a run later without waiting
    uint32_t _statsStagingWidth = 0, _statsStagingHeight = 0;
    bool _statsCopyPending = false; // a copy is in the queue and has not been read yet
    ToneStats _stats;
    bool _statsKnown = false;
    uint32_t _width = 0, _height = 0; // the scene allocation's size
    uint32_t _format = 0;
    bool _clearPending = true;        // the fresh copy has not been cleared yet
    bool _outValid = false;           // the output holds a whole answer (a run has written it past its cleared state)
    uint32_t _targetsCreated = 0;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::engine::gloss
