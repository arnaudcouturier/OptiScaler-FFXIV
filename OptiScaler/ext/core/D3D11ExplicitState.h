#pragma once
// Explicit pipeline-state save and restore for the passes that draw from inside a hooked game draw (screen-space reflections, ambient occlusion,
// puddle gloss, puddle tone, light shafts, contact shadows).
//
// Why this replaces SwapDeviceContextState. The swap is not an exchange when it is called: the runtime records the desired state and performs the
// exchange at the next outermost "amortized" entry. A pass that runs inside a draw hook which was itself entered from the runtime's own
// amortized entry (acquire depth >= 1, "nested") therefore never gets its private state, and its setters, its private draw and its final
// OMSetRenderTargets(0) land on the game's live state: the game's HS and DS stay bound under a triangle-list topology, the private draw runs
// with VS=0/PS=0, and the game's own draw that follows is lost. This guard reads what the pass touches through public getters, sets what the
// private state's defaults used to provide, and puts everything back through public setters in its destructor. It does not depend on the
// acquire depth, and it is the same code on the immediate context whether the draw hook was entered nested or not.
//
// What a pass declares (Spec) is exactly what the guard saves and restores; nothing else is read or written, so a pass pays only for the
// categories it changes. HS, DS, GS, stream-out and predication are the state the private device-context state used to provide as "none":
// they are saved when declared, set to none by Neutralise(), and put back only when they were bound. The render targets, the depth view and
// the UAV slots are saved and put back together (OMSetRenderTargets drops every bound UAV, and a UAV with a counter keeps its counter because the
// restore passes an initial count of -1). Constant-buffer ranges are restored exactly through the *GetConstantBuffers1 pair.
//
// Not covered, by construction: state no public getter can read (stream-out byte offsets, the D3D10-flavoured state, a deferred context's
// command list). The guard runs on one thread, the thread that owns the immediate context, exactly like the swap it replaces.

#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace ext::core::explicit_state
{
constexpr UINT kMaxPsSrv = 8;
constexpr UINT kMaxPsSampler = 4;
constexpr UINT kMaxPsConstantBuffers = 2;
constexpr UINT kMaxUavSlots = 64; // D3D11_1_UAV_SLOT_COUNT; a feature level 11_0 device has 8 (D3D11_PS_CS_UAV_REGISTER_COUNT)

// The state categories one pass declares it touches.
struct Spec
{
    UINT psSrvCount = 0;       // pixel-shader resource slots [0, n)
    UINT psSamplerCount = 0;   // pixel-shader sampler slots [0, n) (the passes' shaders only Load: 0)
    UINT psConstantBuffers = 1; // pixel-shader constant-buffer slots [0, n), restored with their exact first/count ranges
    bool inputAssembler = true; // input layout and primitive topology
    bool vertexShader = true;
    bool pixelShader = true;    // with its class instances
    bool rasterizerState = true;
    bool viewports = true;      // the whole array, count included
    bool scissors = false;      // the passes' rasterizers have ScissorEnable false: the rects are neither read nor written
    bool blend = true;          // state, factor and sample mask
    bool depthStencil = true;   // state and stencil reference
    bool outputMerger = true;   // render targets, depth view, UAV slots
    bool hullDomainGeometry = true; // HS, DS, GS: saved, set to none by Neutralise(), put back only when they were bound
    bool streamOut = true;      // bound targets: saved, set to none by Neutralise(), put back only when they were bound
    bool predication = true;    // saved, cleared by Neutralise() when set, put back when it was set
};

template <class T, UINT N> struct Held
{
    T* p[N] {};
    Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    ~Held()
    {
        for (T* x : p)
            if (x != nullptr)
                x->Release();
    }
};

template <class Shader> struct ShaderBinding
{
    Microsoft::WRL::ComPtr<Shader> shader;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES];
    UINT count = D3D11_SHADER_MAX_INTERFACES;
    ShaderBinding() = default;
    ShaderBinding(const ShaderBinding&) = delete;
    ShaderBinding& operator=(const ShaderBinding&) = delete;
    ~ShaderBinding()
    {
        for (UINT i = 0; i < count; ++i)
            if (instances[i] != nullptr)
                instances[i]->Release();
    }
};

