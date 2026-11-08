#include "pch.h"

#include "ext/ffxiv/engine/SsrAb.h"

#include "ext/core/D3D11ExplicitState.h"
#include "ext/ffxiv/chara/CharaDispatch.h" // the replay marker: the extra draw is another module's replay of a game draw
#include "ext/ffxiv/diag/ShaderCrc.h"
#include "ext/ffxiv/engine/EnginePass.h"   // SetOwnWork: the extra draw is never classified by the draw seam

#include <cmath>
#include <string>

// Screen-space reflections: the frame-exact A/B on the capture frame (SsrAb.h). The rules (what is declined, which draw is kept, the text of the facts
// lines) are in SsrAbRules.h; this file is the D3D side. It is a diagnostic: it runs on the one-frame dump's capture frame only, at the reader draws the
// reflections served (until one has the coverage), and costs one relaxed load of the open mask on every other frame.
namespace ext::ffxiv::engine::ssr::ab
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace es = ext::core::explicit_state;

struct State
{
    DeclineGate gate;
    uint32_t runs = 0;
    uint32_t kept = 0, skippedEmpty = 0, skippedSmall = 0, keptUnknown = 0;
    uint32_t declines[kDeclineCount] {};
    bool queries = true; // the occlusion queries that count a draw's pixels (a test switches them off)
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// Marks the extra draw as another module's replay and as this module's own work, for the length of the call.
struct ExtraDrawScope
{
    ext::ffxiv::chara::ReplayScope replay;
    ExtraDrawScope() { engine::SetOwnWork(true); }
    ~ExtraDrawScope() { engine::SetOwnWork(false); }
    ExtraDrawScope(const ExtraDrawScope&) = delete;
    ExtraDrawScope& operator=(const ExtraDrawScope&) = delete;
};

// One declined draw: counted, written once a session (the log) and in the capture's facts (the comment), and the draw is left as the game made it.
void Refuse(const Call& call, Decline reason)
{
    State& state = S();
    if (uint32_t(reason) < kDeclineCount)
        state.declines[uint32_t(reason)]++;
    if (state.gate.Take(reason))
        LOG_INFO("{}", FormatDeclinedLog(reason));
    ext::ffxiv::diag::AbNote(FormatDeclined(reason, call.framePosition, call.psCrc).c_str());
}

// An occlusion query, or null when queries are off or cannot be made (the draw's coverage is then unknown).
ComPtr<ID3D11Query> MakeQuery(ID3D11Device* device)
{
    ComPtr<ID3D11Query> query;
    if (!S().queries || device == nullptr)
        return query;
    D3D11_QUERY_DESC desc {};
    desc.Query = D3D11_QUERY_OCCLUSION;
    if (FAILED(device->CreateQuery(&desc, &query)))
        query.Reset();
    return query;
}

// The pixels a finished query counted. Flushes and waits for the GPU (a capture frame may stall: F9 hitches by design) for at most half a second;
// false when the query failed or did not finish in time, which the caller reports as "coverage unknown".
bool ReadSamples(ID3D11DeviceContext* context, ID3D11Query* query, uint64_t& samples)
{
    context->Flush();
    const ULONGLONG start = GetTickCount64();
    for (;;)
    {
        const HRESULT hr = context->GetData(query, &samples, sizeof samples, 0);
        if (hr == S_OK)
            return true;
        if (FAILED(hr) || GetTickCount64() - start > 500)
            return false;
        SwitchToThread();
    }
}

// The D3D11 default depth-stencil state, for a draw that has none bound (a null state means the defaults).
D3D11_DEPTH_STENCIL_DESC DefaultDepthStencilDesc()
{
    D3D11_DEPTH_STENCIL_DESC desc {};
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D11_COMPARISON_LESS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    desc.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
    desc.BackFace = desc.FrontFace;
    return desc;
}

// The subresource RT0's view writes (the first mip of the first slice is 0), or 0xFFFFFFFF for a view that is not a plain 2D one.
uint32_t SubresourceOf(const D3D11_RENDER_TARGET_VIEW_DESC& view, uint32_t mipLevels)
{
    switch (view.ViewDimension)
    {
    case D3D11_RTV_DIMENSION_TEXTURE2D: return view.Texture2D.MipSlice;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: return D3D11CalcSubresource(view.Texture2DArray.MipSlice, view.Texture2DArray.FirstArraySlice, mipLevels);
    case D3D11_RTV_DIMENSION_TEXTURE2DMS: return 0;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: return view.Texture2DMSArray.FirstArraySlice;
    default: return 0xFFFFFFFFu;
    }
}

// The extra draw. Never throws: the real draw that follows must run whatever happened here.
void Shadow(Call& call) noexcept
{
    try
    {
        ID3D11DeviceContext* c = call.context;
        ID3D11ShaderResourceView* engineView = call.reader->Original();
        const uint32_t slot = call.reader->Slot();

        ComPtr<ID3D11DeviceContext1> c1;
        if (FAILED(c->QueryInterface(IID_PPV_ARGS(&c1))))
        {
            Refuse(call, Decline::NoContext1);
            return;
        }
        ComPtr<ID3D11Device> device;
        c->GetDevice(&device);
        if (!device)
        {
            Refuse(call, Decline::NoDevice);
            return;
        }
        const bool fl111 = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1;

        // The draw's own RT0 and depth view, read now (the patches that ran before this call do not touch the output merger).
        ID3D11RenderTargetView* rtvRaw = nullptr;
        ID3D11DepthStencilView* dsvRaw = nullptr;
        c->OMGetRenderTargets(1, &rtvRaw, &dsvRaw);
        ComPtr<ID3D11RenderTargetView> gameRtv;
        gameRtv.Attach(rtvRaw);
        ComPtr<ID3D11DepthStencilView> gameDsv;
        gameDsv.Attach(dsvRaw);
        if (!gameRtv)
        {
            Refuse(call, Decline::NoTarget);
            return;
        }
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
        gameRtv->GetDesc(&rtvDesc);
        ComPtr<ID3D11Resource> resource;
        gameRtv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        if (resource)
            resource.As(&texture);
        if (!texture)
        {
            Refuse(call, Decline::NotTexture2D);
            return;
        }
        call.target = texture;

        // The private target: RT0's own description (so the view's description and the subresource indices stay valid), a render target and
        // nothing else. CopyResource below makes it the draw's destination exactly.
        D3D11_TEXTURE2D_DESC textureDesc {};
        texture->GetDesc(&textureDesc);
        D3D11_TEXTURE2D_DESC privateDesc = textureDesc;
        privateDesc.Usage = D3D11_USAGE_DEFAULT;
        privateDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        privateDesc.CPUAccessFlags = 0;
        privateDesc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> privateTexture;
        ComPtr<ID3D11RenderTargetView> privateRtv;
        if (FAILED(device->CreateTexture2D(&privateDesc, nullptr, &privateTexture)) || !privateTexture ||
            FAILED(device->CreateRenderTargetView(privateTexture.Get(), &rtvDesc, &privateRtv)) || !privateRtv)
        {
            Refuse(call, Decline::CreateFailed);
            return;
        }

        // The depth-stencil state the extra draw binds: the game's own tests with depth writes off and the stencil write mask zero.
        ComPtr<ID3D11DepthStencilState> gameDss;
        UINT stencilRef = 0;
        c->OMGetDepthStencilState(&gameDss, &stencilRef);
        D3D11_DEPTH_STENCIL_DESC gameDssDesc = DefaultDepthStencilDesc();
        if (gameDss)
            gameDss->GetDesc(&gameDssDesc);
        const D3D11_DEPTH_STENCIL_DESC shadowDssDesc = ShadowDepthStencilDesc(gameDssDesc);
        ComPtr<ID3D11DepthStencilState> shadowDss;
        if (FAILED(device->CreateDepthStencilState(&shadowDssDesc, &shadowDss)) || !shadowDss)
        {
            Refuse(call, Decline::CreateFailed);
            return;
        }

        const ComPtr<ID3D11Query> extraQuery = MakeQuery(device.Get());

        {
            // Only the output merger and the depth-stencil state are touched through the guard: the water draws are tessellated, so the HS and DS
            // stay bound (the guard's Neutralise() is never called), and the reflection slot (t8) lies beyond the guard's eight shader resources.
            es::Spec spec;
            spec.psSrvCount = 0;
            spec.psSamplerCount = 0;
            spec.psConstantBuffers = 0;
            spec.inputAssembler = false;
            spec.vertexShader = false;
            spec.pixelShader = false;
            spec.rasterizerState = false;
            spec.viewports = false;
            spec.scissors = false;
            spec.blend = false;
            spec.depthStencil = true;
            spec.outputMerger = true;
            spec.hullDomainGeometry = false;
            spec.streamOut = false;
            spec.predication = false;
            es::Guard guard(c, c1.Get(), fl111, spec);

            // The engine's map in the reflection slot for the extra draw; this module's view comes back (the guard below owns it) before the
            // real draw. A slot that holds nothing has no view to put back and no draw to compare: declined.
            ID3D11ShaderResourceView* ours = detail::SwapSlot(c, slot, engineView);
            if (ours == nullptr)
            {
                Refuse(call, Decline::SlotNotBound);
                return;
            }
            BoundReader putOursBack(c, slot, ours);

            // The destination of the extra draw is the draw's own destination: copied immediately before, so blending sees the same pixels.
            c->CopyResource(privateTexture.Get(), texture.Get());

            ID3D11RenderTargetView* targets[1] { privateRtv.Get() };
            c->OMSetRenderTargets(1, targets, gameDsv.Get());
            c->OMSetDepthStencilState(shadowDss.Get(), stencilRef);
            if (extraQuery)
                c->Begin(extraQuery.Get());
            {
                ExtraDrawScope scope;
                call.draw(call.user);
            }
            if (extraQuery)
                c->End(extraQuery.Get());
            call.shadowRan = true;
            S().runs++;
        } // the reflection slot, the output merger and the depth-stencil state are back as the game left them

        // The pixels the extra draw wrote decide whether this draw is the one to measure: the first reader draws of a frame are often small patches
        // or hidden, and a picture of a draw that wrote nothing shows nothing.
        uint64_t extraSamples = 0;
        const bool known = extraQuery && ReadSamples(c, extraQuery.Get(), extraSamples);
        const uint64_t windowPixels = (call.window.width >= 1.0f && call.window.height >= 1.0f)
                                          ? uint64_t(call.window.width) * uint64_t(call.window.height)
                                          : uint64_t(textureDesc.Width) * textureDesc.Height;
        const Coverage coverage = Judge(known, extraSamples, windowPixels);
        ext::ffxiv::diag::AbNote(FormatTry(call.tryIndex, call.framePosition, call.psCrc, coverage, extraSamples, windowPixels).c_str());
        if (!Keep(coverage))
        {
            (coverage == Coverage::Empty ? S().skippedEmpty : S().skippedSmall)++;
            return;
        }
        if (!known)
            S().keptUnknown++;
        S().kept++;

        // Kept: this capture offers no more draws. The target as it stands is the destination of the real draw (the extra draw did not touch it).
        ext::ffxiv::diag::AbAccept();
        ext::ffxiv::diag::AbImage("ssr-ab-before", c, texture.Get(), call.window, call.psCrc);
        ext::ffxiv::diag::AbImage("ssr-ab-engine", c, privateTexture.Get(), call.window, call.psCrc);
        call.accepted = true;
        call.extraKnown = known;
        call.extraSamples = extraSamples;
        call.realQuery = MakeQuery(device.Get());
        call.slot = slot;
        call.targetWidth = textureDesc.Width;
        call.targetHeight = textureDesc.Height;
        call.targetFormat = uint32_t(textureDesc.Format);
        call.depthBound = gameDsv != nullptr;
        call.hadDepthState = gameDss != nullptr;
        call.depthWrites = gameDssDesc.DepthEnable && gameDssDesc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
        call.stencilWriteMask = gameDssDesc.StencilWriteMask;
    }
    catch (...)
    {
    }
}

// The thunk the draw path calls in place of the game's draw: the extra draw, then the real one, exactly once.
void Thunk(void* opaque)
{
    Call& call = *static_cast<Call*>(opaque);
    Shadow(call);
    ID3D11Query* realQuery = call.realQuery.Get(); // only a kept draw has one: the pixels the real draw writes, to compare with the extra draw's
    if (realQuery != nullptr)
        call.context->Begin(realQuery);
    call.draw(call.user);
    if (realQuery != nullptr)
        call.context->End(realQuery);
}
} // namespace

