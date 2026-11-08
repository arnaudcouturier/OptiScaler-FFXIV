#include "pch.h"

#include "ext/ffxiv/enginelight/CbFamily.h"

#include "ext/ffxiv/chara/CharaDispatch.h" // the character preview's replay marker
#include "ext/ffxiv/diag/DrawDiag.h"       // Capturing(): the F9 rows are taken on a capture frame only
#include "ext/ffxiv/engine/EnginePass.h"   // NotifyShadowArmingChanged: the publish re-runs the seam's feature bit
#include "ext/ffxiv/enginelight/AmbientOwner.h"
#include "ext/ffxiv/enginelight/DumpHooks.h"
#include "ext/present/PresentSeams.h"      // the output-frame counter, the frame token the published state is latched at

#include <d3d11.h>
#include <d3d11_1.h>

#include <atomic>
#include <mutex>
#include <vector>

// Engine light controls, writer 1: the device side (ext/ffxiv/enginelight/CbFamily.h). Everything that needs a decision is in CbFamilyRules.h and
// is tested on the host; what is here reads the draw's state (the bound shader's stamp, the targets, the constant-buffer slot), takes the published
// state at the frame boundary, and writes the F9 rows.
namespace ext::ffxiv::enginelight::cb
{
using Microsoft::WRL::ComPtr;

namespace
{
// Stamped on a pixel shader that declares the light draw's or the water surface's names: the packed ShaderKind (CbFamilyRules.h).
constexpr GUID kKindTag { 0x6b0c2e91, 0x4d17, 0x4a83, { 0x8e, 0x55, 0xc1, 0x3a, 0x27, 0xf0, 0x96, 0xd4 } };

struct State
{
    std::mutex mutex;           // the published state, the session
    Published pending;          // the last publish (any thread)
    std::atomic<bool> pendingAsks { false };
    std::atomic<bool> activeAsks { false };
    std::atomic<bool> off { false };
    Session session;
    bool lastMaster = false;
    // Render thread only.
    Latch latch;
    FrameLight frameLight;
    uint32_t moonRuns = 0, waterMoonRuns = 0, waterAmbientRuns = 0, compositeRuns = 0, failures = 0;
    std::atomic<uint32_t> stamped { 0 };
    bool loggedMoon = false, loggedWaterPass = false, loggedWater = false, loggedFailure = false, loggedOff = false;
    // The F9 rows (render thread; the text is taken on the same thread at the capture's write).
    std::string rows;
    uint32_t rowCount = 0;
    bool truncated = false;
    struct Staging
    {
        uint32_t bytes = 0;
        ComPtr<ID3D11Buffer> buffer;
    };
    std::vector<Staging> staging; // two per size: the game's buffer and the patched copy
};

State& S()
{
    static State* state = new State; // never destroyed: no destructor may run under the loader lock
    return *state;
}

// The published state as the frame sees it: latched at the first arm of an output frame.
const Published& ActiveState()
{
    State& s = S();
    const uint64_t token = ext::present::FrameCount();
    if (s.latch.hasFrame && s.latch.frame == token)
        return s.latch.active;
    Published pending;
    {
        std::lock_guard lock(s.mutex);
        pending = s.pending;
    }
    s.latch.Begin(token, pending);
    s.frameLight.Begin(token);
    s.activeAsks.store(!PublishedIdentity(s.latch.active), std::memory_order_relaxed);
    return s.latch.active;
}

void PublishImpl(const Params& params, float weight)
{
    State& s = S();
    const bool before = Asks();
    {
        std::lock_guard lock(s.mutex);
        s.pending.params = params;
        s.pending.weight = weight;
        ++s.pending.sequence;
        s.pendingAsks.store(!PublishedIdentity(s.pending), std::memory_order_relaxed);
        if (params.master && !s.lastMaster)
        {
            // Turning the master back on is the player's re-arm after a stand-down.
            s.session = Session {};
            s.off.store(false, std::memory_order_relaxed);
        }
        s.lastMaster = params.master;
    }
    // The seam's feature bit follows the ask: the draw seam and the pixel-shader tracker run when only this family wants a draw.
    if (Asks() != before)
        ext::ffxiv::engine::NotifyShadowArmingChanged();
}

// Whether the slot binds the whole buffer. The plain Get/Set pair the guard uses would lose a constant-buffer range (REVIEW: ssr-cb-range-restore),
// so a draw that binds a range is left alone.
bool WholeBuffer(ID3D11DeviceContext* context, uint32_t slot, uint32_t byteWidth)
{
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&context1))) || !context1)
        return true; // no ranges exist on a D3D11.0 context
    ID3D11Buffer* buffer = nullptr;
    UINT first = 0, count = 0;
    context1->PSGetConstantBuffers1(slot, 1, &buffer, &first, &count);
    if (buffer != nullptr)
        buffer->Release();
    return first == 0 && (count == 0 || count * 16u >= byteWidth);
}

