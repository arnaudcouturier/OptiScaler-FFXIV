#pragma once
// Shadow mapping (ext/ffxiv/shadows): the mask lift's own pass (ext/ffxiv/shadows/shaders/ShadowLift.hlsl).
//
// One draw on the game's own immediate context, between an explicit save and restore of the state it touches
// (ext/core/D3D11ExplicitState.h, the pattern of the fork's other passes, so no game binding is left changed by it):
// the game's mask is read directly (there is no intermediate CopyResource - the PS renders mask -> remap -> the
// module's own copy) and returns the copy the caller binds at the
// mask slot for exactly one draw, the same contract ext/ffxiv/relight/ContactShadowsApply.cpp gives. When contact
// shadows IS armed, the remap is a stage of its combine instead (one copy per frame there), and this class is not
// asked - the contact seam does the whole mask work.
//
// Everything this class creates lives here and is released with it; nothing of the game is written, cleared or
// rebound; the copy's size follows the engine's allocation and a size change re-creates it.

#include "ext/ffxiv/shadows/ShadowsRules.h"

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ext::ffxiv::shadows
{
// What one run needs: the draw's own bindings, measured sizes and the effective values of the frame.
struct MaskInputs
{
    ID3D11ShaderResourceView* mask = nullptr; // the draw's screen-space mask (the same resource the light draw reads)
    uint32_t maskFormat = 0;                  // the format the draw samples the mask in (the family's typed member)
    uint32_t maskWidth = 0, maskHeight = 0;   // the mask's allocation (the copy is created at exactly that size)
    float window[4] { 0.0f, 0.0f, 0.0f, 0.0f }; // x, y, width, height of the render window in mask pixels
    float lift = 0.0f;                        // the effective remap lift (0 = identity: the caller does not call)
    float softness = 0.0f;                    // the tent's reach in mask pixels (0 = no tent)
};

class MaskLiftPass
{
  public:
    // Runs the pass for one light draw and returns the copy that replaces the mask's binding, or null when the pass
    // did not run. A null return is the fail-open answer: the caller keeps the game's own binding and counts the
    // decline with the failure reason it reads from Failure().
    ID3D11ShaderResourceView* Run(ID3D11DeviceContext* context, const MaskInputs& inputs);

    // Forgets every device object (a device change, or the end of the session).
    void Reset();

    // The reason of the last failure, for the log; null while the pass is healthy.
    const char* Failure() const noexcept { return _failure; }

    // How many copies this instance has created: one per allocation size, so a test can pin that a frame's two light
    // draws (or a dynamic-resolution change inside the same allocation) create no D3D object per frame.
    uint32_t CopiesCreated() const noexcept { return _copiesCreated; }

    // The GPU time of the lift draw, measured with a timestamp pair that a run or two LATER reads with
    // D3D11_ASYNC_GETDATA_DONOTFLUSH (never a wait, and never a flush either - the FRESH wave's rule). `Has` is
    // false until the first measurement has landed.
    // One run at a time: the lift's own draw goes through the same draw hooks, and a nested entry (a draw hook
    // that fires inside this module's own draw) must never start a second run against the same context state.
    bool TryEnter() noexcept
    {
        if (_running)
            return false;
        _running = true;
        return true;
    }
    void Leave() noexcept { _running = false; }

    bool HasGpuMilliseconds() const noexcept { return _haveGpuMs; }
    double GpuMilliseconds() const noexcept { return _gpuMs; }

    // The GPU timing pair's bookkeeping, for the pin: a run that began a pair is the only run that may end it, so a
    // stray End is visible as Ends() > Begins() (a run that found the previous pair still in the queue times
    // nothing).
    uint32_t GpuTimingBegins() const noexcept { return _timingBegins; }
    uint32_t GpuTimingEnds() const noexcept { return _timingEnds; }

  private:
    bool Init(ID3D11Device* device);
    bool EnsureCopy(ID3D11Device* device, uint32_t format, uint32_t width, uint32_t height);
    void CollectGpuTiming(ID3D11DeviceContext* context);
    bool BeginGpuTiming(ID3D11DeviceContext* context);

    bool _running = false;
    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11Query> _disjoint, _begin, _end;
    bool _timingPending = false;
    bool _haveGpuMs = false;
    double _gpuMs = 0.0;
    uint32_t _timingBegins = 0, _timingEnds = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _params;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _copy;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _copyRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _copySrv;
    uint32_t _copyFormat = 0, _copyWidth = 0, _copyHeight = 0;
    uint32_t _copiesCreated = 0;
    const char* _failure = nullptr;
};
} // namespace ext::ffxiv::shadows
