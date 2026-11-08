#pragma once
// Effect meter (METER): the pass (ext/ffxiv/meter/shaders/Meter.hlsl).
//
// One draw per measurement on the context the caller hands in, between an explicit save and restore of the state it touches (ext/core/D3D11ExplicitState.h, the
// stages' own pattern): it reads the stage's input and output with Load (two pixel-shader resources), writes a small float4 target (one texel per block of the
// measured box, MeterRules.h Geometry), copies that target to a staging texture and ends an event query behind the copy. Nothing is ever waited for: Collect()
// asks the event query without flushing and maps the staging texture with D3D11_MAP_FLAG_DO_NOT_WAIT, so a measurement that has not finished is looked at again
// on a later call, and a frame that does not measure pays nothing at all.
//
// The pass changes nothing of the game's state (a pixel-shader slot pair, a constant buffer, the IA/VS/PS/RS/OM state and the viewport come back through the
// guard, also when the draw throws) and nothing of the stage's textures (both are only read).
//
// The register slots and the constant-buffer layout are mirrored by Meter.hlsl; change them together.

#include "ext/ffxiv/meter/MeterRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::meter
{
// What one measurement is asked to do.
struct Request
{
    ID3D11ShaderResourceView* in = nullptr;  // the stage's input (2D, single sample, a float-readable format)
    ID3D11ShaderResourceView* out = nullptr; // the stage's output (the same size)
    StageId id = StageId::Gi;
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f; // the stage's window (diag::StageWindow); zero = the whole texture
    float white = kFallbackWhite;                           // the frame's white point (chain stages only)
    uint32_t seed = 0;                                       // the taps' jitter seed
};

// A finished measurement.
struct Result
{
    Totals totals;
    Geometry geometry;
    StageId id = StageId::Gi;
    uint32_t boxWidth = 0, boxHeight = 0;
    bool haveGpuMs = false;
    double gpuMs = 0.0;      // the draw and the corner copy: the whole GPU cost of one sample
    double gpuDrawMs = 0.0;  // the draw alone
};

class Pass
{
  public:
    // Starts one measurement. False when it did not start (Failure() says why: a view it cannot read, sizes that differ, a box outside the texture, a context
    // that is not the immediate one); nothing is queued then and the game's state is untouched. Never starts while one is in flight.
    bool Start(ID3D11DeviceContext* context, const Request& request);

    // Without waiting: true when the measurement in flight has finished, with its result. A measurement that is not finished yet answers false and stays in
    // flight.
    bool Collect(ID3D11DeviceContext* context, Result& result);

    bool InFlight() const noexcept { return _pending; }

    // Gives up on the measurement in flight (its result never landed): the next Start may begin. The queued work is harmless and its answer is never read.
    void Abandon() noexcept { _pending = false; }
    const char* Failure() const noexcept { return _failure; }

    // Forgets every device object (a device change, a lost device, or the end of the session) and any measurement in flight.
    void Reset();

    // For the tests: the measurements started and collected, and the draws issued by the last Start.
    uint32_t Started() const noexcept { return _started; }
    uint32_t Collected() const noexcept { return _collected; }
    uint32_t DrawsLastStart() const noexcept { return _drawsLastStart; }

    // The formats the pass can read as float4 with Load (UNORM / FLOAT / SNORM): false for UINT, SINT, SRGB, typeless, depth and block-compressed.
    static bool FormatReadable(uint32_t dxgiFormat) noexcept;

  private:
    bool Init(ID3D11Device* device);

    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _target;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _targetRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _staging;
    Microsoft::WRL::ComPtr<ID3D11Query> _done;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _mid, _end;

    bool _pending = false;
    bool _timed = false;
    Geometry _geometry;
    StageId _id = StageId::Gi;
    uint32_t _boxWidth = 0, _boxHeight = 0;
    uint32_t _started = 0, _collected = 0, _drawsLastStart = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::meter