bool BoundBuffer(ID3D11DeviceContext* context, uint32_t slot, ComPtr<ID3D11Buffer>& buffer, uint32_t& bytes)
{
    if (slot >= eng::kConstantBufferSlots)
        return false;
    ID3D11Buffer* fetched = nullptr;
    context->PSGetConstantBuffers(slot, 1, &fetched);
    buffer.Attach(fetched);
    if (!buffer)
        return false;
    D3D11_BUFFER_DESC desc {};
    buffer->GetDesc(&desc);
    bytes = desc.ByteWidth;
    return true;
}

// The two light buffers the draw writes: format, size and identity of the first two targets.
void InspectTargets(ID3D11DeviceContext* context, LightDrawFacts& facts)
{
    ID3D11RenderTargetView* views[2] { nullptr, nullptr };
    context->OMGetRenderTargets(2, views, nullptr);
    facts.rtCount = (views[0] != nullptr ? 1u : 0u) + (views[1] != nullptr ? 1u : 0u);
    bool r11[2] { false, false };
    for (int i = 0; i < 2; ++i)
    {
        if (views[i] == nullptr)
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC desc {};
        views[i]->GetDesc(&desc);
        r11[i] = desc.Format == DXGI_FORMAT_R11G11B10_FLOAT;
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        (i == 0 ? facts.rt0 : facts.rt1) = reinterpret_cast<uintptr_t>(resource.Get());
        if (i == 0 && resource)
        {
            D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            resource->GetType(&dimension);
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            {
                D3D11_TEXTURE2D_DESC texture {};
                static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&texture);
                facts.width = texture.Width;
                facts.height = texture.Height;
            }
        }
    }
    facts.bothR11G11B10 = r11[0] && r11[1];
    for (ID3D11RenderTargetView* view : views)
        if (view != nullptr)
            view->Release();
}

ShaderKind BoundShaderKind(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    if (!shader)
        return ShaderKind {};
    uint32_t packed = 0;
    UINT bytes = sizeof packed;
    if (FAILED(shader->GetPrivateData(kKindTag, &bytes, &packed)) || bytes != sizeof packed)
        return ShaderKind {};
    return UnpackShaderKind(packed);
}

bool PlanLight(ID3D11DeviceContext* context, const ShaderKind& kind, const Published& active, DrawPlan& plan)
{
    State& s = S();
    LightDrawFacts facts;
    facts.replay = ext::ffxiv::chara::t_replay;
    InspectTargets(context, facts);
    const LightKind lightKind = ClassifyLight(kind, facts, s.frameLight);
    if (lightKind == LightKind::None)
        return false;
    if (lightKind == LightKind::Main)
        s.frameLight.NoteMain(facts); // noted whether or not this frame patches it: the water pass's copy is told apart by it
    const Rgb factor = lightKind == LightKind::Main ? MoonFactors(active.params, active.weight) : WaterMoonFactors(active.params, active.weight);
    plan.count = LightEntries(factor, plan.entries, eng::kMaxScaleEntries);
    if (plan.count == 0)
        return false; // identity: the engine's own draw
    if (!BoundBuffer(context, kind.lightSlot, plan.game, plan.bytes) || !IsLightBufferSize(plan.bytes))
        return false;
    if (!WholeBuffer(context, kind.lightSlot, plan.bytes))
        return false;
    plan.kind = lightKind == LightKind::Main ? PatchKind::Moon : PatchKind::WaterMoon;
    plan.slot = kind.lightSlot;
    plan.factor = factor;
    plan.weight = active.weight;
    plan.mapReaders = true;
    if (lightKind == LightKind::Main && !s.loggedMoon)
    {
        s.loggedMoon = true;
        LOG_INFO("Engine light controls: the main directional light draw is patched at g_LightParam slot {} ({} bytes), moon x({:.3f}, {:.3f}, {:.3f}) at weight {:.2f}",
                 plan.slot, plan.bytes, double(factor.c[0]), double(factor.c[1]), double(factor.c[2]), double(active.weight));
    }
    if (lightKind == LightKind::Water && !s.loggedWaterPass)
    {
        s.loggedWaterPass = true;
        LOG_INFO("Engine light controls: the water pass's copy of the light is patched at slot {} ({} bytes), water moon x({:.3f}, {:.3f}, {:.3f}) at weight {:.2f}",
                 plan.slot, plan.bytes, double(factor.c[0]), double(factor.c[1]), double(factor.c[2]), double(active.weight));
    }
    return true;
}

