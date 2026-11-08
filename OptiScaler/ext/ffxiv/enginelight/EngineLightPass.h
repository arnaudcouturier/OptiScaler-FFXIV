#pragma once
// Engine light controls, the exposure hold's pass and its two small readers (ext/ffxiv/enginelight/shaders/ExposureHold.hlsl).
//
// The pass: one draw between an explicit save and restore of the state it touches (ext/core/D3D11ExplicitState.h, the stages' own pattern, so no binding of the game's is
// left changed by it): the scene it was handed is read with Load, scaled by the gain, and written into this pass's own copy of it over the render window plus a two-pixel
// margin. No CopyResource: the tone map reads t0 inside its own window only (a bilinear tap at the window's edge reads the margin).
//
// No pass at all (null, no draw, the engine's own texel stays bound) when the gain is the identity: the mode is off, the master is off, the A/B shows the engine's values,
// the applied weight is 0, no hold has latched yet, or this frame's exposure copy is missing. That is the byte-exact passthrough of a day frame.
//
// The exposure reader copies the 64-byte scene-pass g_CommonParameter the composite binds, on the GPU, into an owned constant buffer (this frame's exact `s` for the
// shader, no CPU lag) and from there into a ring of staging buffers the CPU reads a frame or two later without ever waiting (the latch, the card and the log).
//
// The register slots and the constant-buffer layouts are mirrored by EngineLightPass.cpp; change them together.

#include "ext/ffxiv/engine/ShaftsRules.h" // Window, WindowUsable, IsSceneFormat
#include "ext/ffxiv/enginelight/EngineLightRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

struct ID3D11DeviceContext;

namespace ext::ffxiv::enginelight
{
inline constexpr float kWindowMargin = 2.0f; // the pixels of margin the pass covers around the render window
inline constexpr uint32_t kExposureRing = 4;  // staging buffers in the reader's ring
inline constexpr uint32_t kTableTexels = 1024; // the engine's luminance-rebalance table (1024x1, FP16 RGBA)

// One landed read of the scene-pass exposure.
struct ExposureRead
{
    bool landed = false;       // a copy was mapped (it may still fail the fingerprint: `valid`)
    bool valid = false;        // ... and it passed ExposureValid
    float s = 0.0f, y = 0.0f;
    uint64_t writtenFrame = 0; // the rendered frame the copy was taken at
};

class ExposureReader
{
  public:
    // Copies the bytes [firstByte, firstByte + 64) of the composite's g_CommonParameter into the owned constant buffer and the ring, GPU side. False when the buffer or the
    // staging could not be made.
    bool Capture(ID3D11DeviceContext* context, ID3D11Buffer* common, uint32_t firstByte, uint64_t frame);

    // Maps the newest copy of an earlier frame than `frame` that has not been read yet, without waiting. `landed` false when none has landed yet.
    ExposureRead Read(ID3D11DeviceContext* context, uint64_t frame);

    // This frame's owned copy (bound at b1 by the pass), valid only for the frame it was captured in.
    ID3D11Buffer* Live() const noexcept { return _live.Get(); }
    uint64_t LiveFrame() const noexcept { return _liveFrame; }

    void Reset();

  private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
        uint64_t frame = 0;
        bool written = false;
    };
    bool Ensure(ID3D11Device* device);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _live;
    uint64_t _liveFrame = 0;
    Slot _ring[kExposureRing];
    uint32_t _next = 0;
    uint64_t _lastAcceptedFrame = 0;
    bool _haveAccepted = false;
};

// Three texels of the engine's 1024x1 luminance-rebalance table (the second exposure system, a function of the GPU 1x1), sampled once in a while for the log: whether it
// moves with the darkening is what the 1x1 series of the next session needs. A two-slot staging ring read without waiting.
struct TableRead
{
    bool landed = false;
    float low = 0.0f, mid = 0.0f, high = 0.0f; // mean of RGB at texels 128, 512 and 896
};

class TableSampler
{
  public:
    bool Sample(ID3D11DeviceContext* context, ID3D11ShaderResourceView* table);
    TableRead Read(ID3D11DeviceContext* context);
    void Reset();

  private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _staging[2];
    bool _pending[2] {};
    uint32_t _next = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> _device;
};

struct Inputs
{
    ID3D11ShaderResourceView* scene = nullptr; // the chain's current output (the engine's scene, or an earlier stage's copy), read only
    uint32_t sceneFormat = 0, sceneWidth = 0, sceneHeight = 0; // the SRV's own format and size
    shafts::Window window;
    int mode = kExposureAuto;     // 1 auto hold, 2 manual
    float manualGain = 1.0f;      // mode 2: the factor, already eased with the weight
    float sHold = 0.0f;           // mode 1: the latched exposure
    ID3D11Buffer* exposureCopy = nullptr; // mode 1: this frame's copy of g_CommonParameter (the reader's Live())
};

class Pass
{
  public:
    // Runs the pass for one tone-map draw and returns this pass's copy of the scene with the gain applied, or null when the pass did not run: the identity gain is no run
    // at all (failure stays null, the engine's own texel is the answer), anything else that stops it sets Failure().
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const Inputs& inputs);

    // Forgets every device object (a device change, a lost device, or the end of the session).
    void Reset();

    const char* Failure() const noexcept { return _failure; }
    uint32_t ScenesCreated() const noexcept { return _scenesCreated; }

    // The GPU time of the draw: one timestamp pair inside a disjoint query, read a run or two later and never waited for.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // Draws of the last run: 0 when the pass was declined before drawing, 1 for the hold.
    uint32_t DrawsLastRun() const noexcept { return _drawsLastRun; }

    // For the test that pins the pairing: a run ends a pair only when it began one.
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

    // The pass's own scene copy, for the lab and the tests' readback.
    ID3D11Texture2D* SceneTexture() const noexcept { return _scene.Get(); }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureScene(ID3D11Device* device, D3D11_TEXTURE2D_DESC sceneDesc);
    void CollectGpuTiming(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _holdShader;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;

    // This pass's own texture: the scene's description (so a later stage's CopyResource accepts it), with a render target and a shader resource view in FP16.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _scene;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sceneSrv;
    uint32_t _sceneResourceFormat = 0, _sceneWidth = 0, _sceneHeight = 0;

    uint32_t _scenesCreated = 0;
    uint32_t _drawsLastRun = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::enginelight
