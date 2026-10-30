#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaMotion.h"
#include "ext/ffxiv/chara/CharaTags.h"

#include <array>
#include <atomic>

namespace ext::ffxiv::chara::motion
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr size_t kDepthStates = 16;
constexpr uintptr_t kNoStateKey = 1; // no depth state bound

struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width = 0;
    uint32_t height = 0;
};

Target g_target;
ComPtr<ID3D11BlendState> g_blend;
DepthStateCache<ComPtr<ID3D11DepthStencilState>, kDepthStates> g_depthStates;
OpenQuerySet g_queries;

std::atomic<uint64_t> g_replays { 0 };
std::array<std::atomic<uint64_t>, 12> g_skips {};

ReplayResult Skipped(Skip why)
{
    ++g_skips[static_cast<size_t>(why)];
    return { Outcome::NotReplayed, why };
}

bool CreateTarget(ID3D11Device* device, uint32_t width, uint32_t height)
{
    Target t;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &t.texture)) ||
        FAILED(device->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv)) ||
        FAILED(device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv)))
        return false;
    t.width = width;
    t.height = height;
    g_target = std::move(t);
    return true;
}

// Writes only render target 3, no blending: the replay keeps the pixel shader's raw output.
bool EnsureBlend(ID3D11Device* device)
{
    if (g_blend != nullptr)
        return true;
    D3D11_BLEND_DESC bd {};
    bd.IndependentBlendEnable = TRUE;
    for (UINT i = 0; i < 8; ++i)
    {
        auto& rt = bd.RenderTarget[i];
        rt.BlendEnable = FALSE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = i == 3 ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    }
    return SUCCEEDED(device->CreateBlendState(&bd, &g_blend));
}

int EntryOf(const table::Snapshot& snap, ID3D11View* view)
{
    if (view == nullptr)
        return kEntryNone;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    return table::IndexOfPointer(snap, reinterpret_cast<uintptr_t>(resource.Get()));
}

uint32_t BufferBytes(ID3D11Buffer* buffer)
{
    if (buffer == nullptr)
        return 0;
    D3D11_BUFFER_DESC bd {};
    buffer->GetDesc(&bd);
    return bd.ByteWidth;
}

struct SavedState
{
    std::array<ID3D11RenderTargetView*, 8> rtvs {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] {};
    UINT sampleMask = 0xFFFFFFFF;
    ID3D11DepthStencilState* depthState = nullptr;
    UINT stencilRef = 0;

    SavedState() = default;
    SavedState(const SavedState&) = delete;
    SavedState& operator=(const SavedState&) = delete;

    ~SavedState()
    {
        for (auto* v : rtvs)
        {
            if (v != nullptr)
                v->Release();
        }
        if (dsv != nullptr)
            dsv->Release();
        if (blend != nullptr)
            blend->Release();
        if (depthState != nullptr)
            depthState->Release();
    }
};
} // namespace

