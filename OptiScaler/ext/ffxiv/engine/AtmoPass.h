#pragma once
// Atmosphere: the pass (ext/ffxiv/engine/shaders/Atmo.hlsl).
//
// One full-screen draw between an explicit save and restore of the state it touches (ext/core/D3D11ExplicitState.h, the pattern the other fork passes use,
// so no binding of the game's is left changed by it), at the tone-map draw and before it: the render window, per surface pixel the optical depth of an
// exponential height medium along the view ray, and the scene mixed toward the air's own light in linear light (out = scene * T + inscatter * (1 - T)),
// into this pass's owned copy of the scene it was handed, seeded by a CopyResource of the handed-in scene (the depth of field's own pattern, so the
// window's outside keeps the handed-in picture byte for byte).
//
// The scene the pass is handed is only read: the copy is what the chain's next reader binds at t0. Everything this class creates lives here and is
// released with it. A pass whose keys say the picture is today's (no medium, no light) answers the copy it made with no draw: the identity at that key is
// one CopyResource and nothing else, so it is byte-exact.
//
// The register slots and the constant-buffer layout are mirrored by AtmoPass.cpp; change them together.

#include "ext/ffxiv/engine/AtmoRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

struct ID3D11DeviceContext;

namespace ext::ffxiv::engine::atmo
{
// What one run needs: the frame's own resources, the render window, the camera's reconstruction, the sun, the medium and the keys.
struct Inputs
{
    ID3D11ShaderResourceView* scene = nullptr; // the chain's current output (the engine's scene, or an earlier stage's copy), read only
    ID3D11ShaderResourceView* depth = nullptr; // the scene depth, as the engine's fog post binds it (reversed-Z)
    uint32_t sceneFormat = 0, sceneWidth = 0, sceneHeight = 0; // the SRV's own format and size
    uint32_t depthFormat = 0, depthWidth = 0, depthHeight = 0;
    shafts::Window window;
    float inverseProjection[16] {};          // g_CameraParameter +0x0E0: the depth's reconstruction
    float ndcScale[2] {}, ndcOffset[2] {};   // the pixel-to-NDC mapping, derived from the render window (RecogRules.h PixelToNdcRow)
    float upView[3] { 0.0f, 1.0f, 0.0f };    // the world's up in view space
    float sunView[3] { 0.0f, 1.0f, 0.0f };   // the direction toward the sun in view space
    float sunGain = 0.0f;                    // 0 = no sun: the ambient term alone
    float densityPerMetre = 0.0f;            // the medium's density at the camera's height after the engine-fog and night scales (EffectiveDensityPerMetre)
    float fogColour[3] { 0.0f, 0.0f, 0.0f }; // the engine's fog colour, linear light: the ambient term
    float sunTint[3] { 1.0f, 1.0f, 1.0f };   // the sun's colour of peak 1
    float whiteLinear = 0.0f;                // the frame's white in linear light (WhiteLinearOf)
    Params params;
};

class Pass
{
  public:
    // Runs the pass for one tone-map draw and returns this pass's copy of the scene with the air mixed in, or null when the pass did not run (a null
    // return is the fail-open answer: the caller keeps the texture it handed over - the chain's hold stands).
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const Inputs& inputs);

    // Forgets every device object (a device change, a lost device, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many scene copies this instance has created: one per size, so a dynamic-resolution change does not create a texture per frame.
    uint32_t ScenesCreated() const noexcept { return _scenesCreated; }

    // The GPU time of the pass's draw: one timestamp pair inside a disjoint query, read a run or two later and never waited for.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The pass's own draw count of the last run (0 when the run answered the copy alone, 1 when it drew) - the identity test reads this.
    uint32_t DrawsLastRun() const noexcept { return _drawsLastRun; }

    // For the test that pins the pairing: a run ends a pair only when it began one, so a stray End is visible as Ends() > Begins().
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureScene(ID3D11Device* device, D3D11_TEXTURE2D_DESC sceneDesc);
    bool BeginGpuTiming(ID3D11DeviceContext* context);
    void CollectGpuTiming(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _shader;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;

    // This pass's copy of the scene it was handed: the same description as the engine's, so CopyResource accepts the copy, with a render target and a
    // shader resource view in the scene's FP16 format.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _scene;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sceneSrv;
    uint32_t _sceneResourceFormat = 0, _sceneWidth = 0, _sceneHeight = 0;

    uint32_t _scenesCreated = 0;
    uint32_t _drawsLastRun = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::engine::atmo
