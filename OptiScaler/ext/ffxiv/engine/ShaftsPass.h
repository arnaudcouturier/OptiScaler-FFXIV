#pragma once
// Light shafts before the tone map: the pass itself (ext/ffxiv/engine/shaders/LightShafts.hlsl).
//
// Five full-screen draws between one explicit save and restore of the state they touch (ext/core/D3D11ExplicitState.h; so no binding of the game's is
// touched by them), at the tone-map draw and before it:
//  1. the sky mask, at half the render window, reading the frame's depth (reversed-Z: the far plane is sky);
//  2. the radial blur of that mask toward the sun's screen position, at half the render window (the term);
//  3. the low-pass of the term: its mean over blocks of ShaftsRules.h kBeamBlock x kBeamBlock texels, a few dozen texels in all;
//  4. the sun's colour, into one texel: the sky's own colour around the sun, so a leaf in front of the sun cannot tint the shafts (skipped when
//     the colour key asks for the neutral shaft, which needs no colour from the frame);
//  5. the composite, over the render window, adding the beams - the term's part that stands above its low-pass, plus a small kept share of the
//     level, faded with the distance from the sun - to this pass's own copy of the lit scene.
// The engine's scene is only read: it is copied into an owned texture of the same description, and the tone-map draw binds that copy for its
// one draw. Everything this class creates lives here and is released with it.

#include "ext/ffxiv/engine/ShaftsRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::shafts
{
// What one run needs: the frame's own resources, its measured sizes, the render window, the sun and the tuning keys.
struct Inputs
{
    ID3D11ShaderResourceView* scene = nullptr; // the tone map's t0: the engine's lit scene, read only
    ID3D11ShaderResourceView* depth = nullptr;  // the light draw's depth, read as float (R24_UNORM_X8_TYPELESS or R32_FLOAT)
    uint32_t sceneFormat = 0, sceneWidth = 0, sceneHeight = 0; // the SRV's own format and size
    uint32_t depthFormat = 0, depthWidth = 0, depthHeight = 0;
    Window window;
    SunProjection sun; // the sun's screen position in scene pixels
    Params params;
    SunTint tint;      // the light's colour, or "take the sky's colour at the sun"
    // The frame's own white point in the scene's units (ShaftsRules.h WhitePointFromExposure): the added light is a share of it, and no pixel is
    // pushed past it.
    float whitePoint = kFallbackWhitePoint;
    // The weight of the sun this frame draws with: 1 when the frame's own read landed it, less when it is the last accepted sun held across a gap
    // (ShaftsRules.h HoldFade). It scales the added light, so a lost sun decays instead of switching off.
    float fade = 1.0f;
};

class Pass
{
  public:
    // Runs the pass for one tone-map draw and returns this pass's copy of the scene with the shafts added, or null when the pass did not run
    // (a null return is the fail-open answer: the caller keeps the game's own binding).
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const Inputs& inputs);

    // Forgets every device object (a device change, a lost device, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many scene copies and half-resolution targets this instance has created: one of each per size, so a dynamic-resolution change (a
    // smaller render window inside the same allocation) does not create a texture per frame.
    uint32_t ScenesCreated() const noexcept { return _scenesCreated; }
    uint32_t HalfTargetsCreated() const noexcept { return _halfCreated; }
    uint32_t SunColoursCreated() const noexcept { return _sunColourCreated; }

    // The GPU time of the pass's draws: one timestamp pair inside a disjoint query, read a run or two later and never waited for.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The half-resolution pair of the last run, for the test that pins the mask (1 where the depth is the far plane) and the blur's shape.
    ID3D11ShaderResourceView* MaskView() const noexcept { return _maskSrv.Get(); }
    ID3D11ShaderResourceView* TermView() const noexcept { return _termSrv.Get(); }

    // The low-pass of the term of the last run (R16F, LowExtent of the half-resolution extent in each direction): the block means the composite reads.
    ID3D11ShaderResourceView* LowView() const noexcept { return _lowSrv.Get(); }

    // The one texel the colour pass wrote: rgb = the sky's colour around the sun, a = 1 when there was sky to sample.
    ID3D11ShaderResourceView* SunColourView() const noexcept { return _sunColourSrv.Get(); }

    // For the test that pins the pairing: a run ends a pair only when it began one, so a stray End is visible as Ends() > Begins().
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureScene(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height);
    bool EnsureHalf(ID3D11Device* device, uint32_t width, uint32_t height);
    bool EnsureSunColour(ID3D11Device* device);
    bool BeginGpuTiming(ID3D11DeviceContext* context);
    void CollectGpuTiming(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _maskShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _blurShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _lowShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _sunColourShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _compositeShader;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;

    // This pass's copy of the scene: the same description as the engine's, so CopyResource accepts it, with a render target and a shader
    // resource view in the scene's FP16 format.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _scene;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sceneSrv;
    uint32_t _sceneResourceFormat = 0, _sceneWidth = 0, _sceneHeight = 0;

    // The half-resolution pair: the sky mask (R8) and the blurred term (R16F), both the size of half the render window, and the term's low-pass
    // (R16F, a texel per ShaftsRules.h kBeamBlock term texels): they are created and released together.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _mask;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _maskRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _maskSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _term;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _termRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _termSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _low;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _lowRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _lowSrv;
    uint32_t _halfWidth = 0, _halfHeight = 0;

    // The sun's colour: one texel of the sky's colour around the sun (rgb) and whether there was sky to sample (a).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _sunColour;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sunColourRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sunColourSrv;

    uint32_t _scenesCreated = 0;
    uint32_t _halfCreated = 0;
    uint32_t _sunColourCreated = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::shafts
