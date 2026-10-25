#pragma once
// D3D11 helpers of the ReShade probe (S10 package D0): a guarded call (SEH), a full pipeline state block, format
// families for the sRGB / linear render target views, and a sampled before/after comparison of a render target through
// a staging texture. No ReShade or ImGui types. The first use of __try/__except in ext/ (docs: S10-D0-LOG.md).

#include "ext/reshade/ReShadeProbe.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ext::reshade::d3d11
{
// Runs fn(arg) inside __try/__except(EXCEPTION_EXECUTE_HANDLER). 0 = returned normally; otherwise the exception code
// (an access violation is 0xC0000005, a C++ exception that crosses the frame is 0xE06D7363). The function holds no C++
// objects, which is what makes SEH legal next to the project's /EHsc.
uint32_t GuardedCall(void (*fn)(void*), void* arg);

// ONE process-wide mutex that serializes only OUR passes (the depth copy and the five G-buffer remaps) when they swap the immediate
// context into a private ID3DDeviceContextState (CharaEnhancePass.cpp and CharaGuides.cpp also swap private context states, without it):
// begin_effects can arrive from the HudLess UI-boundary render_effects call and from ReShade's own pass in Present while the depth pass
// runs on the evaluate thread, and a swap-in .. draw .. swap-back that is interleaved with another one puts the wrong state back.
// Recursive (a Release that recovers a swap back holds it already) and TIMED: third-party ReShade / REST callbacks fire during our draws
// and their locks cannot be audited, so a holder that does not come back costs the pass kContextStateLockTimeoutMs, not a freeze (the
// pass is skipped before any state swap and tries again at its next call). Held ONLY around the swap-in, draw, unbind and swap-back (and
// the recovery of a swap back), never around view creation, publishing, or a call out to ReShade. Heap allocated and never destroyed:
// a pass may still run while the process shuts down.
inline constexpr int kContextStateLockTimeoutMs = 30;
std::recursive_timed_mutex& ContextStateMutex();

// ---- formats ------------------------------------------------------------------------------------------------------------
struct FormatFamily
{
    bool known = false;               // false = no sRGB/linear pair known for this format
    DXGI_FORMAT linear = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srgb = DXGI_FORMAT_UNKNOWN;
};
FormatFamily FamilyOf(DXGI_FORMAT format);
bool SameFamily(DXGI_FORMAT a, DXGI_FORMAT b); // identical, or both members of one known family

// ---- pipeline state -----------------------------------------------------------------------------------------------------
// Everything a ReShade effect pass may change on the immediate context: IA, every shader stage with its constant
// buffers / resources / samplers, stream output, rasterizer, output merger and the compute UAVs. Capture AddRefs, Apply
// puts it back, Release drops the references.
class StateBlock
{
  public:
    StateBlock() = default;
    ~StateBlock() { Release(); }
    StateBlock(const StateBlock&) = delete;
    StateBlock& operator=(const StateBlock&) = delete;

    void Capture(ID3D11DeviceContext* context);
    void Apply(ID3D11DeviceContext* context);
    void Release();

  private:
    struct Stage
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader;
        std::array<ID3D11Buffer*, 14> constantBuffers {};
        std::array<ID3D11ShaderResourceView*, 128> resources {};
        std::array<ID3D11SamplerState*, 16> samplers {};
    };
    static constexpr int kStages = 6; // VS HS DS GS PS CS

    bool _captured = false;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> _layout;
    D3D11_PRIMITIVE_TOPOLOGY _topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    std::array<ID3D11Buffer*, 16> _vertexBuffers {};
    std::array<UINT, 16> _strides {};
    std::array<UINT, 16> _offsets {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> _indexBuffer;
    DXGI_FORMAT _indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT _indexOffset = 0;
    std::array<Stage, kStages> _stages {};
    std::array<ID3D11UnorderedAccessView*, 8> _csUavs {};
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    UINT _viewportCount = 0;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> _viewports {};
    UINT _scissorCount = 0;
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> _scissors {};
    Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
    std::array<float, 4> _blendFactor {};
    UINT _sampleMask = 0xFFFFFFFF;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depthStencil;
    UINT _stencilRef = 0;
    std::array<ID3D11RenderTargetView*, 8> _renderTargets {};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> _depthView;
};

// ---- before/after comparison of a render target ------------------------------------------------------------------------
// Copies the texture into a staging texture and samples every `kStride`-th pixel in x and y. A GPU stall (CopyResource +
// Map): used only on the one manual probe frame, and timed separately.
class TargetSnapshot
{
  public:
    static constexpr uint32_t kStride = 16;

    // Stores the sampled pixels of `resource`. False (with a note) for unsupported formats or failures.
    bool Capture(ID3D11DeviceContext* context, ID3D11Resource* resource, std::string& note);
    // Samples `resource` again and compares with the stored pixels; releases the stored data.
    probe::PixelDiff CaptureAndDiff(ID3D11DeviceContext* context, ID3D11Resource* resource);
    void Release();

  private:
    bool Sample(ID3D11DeviceContext* context, ID3D11Resource* resource, std::vector<float>& out, std::string& note);

    std::vector<float> _before;
    std::string _beforeNote;
    bool _haveBefore = false;
    double _beforeReadbackUs = 0;
};
} // namespace ext::reshade::d3d11