bool PlanWater(ID3D11DeviceContext* context, const ShaderKind& kind, const Published& active, DrawPlan& plan)
{
    State& s = S();
    WaterDrawFacts facts;
    facts.replay = ext::ffxiv::chara::t_replay;
    if (!BoundBuffer(context, kind.ambientSlot, plan.game, plan.bytes))
        return false;
    facts.boundBytes = plan.bytes;
    facts.wholeBuffer = WholeBuffer(context, kind.ambientSlot, plan.bytes);
    if (!IsWaterCandidate(kind, facts))
        return false;
    plan.count = WaterAmbientEntries(active.params, active.weight, plan.entries, eng::kMaxScaleEntries);
    if (plan.count == 0)
        return false;
    plan.kind = PatchKind::WaterAmbient;
    plan.slot = kind.ambientSlot;
    plan.factor = WaterAmbientFactors(active.params, active.weight);
    plan.weight = active.weight;
    plan.mapReaders = false;
    if (!s.loggedWater)
    {
        s.loggedWater = true;
        LOG_INFO("Engine light controls: the water surface's ambient block is patched at g_AmbientParam slot {} ({} bytes), x({:.3f}, {:.3f}, {:.3f}) at weight {:.2f}",
                 plan.slot, plan.bytes, double(plan.factor.c[0]), double(plan.factor.c[1]), double(plan.factor.c[2]), double(active.weight));
    }
    return true;
}

// ---- the F9 rows ---------------------------------------------------------------------------------------------------------

State::Staging& StagingFor(State& s, uint32_t bytes, uint32_t which)
{
    uint32_t seen = 0;
    for (State::Staging& staging : s.staging)
        if (staging.bytes == bytes && seen++ == which)
            return staging;
    s.staging.emplace_back();
    s.staging.back().bytes = bytes;
    return s.staging.back();
}

const unsigned char* MapCopy(ID3D11DeviceContext* context, State::Staging& staging, ID3D11Buffer* source, uint32_t bytes, D3D11_MAPPED_SUBRESOURCE& mapped)
{
    if (!staging.buffer)
    {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!device)
            return nullptr;
        D3D11_BUFFER_DESC create {};
        create.ByteWidth = bytes;
        create.Usage = D3D11_USAGE_STAGING;
        create.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&create, nullptr, &staging.buffer)))
            return nullptr;
    }
    context->CopyResource(staging.buffer.Get(), source);
    if (FAILED(context->Map(staging.buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
        return nullptr;
    return static_cast<const unsigned char*>(mapped.pData);
}

// How much of each buffer a row records: the light buffer's position/direction/diffuse/specular, the composite's header and first probe, the
// water block's 160 declared bytes.
size_t RowBytes(PatchKind kind, uint32_t bytes) noexcept
{
    size_t want = 64;
    if (kind == PatchKind::Composite)
        want = 64 + 192;
    else if (kind == PatchKind::WaterAmbient)
        want = kWaterAmbientDeclaredBytes;
    return want < bytes ? want : bytes;
}

std::string DumpText()
{
    State& s = S();
    return DumpHeader() + s.rows + (s.truncated ? "# truncated at the row cap\n" : "");
}

void DumpReset()
{
    State& s = S();
    s.rows.clear();
    s.rowCount = 0;
    s.truncated = false;
}

bool OwnsAmbientNow() { return OwnsAmbient(); }

struct Register
{
    Register()
    {
        publishCbFamily = &PublishImpl;
        ownsAmbient = &OwnsAmbientNow;
        DumpHooks::text = &DumpText;
        DumpHooks::reset = &DumpReset;
    }
} g_register;
} // namespace

bool Asks()
{
    State& s = S();
    return !s.off.load(std::memory_order_relaxed) && (s.pendingAsks.load(std::memory_order_relaxed) || s.activeAsks.load(std::memory_order_relaxed));
}

bool WantsDraw(eng::Anchors anchors)
{
    return (eng::IsShaftsLightAnchors(anchors) || eng::IsReflectionAnchors(anchors)) && Asks();
}

bool Plan(ID3D11DeviceContext* context, eng::Anchors anchors, DrawPlan& plan)
{
    if (context == nullptr || !WantsDraw(anchors))
        return false;
    const Published& active = ActiveState();
    if (PublishedIdentity(active))
        return false;
    if (ext::ffxiv::chara::t_replay)
        return false;
    const ShaderKind kind = BoundShaderKind(context);
    if (eng::IsShaftsLightAnchors(anchors) && kind.directional)
        return PlanLight(context, kind, active, plan);
    if (kind.water)
        return PlanWater(context, kind, active, plan);
    return false;
}

uint32_t AmbientEntries(eng::ScaleEntry* out, uint32_t capacity)
{
    if (!Asks())
        return 0;
    const Published& active = ActiveState();
    return CompositeEntries(active.params, active.weight, out, capacity);
}

bool OwnsAmbient()
{
    if (!Asks())
        return false;
    return OwnsAmbient(ActiveState());
}

void NoteRun()
{
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.session.NoteRun();
}

void NoteFailure(const char* why)
{
    State& s = S();
    ++s.failures;
    bool switchedOff = false;
    {
        std::lock_guard lock(s.mutex);
        switchedOff = s.session.NoteFailure();
        if (switchedOff)
            s.off.store(true, std::memory_order_relaxed);
    }
    if (!s.loggedFailure)
    {
        s.loggedFailure = true;
        LOG_WARN("Engine light controls: a patch was refused ({}); the draw ran with the engine's own buffer", why != nullptr ? why : "unknown");
    }
    if (switchedOff && !s.loggedOff)
    {
        s.loggedOff = true;
        LOG_WARN("Engine light controls: {} patches in a row were refused, so the constant-buffer family is off for this session (turn the master off and on to re-arm it)",
                 kFailureLimit);
    }
}

void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size)
{
    if (shader == nullptr || code == nullptr || size < 32)
        return;
    try
    {
        const ShaderKind kind = KindOfShader(code, size);
        const uint32_t packed = PackShaderKind(kind);
        if (packed == 0)
            return;
        if (SUCCEEDED(shader->SetPrivateData(kKindTag, sizeof packed, &packed)))
            S().stamped.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
    }
}

