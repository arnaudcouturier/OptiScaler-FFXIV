#pragma once
// God rays: the volumetric march (ext/ffxiv/engine/shaders/Godrays.hlsl).
//
// Up to three full-screen draws and one dispatch between an explicit save and restore of the state they touch (ext/core/D3D11ExplicitState.h, the
// pattern the other fork passes use, so no binding of the game's is left changed by them; the dispatch's compute state has its own save and restore
// because the Guard does not cover the compute stage), at the tone-map draw and before it:
//  1. the march, at a quarter (or half) of the render window: per pixel, the view ray from the engine's own inverse projection, N steps
//     through the air in front of it, each step's visibility read from the engine's own cascade shadow atlas (all four cascades, each only
//     inside its own slice, the minimum) with the compare the engine's own mask writer uses, the medium from the engine's own live fog profile
//     times the density key, a Henyey-Greenstein phase around the shadow light, integrated as an in-scatter sum with extinction and the
//     per-pixel Hash01 jitter - one float into an own R16F target;
//  1b. the stats dispatch: the 99th percentile of the scene's own sky pixels per channel (a 1/16 sample) and the term's peak, into an own 2x1
//     RGBA32F texture the composite reads and the draw path copies to its staging ring;
//  2. the sun's colour, into one texel: the sky's colour around the sun's screen position (the radial's scan-and-sky rule, skipped when
//     the colour key does not need it);
//  3. the composite, over the render window, adding the march's term (upsampled depth-aware) to this pass's own copy of the scene it was
//     handed - the same ShaftAdded white-bounded expression the radial's composite runs, the sky capped at its own ceiling.
//
// The scene the pass is handed is only read: it is copied into an owned texture of the same description, and the caller binds that copy
// at t0 for the one draw (the owner role) or takes it as the chain's next stage's input (the chained role). Everything this class creates
// lives here and is released with it.
//
// The register slots and the constant-buffer layout are mirrored by GodraysPass.cpp; change them together.

