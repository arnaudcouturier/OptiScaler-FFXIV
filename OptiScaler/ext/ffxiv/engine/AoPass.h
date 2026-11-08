#pragma once
// Ambient occlusion: the pass itself (ext/ffxiv/engine/shaders/Ao.hlsl).
//
// Two draws on the game's own immediate context, between an explicit save and restore of the state it touches (ext/core/D3D11ExplicitState.h; so no game
// binding is touched by them):
//  1. the horizon search, over the region of the occlusion target that lies inside the render window, reading the reader draw's depth, its
//     G-buffer normal and the camera table, and writing this module's raw occlusion (1 = unoccluded) into an owned R16_FLOAT target of the
//     occlusion texture's size;
//  2. the final draw over the same region: the 4x4 denoise, the strength, the blend with the engine's own occlusion and the 8-bit encoding
//     into an owned B8G8R8A8_UNORM target - the format and size of the texture it replaces.
//
// The owned target starts as a copy of the engine's own occlusion texture, so every texel outside the region keeps the engine's value: the
// readers sample the whole texture and this module only answers for the part its own producer fills.
//
// The camera table (g_CameraParameter, 0x160 bytes through the projection) is copied GPU side into a buffer of this pass with a partial
// CopySubresourceRegion: nothing is ever read back to the CPU, so no frame stalls on a map.
//
// Everything the pass creates lives here and is released with it; nothing of the game is written, cleared or rebound by this class.

#include "ext/ffxiv/engine/AoRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::engine::ao
{
// What one run needs: the reader draw's own resources, its measured sizes and window, and the tuning keys.
struct PassInputs
{
    ID3D11ShaderResourceView* occlusion = nullptr; // the reader's g_SamplerOcclusion (the engine's texture, for the blend)
    ID3D11Texture2D* occlusionTexture = nullptr;   // the resource behind that view, copied into the owned target before the region is written
    ID3D11ShaderResourceView* depth = nullptr;     // the reader's g_SamplerDepth
    ID3D11ShaderResourceView* normal = nullptr;    // the reader's g_SamplerGBuffer (the primary world-space normal)
    ID3D11Buffer* camera = nullptr;                // the reader's g_CameraParameter
    ReaderFacts facts;
    Params params;
};

class Pass
{
  public:
    // Runs the pass for one frame and returns the texture to bind at the occlusion slot, or null when it did not run. A null return is the
    // fail-open answer: the caller keeps the engine's own texture bound and counts the refusal.
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const PassInputs& inputs);

    // Forgets every device object (a device change, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many times the two owned targets were created: one pair per occlusion size, so a test can pin that a frame does not create D3D
    // objects and that a size change does.
    uint32_t TargetsCreated() const noexcept { return _targetsCreated; }

    // The GPU time of the two draws, measured with timestamp queries and read a run or two later (never waited for). `HasGpuMilliseconds` is
    // false until the first measurement has landed.
    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The timing pair's bookkeeping, for the test that pins the pairing: a run ends a pair only when it began one.
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

    // The encoded result of the last run (the texture the reader draws bind) and its resource, for the caller's frame memory.
    ID3D11ShaderResourceView* OutputView() const noexcept { return _outSrv.Get(); }
    ID3D11Texture2D* OutputTexture() const noexcept { return _out.Get(); }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureTargets(ID3D11Device* device, uint32_t width, uint32_t height);
    void CollectGpuTiming(ID3D11DeviceContext* context);
    bool BeginGpuTiming(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ao;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _final;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params; // the scalars of both draws (AoRules.h's Constants)
    Microsoft::WRL::ComPtr<ID3D11Buffer> _camera; // 0x160 bytes: g_CameraParameter +0x000..0x160
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _cameraView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _raw;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _rawRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _rawSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _out;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _outRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _outSrv;
    uint32_t _width = 0, _height = 0;
    uint32_t _targetsCreated = 0;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::engine::ao