// The render targets, the depth view and the UAV slots as read, put back as read. Used by the guard, and on its own around a pass's copy that has
// to take the game's render targets off for a moment (OMSetRenderTargets(0, ...) drops a bound UAV, and so does the rebind that follows it).
class OmSnapshot
{
  public:
    OmSnapshot(ID3D11DeviceContext1* context, bool fl111, bool enabled = true) : _c(context), _uavSlots(fl111 ? kMaxUavSlots : 8u), _enabled(enabled)
    {
        if (_enabled)
            _c->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, _rtv.p, _dsv.p, 0, _uavSlots, _uav.p);
    }
    OmSnapshot(const OmSnapshot&) = delete;
    OmSnapshot& operator=(const OmSnapshot&) = delete;

    void Restore()
    {
        if (!_enabled)
            return;
        UINT rtvCount = 0;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (_rtv.p[i] != nullptr)
                rtvCount = i + 1;
        UINT uavFirst = _uavSlots, uavLast = 0;
        for (UINT i = 0; i < _uavSlots; ++i)
            if (_uav.p[i] != nullptr)
            {
                uavFirst = (std::min)(uavFirst, i);
                uavLast = i;
            }
        if (uavFirst > uavLast) // no UAV was bound: the plain call also leaves none bound
        {
            _c->OMSetRenderTargets(rtvCount, rtvCount ? _rtv.p : nullptr, _dsv.p[0]);
            return;
        }
        UINT keepCounters[kMaxUavSlots];
        for (UINT& k : keepCounters)
            k = static_cast<UINT>(-1);
        _c->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvCount ? _rtv.p : nullptr, _dsv.p[0], uavFirst, uavLast - uavFirst + 1, &_uav.p[uavFirst],
                                                      keepCounters);
    }

  private:
    ID3D11DeviceContext1* _c;
    UINT _uavSlots;
    bool _enabled;
    Held<ID3D11RenderTargetView, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> _rtv;
    Held<ID3D11DepthStencilView, 1> _dsv;
    Held<ID3D11UnorderedAccessView, kMaxUavSlots> _uav;
};