#include "ext/ffxiv/engine/GodraysRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::godrays
{
// The march's per-frame constants, as the CPU hands them over: the camera's matrices, the light's direction, the decoded fog state and
// the cascades in the march's own order (finest first), with the normalization the term's stability needs.
struct FrameState
{
    float sunDirection[3] { 0.0f, 1.0f, 0.0f }; // the shadow light's direction, view space, unit, toward the sun
    float inverseProjection[16] {};             // g_CameraParameter +0x0E0, four rows of four floats
    float inverseViewRow1[4] {};                // +0x040: the world y of a view point is this row dotted with (p, 1)
    float projection[16] {};                    // +0x120: the sun's screen position for the auto colour is projected through it
    float ndcScale[2] { 0.0f, 0.0f };           // the pixel-to-NDC scale, derived from the render window
    float ndcOffset[2] { 0.0f, 0.0f };          // and the offset (RecogRules.h PixelToNdcRow)
    FogState fog;
    Cascade cascades[kCascadeCount] {};         // in the march's order: the finest first (CascadeOrder)
    uint32_t cascadeCount = 0;
    float cameraWorldY = 0.0f;                  // the camera's own world height (CameraWorldY)
    float invRhoCamera = 0.0f;                  // 1/max(rho(cameraWorldY, heightLift), eps): the term's normalization
    float marchFar = 0.0f;                      // the march's far bound in view metres (MarchFarOf)
    float discScale = 0.0f;                     // the shared map row's own disc scale (DiscScale of the captured cb3[7].x)
};

// What one run needs: the frame's own resources, its measured sizes, the render window, the march's state and the tuning keys.
struct Inputs
{
    ID3D11ShaderResourceView* scene = nullptr; // the chain's current output (the engine's scene, or an earlier stage's copy), read only
    ID3D11ShaderResourceView* depth = nullptr;  // the scene depth, as the engine's fog post binds it (reversed-Z)
    ID3D11ShaderResourceView* atlas = nullptr;  // the cascade shadow atlas, sampled as R16_UNORM
    ID3D11ShaderResourceView* sky = nullptr;    // the engine's sky colour texture (the enginefog colour reads it; null otherwise)
    ID3D11ShaderResourceView* lut = nullptr;    // the engine's fog weight LUT (the enginefog colour reads it; null otherwise)
    uint32_t sceneFormat = 0, sceneWidth = 0, sceneHeight = 0; // the SRV's own format and size
    uint32_t depthFormat = 0, depthWidth = 0, depthHeight = 0;
    Window window;
    float sunScreenX = 0.0f, sunScreenY = 0.0f; // the sun's screen position in scene pixels (the auto colour's search centre)
    // The resolved tint: the light's own colour (peak 1) when the engine reports one, the white key's neutral, or - when the auto mode
    // falls back to the sky - the flag that tells the pass to run its one-texel colour search and the composite to read its texel.
    float tintRgb[3] { 1.0f, 1.0f, 1.0f };
    bool tintFromSky = true;
    FrameState state;                            // the march's per-frame constants, decoded on the CPU from the captured copies
    Params params;
    // The light's luminance gain (LightLuminanceGain): scales the composite so a moon-like light never runs at sun strength. 1 = the full effect, which is
    // what a caller that judges no light (the lab driver) gets. The draw path's gates (aside, axis) decide whether the pass runs at all.
    float lightGain = 1.0f;
    bool skyCeiling = true; // the sky is capped at its own 99th percentile; false is the uncapped control of the tests
    bool bilateral = true;  // the depth-aware upsample of the quarter-resolution term; false is the plain bilinear read
    // The frame's own white point in the scene's units (ShaftsRules.h WhitePointFromExposure): the added light is a share of it, and no
    // pixel is pushed past it.
    float whitePoint = shafts::kFallbackWhitePoint;
    // The weight of the state this frame draws with: 1 when this frame's own read landed, less when it is the last accepted state held
    // across a gap (ShaftsRules.h HoldFade). It scales the added light, so a lost read decays instead of switching off.
    float fade = 1.0f;
};

class Pass
{
  public:
    // Runs the pass for one tone-map draw and returns this pass's copy of the scene with the march added, or null when the pass did not
    // run (a null return is the fail-open answer: the caller keeps the texture it handed over - the chain's hold stands).
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const Inputs& inputs);

    // Forgets every device object (a device change, a lost device, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many scene copies and march targets this instance has created: one of each per size, so a dynamic-resolution change does not
    // create a texture per frame.
    uint32_t ScenesCreated() const noexcept { return _scenesCreated; }
    uint32_t MarchTargetsCreated() const noexcept { return _marchCreated; }

    // The GPU time of the pass's draws: one timestamp pair inside a disjoint query, read a run or two later and never waited for.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The march's term target, for the test that pins the beam pattern against the CPU reference march.
    ID3D11ShaderResourceView* TermView() const noexcept { return _termSrv.Get(); }

    // The stats texture (2x1 RGBA32F: texel 0 = sky ceilings and the term's peak, texel 1 = the sky sample count): the draw path copies it to its
    // staging ring (StatsReader) and reads it a frame or two later; null before the first run.
    ID3D11Texture2D* StatsTexture() const noexcept { return _stats.Get(); }

    // For the test that pins the pairing: a run ends a pair only when it began one, so a stray End is visible as Ends() > Begins().
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureScene(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height);
    bool EnsureMarch(ID3D11Device* device, uint32_t width, uint32_t height);
    bool EnsureSunColour(ID3D11Device* device);
    bool EnsureStats(ID3D11Device* device);
    bool BeginGpuTiming(ID3D11DeviceContext* context);
    void CollectGpuTiming(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _marchShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _sunColourShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _compositeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _statsShader;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;

    // This pass's copy of the scene it was handed: the same description as the engine's, so CopyResource accepts the copy, with a render
    // target and a shader resource view in the scene's FP16 format.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _scene;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sceneSrv;
    uint32_t _sceneResourceFormat = 0, _sceneWidth = 0, _sceneHeight = 0;

    // The march's term: one float per pixel of the march's own resolution (R16F).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _term;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _termRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _termSrv;
    uint32_t _marchWidth = 0, _marchHeight = 0;

    // The sun's colour: one texel of the sky's colour around the sun (rgb) and whether there was sky to sample (a).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _sunColour;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sunColourRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sunColourSrv;

    // The stats: the compute pass writes it through the UAV, the composite reads it through the SRV.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _stats;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _statsUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _statsSrv;

    uint32_t _scenesCreated = 0;
    uint32_t _marchCreated = 0;
    uint32_t _sunColourCreated = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::godrays
