#pragma once
// Screen-space light bounce (ext/ffxiv/engine/shaders/Gi.hlsl).
//
// Eight full-screen draws between an explicit save and restore of the state they touch (ext/core/D3D11ExplicitState.h, the pattern the other
// fork passes use, so no binding of the game's is left changed by them), at the tone-map draw and before it:
//  1. the irradiance, at a half (or a quarter) of the render window: per pixel, K cosine rays out of the G-buffer normal's tangent basis,
//     each ray marched against the composite's own view positions through the pass's own march file (shaders/GiMarch.hlsl) up to the range
//     key with thickness rejection, a hit's radiance toward the origin read as the hit texel's own linear scene value (the scene's stored
//     texel squared), the estimate (the rules' IrradianceEstimate, times the pixel's own view fade) into an own RGBA16F target whose alpha
//     is the rays marched (above 0 for every traced origin, hit or not; 0 for one that could not be traced);
//  2. the guide, over the same extent: each traced texel's camera-facing view-space normal and its depth distance, which the filter tells
//     surfaces apart by (all zero where the irradiance is no data: both come from one decode of the origin pixel);
//  3. the outlier control: a texel's luminance held to a multiple of its neighbours' mean (plus a floor), so a lone ray that hit a lamp
//     cannot print a bright blob;
//  4. to 7. four iterations of the edge-aware a-trous filter over the clamped irradiance (the 1-4-6-4-1 outer product, taps one, two,
//     four and eight texels apart, ping-ponging between two targets), each tap weighted by its distance from the centre's tangent plane
//     and the agreement of the normals, the taps that have no data (alpha 0) skipped, the traced zeros averaged in, the sum divided by
//     the weights that were present;
//  8. the apply, over the render window: the scene plus the filtered estimate (the four texels around the pixel, weighted by their
//     bilinear share and by whether they carry data) multiplied by the origin pixel's own albedo multiplier (the engine's own
//     (gb2.rgb)^2, with the material pack's diffuse weight and the engine's special-row exclusion) times the strength key, added
//     through the radial shafts' own white-bounded addition.
//
// The scene the pass is handed is only read: it is copied into an owned texture of the same description, the apply writes that copy, and the
// caller binds it at t0 for the one draw (the owner role) or takes it as the chain's next stage's input (the chained role). Everything this
// class creates lives here and is released with it.
//
// The register slots and the constant-buffer layout are mirrored by GiPass.cpp; change them together with shaders/Gi.hlsl.

#include "ext/ffxiv/engine/GiRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <vector>
#include <wrl/client.h>

namespace ext::ffxiv::engine::gi
{
// What one run needs: the frame's own resources (all of them read only), its measured sizes, the render window, the camera's two matrices
// the march needs, and the tuning keys, white point and fade.
struct Inputs
{
    ID3D11ShaderResourceView* scene = nullptr;    // the chain's current output (the engine's scene, or an earlier stage's copy), read only
    ID3D11ShaderResourceView* depth = nullptr;    // the scene depth, as the fog post binds it (reversed-Z)
    ID3D11ShaderResourceView* viewPos = nullptr;  // the composite's own view-position buffer
    ID3D11ShaderResourceView* normal = nullptr;   // G-buffer 0: the primary world-space normal, the material id byte in .a
    ID3D11ShaderResourceView* albedo = nullptr;   // G-buffer 2: the albedo carrier, gamma-2 encoded (the apply reads it: the origin's own albedo multiplier)
    ID3D11ShaderResourceView* material = nullptr; // G-buffer 1: the material pack (x the selector, z the diffuse weight)
    uint32_t sceneFormat = 0, sceneWidth = 0, sceneHeight = 0;
    uint32_t depthFormat = 0, depthWidth = 0, depthHeight = 0;
    shafts::Window window; // the render window of the draw, in the scene's own pixels (ShaftsRules.h)
    float view[12] {};           // g_CameraParameter +0x000: the view matrix's three rows, mul(matrix, v) applies them row-wise
    float projection[16] {};     // g_CameraParameter +0x120: the projection, the march's rays project through it
    Params params;
    // The frame's own white point in the scene's units (ShaftsRules.h WhitePointFromExposure): the added light is a share of it, and no
    // pixel is pushed past it.
    float whitePoint = shafts::kFallbackWhitePoint;
    // The weight of the state this frame draws with: 1 when this frame's own read landed, less when the held state draws (ShaftsRules.h
    // HoldFade). It scales the added light, so a lost read decays instead of switching off.
    float fade = 1.0f;
    // True on the frame an F9 dump captures, and only then: the pass reads its own half-resolution targets back once (a staging copy of the raw and of the
    // final one, a blocking map) and keeps the two numbers the stage line and the log carry (CaptureStats). Off on every other frame: no readback, no
    // reduction, nothing created.
    bool captureStats = false;
};

class Pass
{
  public:
    // Runs the pass for one tone-map draw and returns this pass's copy of the scene with the bounce added, or null when the pass did not
    // run (a null return is the fail-open answer: the caller keeps the texture it handed over - the chain's hold stands).
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const Inputs& inputs);

