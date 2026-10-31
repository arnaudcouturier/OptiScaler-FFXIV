#pragma once
// Contact shadows before the sun light pass: the pass itself (ext/ffxiv/relight/shaders/ContactShadows.hlsl).
//
// Two draws on the game's own immediate context, inside a private device-context state (the pattern the other fork passes use, so no
// game binding is touched by them):
//  1. the march, at half the render window, reading the draw's depth buffer and the camera's inverse projection (the light draw binds no
//     view-position buffer) and writing the contact visibility;
//  2. the combine, over the mask's whole allocation, writing `mask * term` into this pass's copy of the mask.
// The two buffers the game owns and the pass needs - the sun direction (g_LightParam +0x10) and the view and view-projection matrices
// (g_CameraParameter +0x00..0xA0) - are copied GPU side into buffers of this pass with a partial CopySubresourceRegion: nothing is ever
// read back to the CPU, so no frame stalls on a map.
//
// Everything the pass creates lives here and is released with it; nothing of the game is written, cleared or rebound by this class.

#include "ext/ffxiv/relight/ContactShadowsRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::relight
{
// What one run needs: the draw's own resources, its measured sizes, the render window and the tuning keys.
struct Inputs
{
    ID3D11ShaderResourceView* mask = nullptr;  // the draw's g_SamplerShadow (the screen-space mask)
    ID3D11ShaderResourceView* depth = nullptr; // the draw's g_SamplerDepth (the view position is reconstructed from it)
    ID3D11Buffer* lightParam = nullptr;        // the draw's g_LightParam
    ID3D11Buffer* cameraParam = nullptr;       // the draw's g_CameraParameter
    uint32_t maskFormat = 0;
    uint32_t maskWidth = 0, maskHeight = 0;
    Window window;
    Params params;
};

class Pass
{
  public:
    // Runs the pass for one draw and returns the copy that replaces the mask's binding, or null when the pass did not run. A null return
    // is the fail-open answer: the caller keeps the game's own binding.
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const Inputs& inputs);

    // Forgets every device object (a device change, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many mask copies and term targets this instance has created: one of each per allocation size, so a test can pin that a
    // dynamic-resolution change (a smaller render window inside the same allocation) does not create D3D objects per frame.
    uint32_t CopiesCreated() const noexcept { return _copiesCreated; }
    uint32_t TermsCreated() const noexcept { return _termsCreated; }

    // The GPU time of the two draws, measured with timestamp queries and read a run or two later (never waited for): the pass's own cost,
    // reported separately from the draw hook's CPU cost. `HasGpuMilliseconds` is false until the first measurement has landed.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The march's output and the mask copy of the last run, for the one-frame dump of the developer check (read back by the caller).
    ID3D11ShaderResourceView* TermView() const noexcept { return _termSrv.Get(); }
    ID3D11ShaderResourceView* CopyView() const noexcept { return _copySrv.Get(); }

    // One run at a time. `Run` takes this before it touches the context state, because the pass's two draws go through the game's own draw
    // hooks and a nested entry there would swap the context state under the outer run and double every draw. A second take fails, and the
    // caller leaves the game's draw alone. `Run` always leaves it again; a test takes it directly to pin that the second entry is refused.
    bool TryEnter() noexcept;
    void Leave() noexcept;

    // The GPU timing pair's bookkeeping, for the test that pins the pairing: a run ends a timestamp pair only when it began one, so a
    // stray End is visible as Ends() > Begins().
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureCopy(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height);
    bool EnsureTerm(ID3D11Device* device, uint32_t width, uint32_t height);
    void BindSharedState(ID3D11DeviceContext* context);

    void CollectGpuTiming(ID3D11DeviceContext* context);
    bool BeginGpuTiming(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _running = false; // one run at a time: the pass's own draws must never start a second one
    bool _timingPending = false; // a pair is in the queue: the run that began it ends it, and no other run touches the queries
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> _state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _march;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _combine;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;  // the scalar parameters of both passes
    Microsoft::WRL::ComPtr<ID3D11Buffer> _sun;     // 16 bytes: g_LightParam +0x10
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sunView;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _camera;  // 0x160 bytes: g_CameraParameter +0x000..0x160 (through the projection)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _cameraView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _copy;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _copyRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _copySrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _term;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _termRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _termSrv;
    uint32_t _copyFormat = 0, _copyWidth = 0, _copyHeight = 0;
    uint32_t _termWidth = 0, _termHeight = 0;
    uint32_t _copiesCreated = 0, _termsCreated = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::relight
