#pragma once
// Depth of field, Route B: the pass (ext/ffxiv/engine/shaders/Dof.hlsl).
//
// Three draws (four in the distance look with an auto focus) between an explicit save and restore of the state they touch
// (ext/core/D3D11ExplicitState.h, the march's own pattern, so no binding of the game's is left changed by them), at the tone-map draw and before it:
//  0. (distance look, auto focus only) the focus update, one texel: the smoothed focus of the look, kept in a pair of one-texel textures that swap each
//     run (no readback; the state is read at pixel-shader slot 3, which nothing else binds during the focus update and the pack);
//  1. the weight+CoC pack, at the render window: colour (the pack's cap, the game's own min(col,10)) in rgb, the
//     analytic signed CoC in alpha - one draw;
//  2. the disc blur, at a quarter of the window, near and far: two targets bound at once (the game's own DownScale4x4
//     writes two), the taps split by the CoC's sign and weighted by each tap texel's own CoC - one draw;
//  3. the merge: the two quarter targets read back bilinearly over the render window and mixed into this pass's owned
//     copy of the scene it was handed, seeded by a CopyResource of the handed-in scene (the march's own pattern, so the
//     window's outside keeps the handed-in picture byte for byte) - one draw.
//
// The scene the pass is handed is only read for the pack and the merge: the copy is what the chain's next reader binds
// at t0. Everything this class creates lives here and is released with it.
//
// The register slots and the constant-buffer layout are mirrored by DofPass.cpp; change them together.

#include "ext/ffxiv/engine/DofRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

struct ID3D11DeviceContext;

namespace ext::ffxiv::dof
{
// What one run needs: the frame's own resources, the render window, the camera rows (the reconstruction the pack walks)
// and the tuning keys.
struct Inputs
{
    ID3D11ShaderResourceView* scene = nullptr; // the chain's current output (the engine's scene, or an earlier stage's copy), read only
    ID3D11ShaderResourceView* depth = nullptr; // the scene depth, as the engine's fog post binds it (reversed-Z)
    uint32_t sceneFormat = 0, sceneWidth = 0, sceneHeight = 0; // the SRV's own format and size
    uint32_t depthFormat = 0, depthWidth = 0, depthHeight = 0;
    shafts::Window window;
    float deltaSeconds = 0.0f;        // the time since this pass last RAN, in seconds (the focus smoothing's clock; 0 = no time passed, so a
                                      // caller that gives none sees the first sample adopted and the focus held after that)
    float inverseProjection[16] {};   // g_CameraParameter +0x0E0, the band accepted it (DofRules.h CameraDepthBand)
    float ndcScale[2] {}, ndcOffset[2] {}; // the pack's pixel-to-NDC mapping, derived from the render window (RecogRules.h PixelToNdcRow)
    Params params;
};

class Pass
{
  public:
    // Runs the pass for one tone-map draw and returns this pass's copy of the scene with the blur merged into it, or
    // null when the pass did not run (a null return is the fail-open answer: the caller keeps the texture it handed
    // over - the chain's hold stands). A stage whose keys say the picture is today's (Strength 0, or an aperture at the
    // floor) answers the copy it made, with no draws: the identity at that key is one CopyResource and nothing else.
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const Inputs& inputs);

    // Forgets every device object (a device change, a lost device, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many scene copies, packs and quarter targets this instance has created: one of each per size, so a
    // dynamic-resolution change does not create a texture per frame.
    uint32_t ScenesCreated() const noexcept { return _scenesCreated; }
    uint32_t PacksCreated() const noexcept { return _packsCreated; }
    uint32_t QuartersCreated() const noexcept { return _quartersCreated; }

    // The GPU time of the pass's draws: one timestamp pair inside a disjoint query, read a run or two later and never
    // waited for.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The pass's own draw count of the last run (0 when the run was declined before the draws, 3 when it completed) -
    // the Strength-0 identity test reads this: no draws at all is the copy's own byte-exactness.
    uint32_t DrawsLastRun() const noexcept { return _drawsLastRun; }

    // The pass's own SRVs, for the test that pins the pack, the blur and the merge against the CPU reference.
    ID3D11ShaderResourceView* PackView() const noexcept { return _packSrv.Get(); }
    ID3D11ShaderResourceView* NearView() const noexcept { return _nearSrv.Get(); }
    ID3D11ShaderResourceView* FarView() const noexcept { return _farSrv.Get(); }

    // The distance look's one-texel focus state (R32G32_FLOAT: R = the smoothed focus in view metres, G = the last run's raw sample), or null before the
    // first update. The dump hands it to a staging copy on the capture frame; nothing reads it back otherwise.
    ID3D11ShaderResourceView* FocusView() const noexcept { return _focusRan ? _focusSrv[_focusIndex].Get() : nullptr; }

    // For the test that pins the pairing: a run ends a pair only when it began one, so a stray End is visible as Ends() > Begins().
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureTargets(ID3D11Device* device, D3D11_TEXTURE2D_DESC sceneDesc, uint32_t windowWidth, uint32_t windowHeight);
    bool BeginGpuTiming(ID3D11DeviceContext* context);
    void CollectGpuTiming(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _packShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _blurShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _mergeShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _packDistanceShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _focusShader;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;

    // The pack: one FP16 RGBA texture at the scene's own allocation shape (rgb = the capped colour, a = the signed CoC).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _pack;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _packRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _packSrv;
    uint32_t _packWidth = 0, _packHeight = 0;

    // The focus state: two one-texel textures, the latest result in _focusIndex (a run reads it and writes the other one, then swaps).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _focus[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _focusRtv[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _focusSrv[2];
    uint32_t _focusIndex = 0;
    bool _focusRan = false;

    // The quarter pair: the near and the far half, each one FP16 RGBA texture at the quarter of the window.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _near, _far;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _nearRtv, _farRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _nearSrv, _farSrv;
    uint32_t _quarterWidth = 0, _quarterHeight = 0;

    // This pass's copy of the scene it was handed: the same description as the engine's, so CopyResource accepts the
    // copy, with a render target and a shader resource view in the scene's FP16 format.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _scene;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _sceneRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sceneSrv;
    uint32_t _sceneResourceFormat = 0, _sceneWidth = 0, _sceneHeight = 0;

    uint32_t _scenesCreated = 0;
    uint32_t _packsCreated = 0;
    uint32_t _quartersCreated = 0;
    uint32_t _drawsLastRun = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::dof