    // Forgets every device object (a device change, a lost device, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many scene copies, irradiance and smooth targets this instance has created: one of each per size, so a dynamic-resolution change
    // does not create a texture per frame.
    uint32_t ScenesCreated() const noexcept { return _scenesCreated; }
    uint32_t IrradiancesCreated() const noexcept { return _irradianceCreated; }

    // The GPU time of the pass's three draws: one timestamp pair inside a disjoint query, read a run or two later and never waited for.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The pass's own targets, for the tests that pin the march against the CPU reference (the irradiance is the estimator's own store, the
    // guide the filter's per-texel surface, the smooth the filter's final answer, the one the apply reads).
    ID3D11ShaderResourceView* IrradianceView() const noexcept { return _irradiance.srv.Get(); }
    ID3D11ShaderResourceView* GuideView() const noexcept { return _guide.srv.Get(); }
    ID3D11ShaderResourceView* SmoothView() const noexcept { return _filterA.srv.Get(); }

    // The capture frame's numbers of the last run, when that run was asked for them (Inputs::captureStats); false after any other run.
    bool HasCaptureStats() const noexcept { return _haveStats; }
    CaptureStats LastCaptureStats() const noexcept { return _stats; }

    // The two roi sizes the pass built last (or 0, 0), for the card and the log.
    uint32_t IrradianceWidth() const noexcept { return _irradianceWidth; }
    uint32_t IrradianceHeight() const noexcept { return _irradianceHeight; }

    // For the test that pins the pairing: a run ends a pair only when it began one, so a stray End is visible as Ends() > Begins().
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

    bool Init(ID3D11Device* device);

  private:
    // One half- or quarter-resolution RGBA16F target with the views the draws use.
    struct Target
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        bool Create(ID3D11Device* device, uint32_t width, uint32_t height);
        void Reset();
    };

    bool EnsureScene(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height);
    bool EnsureStageTargets(ID3D11Device* device, uint32_t width, uint32_t height);
    void CollectGpuTiming(ID3D11DeviceContext* context);
    bool BeginGpuTiming(ID3D11DeviceContext* context);
    // One half-resolution target copied to a staging texture and read as floats (RGBA, row-major); false when the copy or the map failed.
    bool ReadTarget(ID3D11DeviceContext* context, const Target& target, std::vector<float>& out);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _giShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _guideShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _clampShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _atrousShader[4];
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _applyShader;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _scene;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sceneSrv;
    Target _irradiance; // the raw estimate (PS_Gi)
    Target _guide;      // the filter's guide (PS_Guide)
    Target _filterA;    // the clamp's answer and every second iteration's; the final one after the fourth
    Target _filterB;    // the iterations between
    uint32_t _sceneResourceFormat = 0, _sceneWidth = 0, _sceneHeight = 0;
    uint32_t _irradianceWidth = 0, _irradianceHeight = 0;
    uint32_t _scenesCreated = 0, _irradianceCreated = 0;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;
    bool _haveStats = false;
    CaptureStats _stats;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::engine::gi