D3D11_DEPTH_STENCIL_DESC ShadowDepthStencilDesc(const D3D11_DEPTH_STENCIL_DESC& in)
{
    D3D11_DEPTH_STENCIL_DESC out = in;
    out.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    out.StencilWriteMask = 0;
    return out;
}

bool Begin(Call& call, ID3D11DeviceContext* context, const BoundReader& reader, DrawFn& draw, void*& user)
{
    if (!ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::Ssr)) // one relaxed load: every frame that is not the capture frame stops here
        return false;
    if (context == nullptr || !reader.Bound() || draw == nullptr)
        return false;
    try
    {
        uint32_t framePosition = 0, tryIndex = 0;
        if (!ext::ffxiv::diag::AbTake(&framePosition, &tryIndex)) // an offer: while the capture has kept no draw and has tries left
            return false;
        call.context = context;
        call.reader = &reader;
        call.framePosition = framePosition;
        call.tryIndex = tryIndex;
        {
            ComPtr<ID3D11PixelShader> shader;
            context->PSGetShader(&shader, nullptr, nullptr);
            call.psCrc = ext::ffxiv::diag::ReadShaderCrc(shader.Get());
        }
        D3D11_VIEWPORT viewport {};
        UINT viewports = 1;
        context->RSGetViewports(&viewports, &viewport);
        if (viewports != 0)
        {
            // The window the draw worked in, floored the way the module's own pass and the stage images floor it.
            call.window.x = std::floor(viewport.TopLeftX);
            call.window.y = std::floor(viewport.TopLeftY);
            call.window.width = viewport.Width;
            call.window.height = viewport.Height;
        }
        if (DiagnosticOn())
        {
            Refuse(call, Decline::SsrDiagnostic);
            return false;
        }

        ComPtr<ID3D11DeviceContext1> context1;
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))) || !context1)
        {
            Refuse(call, Decline::NoContext1);
            return false;
        }
        if (!device)
        {
            Refuse(call, Decline::NoDevice);
            return false;
        }
        const bool fl111 = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1;

        // What the draw's output merger holds: every render target, and the unordered access views (a feature level 11_0 device has eight slots).
        es::Held<ID3D11RenderTargetView, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets;
        es::Held<ID3D11UnorderedAccessView, es::kMaxUavSlots> uavs;
        ID3D11DepthStencilView* depthRaw = nullptr;
        context->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets.p, &depthRaw, 0, fl111 ? es::kMaxUavSlots : 8u,
                                                           uavs.p);
        if (depthRaw != nullptr)
            depthRaw->Release();
        OmFacts facts;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (targets.p[i] != nullptr)
                facts.renderTargetMask |= 1u << i;
        for (ID3D11UnorderedAccessView* uav : uavs.p)
            facts.anyUav = facts.anyUav || uav != nullptr;
        if (targets.p[0] != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            targets.p[0]->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (resource)
                resource.As(&texture);
            facts.texture2D = texture != nullptr;
            if (texture)
            {
                D3D11_TEXTURE2D_DESC textureDesc {};
                texture->GetDesc(&textureDesc);
                facts.samples = textureDesc.SampleDesc.Count;
                D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
                targets.p[0]->GetDesc(&viewDesc);
                facts.subresource = SubresourceOf(viewDesc, textureDesc.MipLevels);
            }
        }
        const Decline verdict = Decide(facts);
        if (verdict != Decline::None)
        {
            Refuse(call, verdict);
            return false;
        }

        call.draw = draw;
        call.user = user;
        call.active = true;
        draw = &Thunk;
        user = &call;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void End(Call& call)
{
    if (!call.active || !call.accepted)
        return;
    call.active = false;
    ext::ffxiv::diag::AbImage("ssr-ab-ours", call.context, call.target.Get(), call.window, call.psCrc);
    // The pixels the real draw wrote, next to the extra draw's: equal counts say the two draws are the same experiment.
    uint64_t realSamples = 0;
    const bool realKnown = call.realQuery && ReadSamples(call.context, call.realQuery.Get(), realSamples);
    ext::ffxiv::diag::AbNote(FormatRan(call.framePosition, call.psCrc, call.slot, call.targetWidth, call.targetHeight, call.targetFormat, call.depthBound,
                                       call.hadDepthState, call.depthWrites, call.stencilWriteMask, call.tryIndex, call.extraKnown ? call.extraSamples : ~0ull,
                                       realKnown ? realSamples : ~0ull)
                                 .c_str());
}

uint32_t Runs() { return S().runs; }
uint32_t Kept() { return S().kept; }
uint32_t SkippedEmpty() { return S().skippedEmpty; }
uint32_t SkippedSmall() { return S().skippedSmall; }
uint32_t KeptUnknown() { return S().keptUnknown; }
void SetCoverageQueryForTest(bool on) { S().queries = on; }

uint32_t Declines(Decline reason) { return uint32_t(reason) < kDeclineCount ? S().declines[uint32_t(reason)] : 0; }

void ResetForTest()
{
    const bool queries = S().queries;
    S() = State {};
    S().queries = queries;
}
} // namespace ext::ffxiv::engine::ssr::ab