void RecordPatch(ID3D11DeviceContext* context, PatchKind kind, uint32_t slot, ID3D11Buffer* game, ID3D11Buffer* patched, uint32_t bytes,
                 const Rgb& factor, float weight)
{
    State& s = S();
    switch (kind)
    {
    case PatchKind::Moon: ++s.moonRuns; break;
    case PatchKind::WaterMoon: ++s.waterMoonRuns; break;
    case PatchKind::Composite: ++s.compositeRuns; break;
    case PatchKind::WaterAmbient: ++s.waterAmbientRuns; break;
    }
    // Collecting, not merely armed: while the key waits for its delay or its press the diagnostic is "capturing" too, and the staging copies below stall
    // the GPU and would fill the row cap with frames that are not the capture's.
    if (!ext::ffxiv::diag::Collecting() || context == nullptr || game == nullptr || patched == nullptr)
        return;
    if (s.truncated)
        return;
    if (s.rowCount >= kDumpMaxRows || s.rows.size() >= kDumpMaxBytes)
    {
        s.truncated = true;
        return;
    }
    try
    {
        ext::ffxiv::chara::ReplayScope own; // the copies below are not the game's
        const size_t count = RowBytes(kind, bytes);
        D3D11_MAPPED_SUBRESOURCE beforeMap {}, afterMap {};
        State::Staging& beforeStage = StagingFor(s, bytes, 0);
        State::Staging& afterStage = StagingFor(s, bytes, 1);
        const unsigned char* before = MapCopy(context, beforeStage, game, bytes, beforeMap);
        if (before == nullptr)
            return;
        std::vector<unsigned char> beforeCopy(before, before + count);
        context->Unmap(beforeStage.buffer.Get(), 0);
        const unsigned char* after = MapCopy(context, afterStage, patched, bytes, afterMap);
        if (after == nullptr)
            return;
        s.rows += DumpRow(ext::present::FrameCount(), kind, slot, reinterpret_cast<uint64_t>(game), bytes, weight, factor, beforeCopy.data(), after, count);
        ++s.rowCount;
        context->Unmap(afterStage.buffer.Get(), 0);
    }
    catch (...)
    {
    }
}

Status CurrentStatus()
{
    State& s = S();
    Status status;
    status.asks = Asks();
    status.standDown = s.off.load(std::memory_order_relaxed);
    status.moonRuns = s.moonRuns;
    status.waterMoonRuns = s.waterMoonRuns;
    status.waterAmbientRuns = s.waterAmbientRuns;
    status.compositeRuns = s.compositeRuns;
    status.failures = s.failures;
    status.stamped = s.stamped.load(std::memory_order_relaxed);
    return status;
}

void RecordComposite(ID3D11DeviceContext* context, uint32_t slot, ID3D11Buffer* game, ID3D11Buffer* patched, uint32_t bytes)
{
    const Published& active = ActiveState();
    RecordPatch(context, PatchKind::Composite, slot, game, patched, bytes, AmbientFactors(active.params, active.weight), active.weight);
}

std::string CompositeRunLine(uint32_t boundBytes)
{
    const Published& active = ActiveState();
    const Rgb factor = AmbientFactors(active.params, active.weight);
    return std::format("Engine light controls: the composite's ambient SH rows and sky scale ride the ambient dispatch, x({:.3f}, {:.3f}, {:.3f}) at weight {:.2f} over {} bytes",
                       double(factor.c[0]), double(factor.c[1]), double(factor.c[2]), double(active.weight), boundBytes);
}

void Release()
{
    State& s = S();
    s.staging.clear();
}
} // namespace ext::ffxiv::enginelight::cb