class Guard
{
  public:
    // `context1` is the same object as `context`, asked for ID3D11DeviceContext1 (the constant-buffer ranges and the UAV rebind need it).
    Guard(ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, bool fl111, const Spec& spec)
        : _c(context), _c1(context1), _spec(spec), _om(context1, fl111, spec.outputMerger)
    {
        if (_spec.inputAssembler)
        {
            _c->IAGetInputLayout(&_layout);
            _c->IAGetPrimitiveTopology(&_topology);
        }
        if (_spec.vertexShader)
            _c->VSGetShader(&_vs.shader, _vs.instances, &_vs.count);
        else
            _vs.count = 0;
        if (_spec.hullDomainGeometry)
        {
            _c->HSGetShader(&_hs.shader, _hs.instances, &_hs.count);
            _c->DSGetShader(&_ds.shader, _ds.instances, &_ds.count);
            _c->GSGetShader(&_gs.shader, _gs.instances, &_gs.count);
        }
        else
            _hs.count = _ds.count = _gs.count = 0;
        if (_spec.pixelShader)
            _c->PSGetShader(&_ps.shader, _ps.instances, &_ps.count);
        else
            _ps.count = 0;
        _srvCount = (std::min)(_spec.psSrvCount, kMaxPsSrv);
        if (_srvCount)
            _c->PSGetShaderResources(0, _srvCount, _srv.p);
        _samplerCount = (std::min)(_spec.psSamplerCount, kMaxPsSampler);
        if (_samplerCount)
            _c->PSGetSamplers(0, _samplerCount, _sampler.p);
        _cbCount = (std::min)(_spec.psConstantBuffers, kMaxPsConstantBuffers);
        if (_cbCount)
            _c1->PSGetConstantBuffers1(0, _cbCount, _cb.p, _cbFirst, _cbNum);
        if (_spec.rasterizerState)
            _c->RSGetState(&_rs);
        if (_spec.viewports)
            _c->RSGetViewports(&_viewportCount, _viewports);
        if (_spec.scissors)
            _c->RSGetScissorRects(&_scissorCount, _scissors);
        if (_spec.blend)
            _c->OMGetBlendState(&_blend, _blendFactor, &_sampleMask);
        if (_spec.depthStencil)
            _c->OMGetDepthStencilState(&_depthStencil, &_stencilRef);
        if (_spec.streamOut)
        {
            _c->SOGetTargets(4, _so.p);
            for (ID3D11Buffer* b : _so.p)
                _anyStreamOut = _anyStreamOut || b != nullptr;
        }
        if (_spec.predication)
            _c->GetPredication(&_predicate, &_predicateValue);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() { Restore(); }

    // What the private device-context state's defaults used to provide: no HS, DS or GS, no stream-out target, no predicate. Only what was bound
    // is touched, so a pass on a game state that has none of them pays nothing here.
    void Neutralise()
    {
        if (_neutralised)
            return;
        _neutralised = true;
        if (_hs.shader) _c->HSSetShader(nullptr, nullptr, 0);
        if (_ds.shader) _c->DSSetShader(nullptr, nullptr, 0);
        if (_gs.shader) _c->GSSetShader(nullptr, nullptr, 0);
        if (_anyStreamOut)
        {
            ID3D11Buffer* none[4] {};
            _c->SOSetTargets(4, none, nullptr);
        }
        if (_predicate) _c->SetPredication(nullptr, FALSE);
    }

    // Puts everything back; runs once, at the latest in the destructor (so also on an early return, and while an exception unwinds: it never throws).
    void Restore() noexcept
    {
        if (_restored)
            return;
        _restored = true;
        try
        {
            _om.Restore(); // first: the pass's own target comes off before the game's shader resources come back
            if (_spec.blend)
                _c->OMSetBlendState(_blend.Get(), _blendFactor, _sampleMask);
            if (_spec.depthStencil)
                _c->OMSetDepthStencilState(_depthStencil.Get(), _stencilRef);
            if (_spec.viewports)
                _c->RSSetViewports(_viewportCount, _viewportCount ? _viewports : nullptr);
            if (_spec.scissors)
                _c->RSSetScissorRects(_scissorCount, _scissorCount ? _scissors : nullptr);
            if (_spec.rasterizerState)
                _c->RSSetState(_rs.Get());
            if (_srvCount)
                _c->PSSetShaderResources(0, _srvCount, _srv.p);
            if (_samplerCount)
                _c->PSSetSamplers(0, _samplerCount, _sampler.p);
            for (UINT i = 0; i < _cbCount; ++i)
            {
                if (_cb.p[i] != nullptr)
                    _c1->PSSetConstantBuffers1(i, 1, &_cb.p[i], &_cbFirst[i], &_cbNum[i]);
                else
                    _c->PSSetConstantBuffers(i, 1, &_cb.p[i]);
            }
            if (_spec.pixelShader)
                _c->PSSetShader(_ps.shader.Get(), _ps.instances, _ps.count);
            if (_neutralised) // only what Neutralise() took off is put back
            {
                if (_gs.shader) _c->GSSetShader(_gs.shader.Get(), _gs.instances, _gs.count);
                if (_ds.shader) _c->DSSetShader(_ds.shader.Get(), _ds.instances, _ds.count);
                if (_hs.shader) _c->HSSetShader(_hs.shader.Get(), _hs.instances, _hs.count);
            }
            if (_spec.vertexShader)
                _c->VSSetShader(_vs.shader.Get(), _vs.instances, _vs.count);
            if (_spec.inputAssembler)
            {
                _c->IASetPrimitiveTopology(_topology);
                _c->IASetInputLayout(_layout.Get());
            }
            if (_neutralised && _anyStreamOut)
            {
                UINT keepOffsets[4] { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
                _c->SOSetTargets(4, _so.p, keepOffsets); // the byte offsets are not readable through any getter: -1 keeps the current ones
            }
            if (_neutralised && _predicate)
                _c->SetPredication(_predicate.Get(), _predicateValue);
        }
        catch (...)
        {
            // A hook that throws out of a setter must not escape a destructor; the rest of the restore is lost with it, which is the fault the caller sees.
        }
    }

    bool Restored() const noexcept { return _restored; }

  private:
    ID3D11DeviceContext* _c;
    ID3D11DeviceContext1* _c1;
    Spec _spec;
    bool _restored = false;
    bool _neutralised = false;
    OmSnapshot _om;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> _layout;
    D3D11_PRIMITIVE_TOPOLOGY _topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ShaderBinding<ID3D11VertexShader> _vs;
    ShaderBinding<ID3D11HullShader> _hs;
    ShaderBinding<ID3D11DomainShader> _ds;
    ShaderBinding<ID3D11GeometryShader> _gs;
    ShaderBinding<ID3D11PixelShader> _ps;
    UINT _srvCount = 0, _samplerCount = 0, _cbCount = 0;
    Held<ID3D11ShaderResourceView, kMaxPsSrv> _srv;
    Held<ID3D11SamplerState, kMaxPsSampler> _sampler;
    Held<ID3D11Buffer, kMaxPsConstantBuffers> _cb;
    UINT _cbFirst[kMaxPsConstantBuffers] {}, _cbNum[kMaxPsConstantBuffers] {};
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rs;
    D3D11_VIEWPORT _viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    UINT _viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT _scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    UINT _scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
    FLOAT _blendFactor[4] {};
    UINT _sampleMask = 0xffffffffu;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depthStencil;
    UINT _stencilRef = 0;
    Held<ID3D11Buffer, 4> _so;
    bool _anyStreamOut = false;
    Microsoft::WRL::ComPtr<ID3D11Predicate> _predicate;
    BOOL _predicateValue = FALSE;
};

// ---- FullStateSnapshot ------------------------------------------------------------------------------------------------------------------------
// Everything a caller can read back through public getters, saved at construction and put back by Restore() or the destructor. It is for a call that
// is not a pass of this fork and whose pipeline-state use is not known: the NVIDIA NGX D3D11 CreateFeature / EvaluateFeature of the character preview,
// which used to run inside a private device-context state that the swap threw away afterwards. It is a separate class on purpose: Spec and Guard are not
// touched, so the passes that use them behave and cost exactly as before.
//
// Covers: IA (input layout, topology, 32 vertex-buffer slots with stride and offset, index buffer with format and offset); for each of VS, HS, DS, GS, PS
// and CS the shader with its class instances, SRV slots 0..127, sampler slots 0..15 and constant-buffer slots 0..13 with their exact first/count
// ranges; stream-out targets; rasterizer state, viewports and scissor rects (whole arrays, counts included); blend state, factor and sample mask;
// depth-stencil state and reference; render targets, depth view and PS-side UAV slots (OmSnapshot); the CS UAV slots; predication.
//
// Restore order matters. D3D11 unbinds a resource from the input slots when it is bound as an output, and silently drops an SRV or buffer bind of a
// resource that is still bound as an output. So: outputs first (render targets, depth, PS UAVs, CS UAVs, stream-out targets), then every SRV, then the
// rest. A UAV goes back with initial count -1, so its counter is kept. Stream-out byte offsets are not readable through any getter: -1 keeps the
// current ones. The destructor never throws (a throwing hook loses the rest of the restore). Stack: about 26 KB.
//
// The SRVs of a stage go back in ONE setter call that covers [0, hi], hi being the highest slot that is bound either at capture or at restore time (the
// current state is read with the getters, once per stage), and in no call at all when the stage is the same as at capture. Slots above hi are empty on both
// sides, so the result is exact. The bound matters to the hooks: ResTrack (ResTrack_dx11.cpp NoteGraphicsShaderResources / NoteCSShaderResources) remembers
// the whole slot range of every SetShaderResources call, null views included, and its HUD-fix scan then reads every remembered slot at each draw and
// dispatch until the next Present; a fixed 0..127 restore put all 128 slots of all six stages on that list.
constexpr UINT kMaxVertexBufferSlots = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;                // 32
constexpr UINT kMaxViewportSlots = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;     // 16
constexpr UINT kMaxStageSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;                 // 128
constexpr UINT kMaxStageSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;                    // 16
constexpr UINT kMaxStageConstantBufferSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; // 14

namespace detail
{
#define EXPLICIT_STATE_STAGE_TRAITS(NAME, PFX, SHADER)                                                                                               \
    struct NAME                                                                                                                                      \
    {                                                                                                                                                \
        using Shader = SHADER;                                                                                                                       \
        static void GetShader(ID3D11DeviceContext* c, SHADER** s, ID3D11ClassInstance** i, UINT* n) { c->PFX##GetShader(s, i, n); }                  \
        static void SetShader(ID3D11DeviceContext* c, SHADER* s, ID3D11ClassInstance* const* i, UINT n) { c->PFX##SetShader(s, i, n); }              \
        static void GetSrv(ID3D11DeviceContext* c, ID3D11ShaderResourceView** v) { c->PFX##GetShaderResources(0, kMaxStageSrvSlots, v); }             \
        static void SetSrv(ID3D11DeviceContext* c, UINT n, ID3D11ShaderResourceView* const* v) { c->PFX##SetShaderResources(0, n, v); }              \
        static void GetSampler(ID3D11DeviceContext* c, ID3D11SamplerState** v) { c->PFX##GetSamplers(0, kMaxStageSamplerSlots, v); }                 \
        static void SetSampler(ID3D11DeviceContext* c, ID3D11SamplerState* const* v) { c->PFX##SetSamplers(0, kMaxStageSamplerSlots, v); }           \
        static void GetCb(ID3D11DeviceContext1* c, ID3D11Buffer** b, UINT* f, UINT* n) { c->PFX##GetConstantBuffers1(0, kMaxStageConstantBufferSlots, b, f, n); } \
        static void SetCbRange(ID3D11DeviceContext1* c, UINT slot, ID3D11Buffer* const* b, const UINT* f, const UINT* n) { c->PFX##SetConstantBuffers1(slot, 1, b, f, n); } \
        static void SetCbNone(ID3D11DeviceContext* c, UINT slot, UINT count, ID3D11Buffer* const* b) { c->PFX##SetConstantBuffers(slot, count, b); } \
    };
EXPLICIT_STATE_STAGE_TRAITS(VsStage, VS, ID3D11VertexShader)
EXPLICIT_STATE_STAGE_TRAITS(HsStage, HS, ID3D11HullShader)
EXPLICIT_STATE_STAGE_TRAITS(DsStage, DS, ID3D11DomainShader)
EXPLICIT_STATE_STAGE_TRAITS(GsStage, GS, ID3D11GeometryShader)
EXPLICIT_STATE_STAGE_TRAITS(PsStage, PS, ID3D11PixelShader)
EXPLICIT_STATE_STAGE_TRAITS(CsStage, CS, ID3D11ComputeShader)
#undef EXPLICIT_STATE_STAGE_TRAITS

// 1 + the index of the last non-null entry, 0 when all are null.
template <class V, UINT N> UINT BoundCount(V* const (&views)[N])
{
    UINT n = N;
    while (n > 0 && views[n - 1] == nullptr)
        --n;
    return n;
}

// One programmable stage: its shader with class instances, SRV, sampler and constant-buffer slots.
template <class Stage> class StageSnapshot
{
  public:
    StageSnapshot(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1)
    {
        Stage::GetShader(c, &_shader.shader, _shader.instances, &_shader.count);
        Stage::GetSrv(c, _srv.p);
        _srvBound = BoundCount(_srv.p);
        Stage::GetSampler(c, _sampler.p);
        Stage::GetCb(c1, _cb.p, _cbFirst, _cbNum);
    }
    StageSnapshot(const StageSnapshot&) = delete;
    StageSnapshot& operator=(const StageSnapshot&) = delete;

    // One call [0, hi] with hi = the highest slot bound at capture or now; none when the stage is as it was (see the class comment).
    void RestoreResources(ID3D11DeviceContext* c)
    {
        Held<ID3D11ShaderResourceView, kMaxStageSrvSlots> now;
        Stage::GetSrv(c, now.p);
        const UINT nowBound = BoundCount(now.p);
        const UINT n = nowBound > _srvBound ? nowBound : _srvBound;
        for (UINT i = 0; i < n; ++i)
        {
            if (now.p[i] != _srv.p[i])
            {
                Stage::SetSrv(c, n, _srv.p);
                return;
            }
        }
    }
    void RestoreRest(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1)
    {
        Stage::SetSampler(c, _sampler.p);
        for (UINT i = 0; i < kMaxStageConstantBufferSlots;)
        {
            if (_cb.p[i] != nullptr)
            {
                Stage::SetCbRange(c1, i, &_cb.p[i], &_cbFirst[i], &_cbNum[i]);
                ++i;
                continue;
            }
            UINT run = 1; // a run of empty slots goes back in one call
            while (i + run < kMaxStageConstantBufferSlots && _cb.p[i + run] == nullptr)
                ++run;
            Stage::SetCbNone(c, i, run, &_cb.p[i]);
            i += run;
        }
        Stage::SetShader(c, _shader.shader.Get(), _shader.instances, _shader.count);
    }

  private:
    ShaderBinding<typename Stage::Shader> _shader;
    Held<ID3D11ShaderResourceView, kMaxStageSrvSlots> _srv;
    UINT _srvBound = 0; // 1 + the highest SRV slot that was bound at capture
    Held<ID3D11SamplerState, kMaxStageSamplerSlots> _sampler;
    Held<ID3D11Buffer, kMaxStageConstantBufferSlots> _cb;
    UINT _cbFirst[kMaxStageConstantBufferSlots] {}, _cbNum[kMaxStageConstantBufferSlots] {};
};
} // namespace detail

class FullStateSnapshot
{
  public:
    // `context1` is the same object as `context`, asked for ID3D11DeviceContext1. `enabled` false builds an inert object (nothing is read or written).
    FullStateSnapshot(ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, bool fl111, bool enabled = true)
        : _c(context), _c1(context1), _enabled(enabled && context != nullptr && context1 != nullptr), _uavSlots(fl111 ? kMaxUavSlots : 8u),
          _om(context1, fl111, enabled && context != nullptr && context1 != nullptr)
    {
        if (!_enabled)
            return;
        _vs.emplace(_c, _c1);
        _hs.emplace(_c, _c1);
        _ds.emplace(_c, _c1);
        _gs.emplace(_c, _c1);
        _ps.emplace(_c, _c1);
        _cs.emplace(_c, _c1);
        _c->IAGetInputLayout(&_layout);
        _c->IAGetPrimitiveTopology(&_topology);
        _c->IAGetVertexBuffers(0, kMaxVertexBufferSlots, _vb.p, _vbStride, _vbOffset);
        _c->IAGetIndexBuffer(&_ib, &_ibFormat, &_ibOffset);
        _c->SOGetTargets(4, _so.p);
        for (ID3D11Buffer* b : _so.p)
            _anySo = _anySo || b != nullptr;
        _c->RSGetState(&_rs);
        _c->RSGetViewports(&_viewportCount, _viewports);
        _c->RSGetScissorRects(&_scissorCount, _scissors);
        _c->OMGetBlendState(&_blend, _blendFactor, &_sampleMask);
        _c->OMGetDepthStencilState(&_depthStencil, &_stencilRef);
        _c->CSGetUnorderedAccessViews(0, _uavSlots, _csUav.p);
        _c->GetPredication(&_predicate, &_predicateValue);
    }
    FullStateSnapshot(const FullStateSnapshot&) = delete;
    FullStateSnapshot& operator=(const FullStateSnapshot&) = delete;
    ~FullStateSnapshot() { Restore(); }

    // Puts everything back; runs once, at the latest in the destructor. Outputs before inputs (see above).
    void Restore() noexcept
    {
        if (_restored || !_enabled)
            return;
        _restored = true;
        try
        {
            // 1. outputs
            _om.Restore();
            UINT keepCounters[kMaxUavSlots];
            for (UINT& k : keepCounters)
                k = static_cast<UINT>(-1);
            _c->CSSetUnorderedAccessViews(0, _uavSlots, _csUav.p, keepCounters);
            if (_anySo)
            {
                UINT keepOffsets[4] { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
                _c->SOSetTargets(4, _so.p, keepOffsets);
            }
            else
            {
                ID3D11Buffer* none[4] {};
                _c->SOSetTargets(4, none, nullptr);
            }
            // 2. every shader resource view
            _vs->RestoreResources(_c);
            _hs->RestoreResources(_c);
            _ds->RestoreResources(_c);
            _gs->RestoreResources(_c);
            _ps->RestoreResources(_c);
            _cs->RestoreResources(_c);
            // 3. the rest
            _vs->RestoreRest(_c, _c1);
            _hs->RestoreRest(_c, _c1);
            _ds->RestoreRest(_c, _c1);
            _gs->RestoreRest(_c, _c1);
            _ps->RestoreRest(_c, _c1);
            _cs->RestoreRest(_c, _c1);
            _c->IASetInputLayout(_layout.Get());
            _c->IASetPrimitiveTopology(_topology);
            _c->IASetVertexBuffers(0, kMaxVertexBufferSlots, _vb.p, _vbStride, _vbOffset);
            _c->IASetIndexBuffer(_ib.Get(), _ibFormat, _ibOffset);
            _c->RSSetState(_rs.Get());
            _c->RSSetViewports(_viewportCount, _viewportCount ? _viewports : nullptr);
            _c->RSSetScissorRects(_scissorCount, _scissorCount ? _scissors : nullptr);
            _c->OMSetBlendState(_blend.Get(), _blendFactor, _sampleMask);
            _c->OMSetDepthStencilState(_depthStencil.Get(), _stencilRef);
            _c->SetPredication(_predicate.Get(), _predicateValue);
        }
        catch (...)
        {
            // A hook that throws out of a setter must not escape a destructor; the rest of the restore is lost with it, which is the fault the caller sees.
        }
    }

    bool Restored() const noexcept { return _restored; }

  private:
    ID3D11DeviceContext* _c;
    ID3D11DeviceContext1* _c1;
    bool _enabled;
    UINT _uavSlots;
    bool _restored = false;
    OmSnapshot _om;
    std::optional<detail::StageSnapshot<detail::VsStage>> _vs;
    std::optional<detail::StageSnapshot<detail::HsStage>> _hs;
    std::optional<detail::StageSnapshot<detail::DsStage>> _ds;
    std::optional<detail::StageSnapshot<detail::GsStage>> _gs;
    std::optional<detail::StageSnapshot<detail::PsStage>> _ps;
    std::optional<detail::StageSnapshot<detail::CsStage>> _cs;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> _layout;
    D3D11_PRIMITIVE_TOPOLOGY _topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Held<ID3D11Buffer, kMaxVertexBufferSlots> _vb;
    UINT _vbStride[kMaxVertexBufferSlots] {}, _vbOffset[kMaxVertexBufferSlots] {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> _ib;
    DXGI_FORMAT _ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT _ibOffset = 0;
    Held<ID3D11Buffer, 4> _so;
    bool _anySo = false;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rs;
    D3D11_VIEWPORT _viewports[kMaxViewportSlots] {};
    UINT _viewportCount = kMaxViewportSlots;
    D3D11_RECT _scissors[kMaxViewportSlots] {};
    UINT _scissorCount = kMaxViewportSlots;
    Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
    FLOAT _blendFactor[4] {};
    UINT _sampleMask = 0xffffffffu;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depthStencil;
    UINT _stencilRef = 0;
    Held<ID3D11UnorderedAccessView, kMaxUavSlots> _csUav;
    Microsoft::WRL::ComPtr<ID3D11Predicate> _predicate;
    BOOL _predicateValue = FALSE;
};
} // namespace ext::core::explicit_state