bool EnsureTarget(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (device == nullptr || width == 0 || height == 0)
        return false;
    if (g_target.texture != nullptr && g_target.width == width && g_target.height == height)
        return true;
    if (!CreateTarget(device, width, height))
    {
        g_target = Target {};
        return false;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ClearTarget(context.Get());
    return true;
}

void ClearTarget(ID3D11DeviceContext* context)
{
    if (context == nullptr || g_target.rtv == nullptr)
        return;
    const FLOAT zero[4] {};
    ReplayScope scope;
    context->ClearRenderTargetView(g_target.rtv.Get(), zero);
}

ID3D11ShaderResourceView* TargetSrv() { return g_target.srv.Get(); }
ID3D11Texture2D* TargetTexture() { return g_target.texture.Get(); }

DepthConvention BoundDepthConvention(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11DepthStencilState> state;
    UINT ref = 0;
    context->OMGetDepthStencilState(&state, &ref);
    if (state == nullptr)
        return ConventionOf(false, false, D3D11_COMPARISON_LESS);
    D3D11_DEPTH_STENCIL_DESC desc {};
    state->GetDesc(&desc);
    return ConventionOf(true, desc.DepthEnable != FALSE, desc.DepthFunc);
}

ReplayResult Replay(const DrawView& view, DrawCall draw)
{
    ID3D11DeviceContext* context = view.context;
    if (context == nullptr || view.table == nullptr)
        return Skipped(Skip::NotImmediate);

    ReplayFacts facts;
    facts.ownDraw = t_replay;
    facts.openQueries = static_cast<uint32_t>(g_queries.Count());
    facts.replayAllowedByQueries = !g_queries.Blind();
    facts.psGeometryTag = (view.roles & RoleGeometry) != 0;
    facts.wideShape = ext::Cfg().ffxiv.chara.previewReplayWideShape.value_or_default();

    // Cheap exits before any state is read.
    if (facts.ownDraw)
        return Skipped(Skip::OwnDraw);
    if (facts.openQueries != 0 || !facts.replayAllowedByQueries)
        return Skipped(Skip::OpenQuery);
    if (g_target.rtv == nullptr || g_target.width != view.facts.targetWidth || g_target.height != view.facts.targetHeight)
        return Skipped(Skip::NoTarget);

    SavedState saved;
    context->OMGetRenderTargets(static_cast<UINT>(saved.rtvs.size()), saved.rtvs.data(), &saved.dsv);
    for (size_t i = 0; i < 5; ++i)
    {
        facts.rtvBound[i] = saved.rtvs[i] != nullptr;
        facts.rtvEntry[i] = EntryOf(*view.table, saved.rtvs[i]);
    }
    facts.dsvBound = saved.dsv != nullptr;
    facts.rtv0Samples = view.facts.samples;

    // Unordered access views and stream-output targets forbid the second draw.
    {
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] {};
        context->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 0,
                                                           D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        for (auto* uav : uavs)
        {
            if (uav != nullptr)
            {
                facts.anyUav = true;
                uav->Release();
            }
        }
        ID3D11Buffer* streamOut[4] {};
        context->SOGetTargets(4, streamOut);
        for (auto* buffer : streamOut)
        {
            if (buffer != nullptr)
            {
                facts.anyStreamOut = true;
                buffer->Release();
            }
        }
    }

    if (!facts.psGeometryTag)
    {
        ComPtr<ID3D11Buffer> vsCb, psCb;
        context->VSGetConstantBuffers(0, 1, &vsCb);
        context->PSGetConstantBuffers(2, 1, &psCb);
        facts.vsCb0Bytes = BufferBytes(vsCb.Get());
        facts.psCb2Bytes = BufferBytes(psCb.Get());
    }

    const ReplayDecision decision = DecideReplay(facts);
    if (!decision.replay)
    {
        ReplayResult refused = Skipped(decision.skip);
        if (decision.skip == Skip::UnknownShader)
        {
            refused.shape.vsCb0Bytes = facts.vsCb0Bytes;
            refused.shape.psCb2Bytes = facts.psCb2Bytes;
            refused.shape.rtvEntry = facts.rtvEntry;
        }
        return refused;
    }
    if (decision.tagShader && view.pixelShader != nullptr)
        SetRoles(view.pixelShader, view.roles | RoleGeometry);

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device == nullptr || !EnsureBlend(device.Get()))
        return Skipped(Skip::NoTarget);

    // Replay depth state: the game's state with writes off, cached per source state.
    context->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);
    context->OMGetDepthStencilState(&saved.depthState, &saved.stencilRef);
    D3D11_DEPTH_STENCIL_DESC source {};
    if (saved.depthState != nullptr)
        saved.depthState->GetDesc(&source);
    const uintptr_t key = saved.depthState != nullptr ? reinterpret_cast<uintptr_t>(saved.depthState) : kNoStateKey;
    ComPtr<ID3D11DepthStencilState>* cached = g_depthStates.Find(key, source);
    if (cached == nullptr)
    {
        ComPtr<ID3D11DepthStencilState> created;
        const D3D11_DEPTH_STENCIL_DESC desc = ReplayDepthDesc(saved.depthState != nullptr, source);
        if (FAILED(device->CreateDepthStencilState(&desc, &created)))
            return Skipped(Skip::DepthStateFailed);
        cached = g_depthStates.Insert(key, source, created);
        if (cached == nullptr)
            return Skipped(Skip::DepthStateFailed); // cache full: leave the draw native
    }

    // Render target 3 only, depth tested but not written, then everything back.
    ID3D11RenderTargetView* targets[4] { nullptr, nullptr, nullptr, g_target.rtv.Get() };
    context->OMSetRenderTargets(4, targets, saved.dsv);
    const FLOAT factor[4] {};
    context->OMSetBlendState(g_blend.Get(), factor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(cached->Get(), saved.stencilRef);
    {
        ReplayScope scope;
        draw();
    }
    context->OMSetRenderTargets(static_cast<UINT>(saved.rtvs.size()), saved.rtvs.data(), saved.dsv);
    context->OMSetBlendState(saved.blend, saved.blendFactor, saved.sampleMask);
    context->OMSetDepthStencilState(saved.depthState, saved.stencilRef);

    const uint64_t n = ++g_replays;
    if (n == 1 || n % 600 == 0)
        LOG_INFO("CharaView signed motion: replayed G-buffer draws {}", n);
    return { Outcome::Replayed, Skip::None };
}

void OnQueryBegin(ID3D11Asynchronous* query)
{
    if (query == nullptr)
        return;
    bool tracked = true; // an object that is not a query (a counter) is tracked
    ComPtr<ID3D11Query> asQuery;
    if (SUCCEEDED(query->QueryInterface(IID_PPV_ARGS(&asQuery))))
    {
        D3D11_QUERY_DESC desc {};
        asQuery->GetDesc(&desc);
        tracked = QueryTypeCounts(desc.Query);
    }
    g_queries.Begin(reinterpret_cast<uintptr_t>(query), tracked);
}

void OnQueryEnd(ID3D11Asynchronous* query) { g_queries.End(reinterpret_cast<uintptr_t>(query)); }
void OnFrameEnd() { g_queries.OnFrameEnd(); }
void MarkQueriesBlind() { g_queries.MarkBlind(); }
void OnMaintain(uint64_t nowMs) { g_queries.Maintain(nowMs); }

uint64_t ReplayCount() { return g_replays.load(std::memory_order_relaxed); }
uint64_t SkipCount(Skip reason) { return g_skips[static_cast<size_t>(reason)].load(std::memory_order_relaxed); }

void Release()
{
    g_target = Target {};
    g_blend.Reset();
    g_depthStates.Clear();
    g_queries.Clear();
}
} // namespace ext::ffxiv::chara::motion
