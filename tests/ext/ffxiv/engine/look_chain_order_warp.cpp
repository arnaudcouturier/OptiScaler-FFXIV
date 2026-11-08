// INT1: the tone-map chain's order AS BUILT, driven at run time on D3D11 WARP.
//
//     god-rays march -> light bounce (GI) -> night grade (L1) -> atmosphere (L2) -> depth of field -> t0
//
// What is REAL here: ChainHooks.h's dispatchers (the only code that writes the order), the night grade (NightGradeDraw.cpp and its pass: it answers, grades and
// draws the game's call exactly as in the product) and the light bounce's own off and declining exits (GiDraw.cpp: a real stage with its key off, and with its key
// on but nothing captured, so it asks the dispatchers the way it does in a frame it cannot serve). What is a STAND-IN: the light bounce's ANSWER (its pass needs the
// engine's G-buffer), the atmosphere (its captures - depth, camera, fog - are the engine's) and depth of field. A stand-in follows exactly the contract
// ChainHooks.h states for its link and that atmo_pins / dof_pins pin in the real sources: on, and not yet asked this frame, it answers its own texture and composes
// the stage behind it on that texture; otherwise it hands the stage behind it the texture it was handed (or the owner turn); an owner stand-in that answers binds its
// result at t0 for the game's one draw and puts the game's view back. The seam model (SeamDispatch) is the engine pass dispatch's order, and look_chain_order_pins
// pins that this function's call order equals EnginePass.cpp's.
//
// The light bounce's real DECLINE has two shapes, and both are driven (INT2). "declines" (GiMode::Declines) arms the real stage but never feeds it the frame's composite, so its
// rendered-frame count never moves: its first call of the session declines for real and every later call meets its ALREADY-ANSWERED exit. "declines, first call"
// (GiMode::DeclinesFirstCall) feeds the composite to the real stage at every frame (as the engine pass dispatch does), so every call of every entry is the stage's FIRST call of
// its frame and declines for real: the stage reaches its pass (no G-buffer was captured, so it skips the frame - the session log's "the frame skipped" line is the camera flavour
// of the same exit), returns no answer, and the forward after the decline (the dispatchers: the night grade, the atmosphere, depth of field) is what these cells exist for. Each
// such call is proven to be a first call by the stage's own status (one more skipped frame and one more "no G-buffer" per entry, the frame count moved by one).
//
// What each cell checks (light bounce: real-off | real-on-but-declining (already answered, first call) | answering; night grade: key off | key on at weight 0 (a day frame) | key on at night;
// atmosphere: not linked | linked and off | linked and on; depth of field: off | on; and three entries: the chained texture hook, the march's owner hook, the seam):
//   - every enabled stage receives the previous ENABLED stage's texture (the night grade the light bounce's answer when it has one, else the engine's scene; the
//     atmosphere the night grade's answer when it has one, else what the night grade was handed; depth of field the atmosphere's, else the one before) - the contents
//     are checked, not only the pointers: the night grade's output is its INPUT graded (the lamp's half-float bits are the input's own, so a stage handed the wrong
//     texture shows);
//   - a stage that is off (key off, or weight 0 on a day frame) is a bit-exact bypass: the next stage is handed the very same view, no pass runs, nothing is bound;
//   - the game's tone-map draw happens exactly once on every entry, the owner-return rule holds (an entry answers true exactly when it drew, never twice, and the
//     caller's draw happens once when none did), the draw sees the last answering stage's texture at t0 and the game's own view comes back;
//   - the answer the chained entry returns is the last answering stage's texture (null when none answered: the hold stands);
//   - one answer a frame: a second tone-shaped call in the same frame is not answered again by any stage.
#include "check.h"

#include "ext/ffxiv/engine/ChainHooks.h"
#include "ext/ffxiv/engine/EnginePass.h" // OwnWork: this harness defines its two members (the engine module is not linked)
#include "ext/ffxiv/engine/Gi.h"
#include "ext/ffxiv/enginelight/EngineLight.h"
#include "ext/ffxiv/shadows/ShadowsHooks.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
namespace ng = ext::ffxiv::enginelight;
namespace eng = ext::ffxiv::engine;
namespace gi = ext::ffxiv::engine::gi;
namespace sh = ext::ffxiv::shafts;
namespace sd = ext::ffxiv::shadows;

#define CHECK_SUCCEEDED(call) CHECK(SUCCEEDED(call))

// ---- the neighbours this harness is instead of ---------------------------------------------------------------------------------------

namespace ext::ffxiv::engine
{
OwnWork::OwnWork() {}
OwnWork::~OwnWork() {}
} // namespace ext::ffxiv::engine

namespace ext::present
{
uint64_t FrameCount() noexcept { return 1; }
} // namespace ext::present

namespace
{
constexpr uint32_t kWidth = 160;
constexpr uint32_t kHeight = 96;
constexpr size_t kStone = (size_t(48) * kWidth + 20) * 4; // a moonlit-stone texel (left half)
constexpr size_t kLamp = (size_t(48) * kWidth + 120) * 4; // a lamp texel (right half)

uint16_t HalfOf(float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = int32_t((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
        return uint16_t(sign);
    if (exponent >= 31)
        return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exponent) << 10) | (mantissa >> 13));
}

float FromHalf(uint16_t value)
{
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    const uint32_t mantissa = value & 0x3FFu;
    if (exponent == 0)
        return sign * std::ldexp(float(mantissa), -24);
    return sign * std::ldexp(float(1024 + mantissa), int(exponent) - 25);
}

// The injected wall clock: 1000 ticks a second.
int64_t g_ticks = 0;
int64_t Clock() { return g_ticks; }
void Advance(double seconds) { g_ticks += int64_t(seconds * 1000.0 + 0.5); }

// The night curve's stub: what the shadow module's hook would answer.
sd::NightNow g_now;
sd::NightNow NightNowStub(ID3D11DeviceContext*) { return g_now; }
void NightFeedStub(bool) {}
const sd::NightNow kNightReading { true, 1.0f, 1, 2, true, 0.5f, true, 0.1f, true }; // the moon arm, full curve
const sd::NightNow kDayReading { true, 0.0f, 1, 1, true, 0.6f, true, 1.5f, true };   // a daylight reading: weight 0

void NoDraw(void*) {}

struct Fixture;
Fixture* g_fx = nullptr;
uint64_t g_frame = 1; // the harness's rendered-frame counter (the stand-ins' one-answer gate)

// One texture of the harness: the half-float RGBA scene with a stone on the left and a lamp on the right.
struct Plate
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    std::vector<uint16_t> data;
};

// A stand-in stage behind the dispatchers (see the file header for the contract it follows).
struct Stand
{
    bool linked = true;
    bool on = false;
    Plate plate;               // its own answer
    uint64_t answeredFrame = ~0ull;
    int calls = 0;             // hooks asked (the texture hook and the owner hook, in any role)
    int answers = 0;           // times it answered
    ID3D11ShaderResourceView* saw = nullptr; // the texture / scene the LAST hook call was handed
};
Stand g_atmo, g_dof;

void BindDraw(ID3D11DeviceContext* context, ID3D11ShaderResourceView* bind, eng::ChainDrawFn draw, void* user)
{
    ID3D11ShaderResourceView* original = nullptr;
    context->PSGetShaderResources(0, 1, &original);
    context->PSSetShaderResources(0, 1, &bind);
    draw(user);
    context->PSSetShaderResources(0, 1, &original);
    if (original != nullptr)
        original->Release();
}

ID3D11ShaderResourceView* StandChain(Stand& s, eng::ChainSceneFn next, ID3D11DeviceContext* context, const sh::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    ++s.calls;
    s.saw = scene;
    if (s.on && s.answeredFrame != g_frame)
    {
        s.answeredFrame = g_frame;
        ++s.answers;
        ID3D11ShaderResourceView* ours = s.plate.view.Get();
        if (next != nullptr)
            if (ID3D11ShaderResourceView* composed = next(context, facts, ours))
                return composed;
        return ours;
    }
    return next != nullptr ? next(context, facts, scene) : nullptr;
}

bool StandOwner(Stand& s, eng::ChainSceneFn nextScene, eng::ToneOwnerFn nextOwner, ID3D11DeviceContext* context, const sh::ToneMapFacts& facts,
                ID3D11ShaderResourceView* scene, eng::ChainDrawFn draw, void* user)
{
    ++s.calls;
    s.saw = scene;
    if (s.on && s.answeredFrame != g_frame)
    {
        s.answeredFrame = g_frame;
        ++s.answers;
        ID3D11ShaderResourceView* ours = s.plate.view.Get();
        ID3D11ShaderResourceView* composed = nextScene != nullptr ? nextScene(context, facts, ours) : nullptr;
        BindDraw(context, composed != nullptr ? composed : ours, draw, user);
        return true;
    }
    return nextOwner != nullptr ? nextOwner(context, facts, scene, draw, user) : false;
}

// The atmosphere's pair: it asks depth of field (and only depth of field) behind it, as AtmoDraw.cpp does.
ID3D11ShaderResourceView* AtmoChain(ID3D11DeviceContext* context, const sh::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    return StandChain(g_atmo, eng::chainDofScene, context, facts, scene);
}
bool AtmoOwner(ID3D11DeviceContext* context, const sh::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, eng::ChainDrawFn draw, void* user)
{
    return StandOwner(g_atmo, eng::chainDofScene, eng::toneMapDofOwner, context, facts, scene, draw, user);
}
ID3D11ShaderResourceView* DofChain(ID3D11DeviceContext* context, const sh::ToneMapFacts& facts, ID3D11ShaderResourceView* scene)
{
    return StandChain(g_dof, nullptr, context, facts, scene);
}
bool DofOwner(ID3D11DeviceContext* context, const sh::ToneMapFacts& facts, ID3D11ShaderResourceView* scene, eng::ChainDrawFn draw, void* user)
{
    return StandOwner(g_dof, nullptr, nullptr, context, facts, scene, draw, user);
}

void LinkAtmo(bool linked)
{
    g_atmo.linked = linked;
    eng::chainAtmoScene = linked ? &AtmoChain : nullptr;
    eng::toneMapAtmoOwner = linked ? &AtmoOwner : nullptr;
}

sh::ToneMapFacts g_facts;
// The seam role of a stand-in: at the tone map only, no owner turn after it (a false lets the dispatch reach the next stage's own seam entry).
bool StandSeam(Stand& s, eng::ChainSceneFn nextScene, eng::Anchors anchors, ID3D11DeviceContext* context, eng::ChainDrawFn draw, void* user)
{
    if (!s.linked || (anchors & eng::kShaftsToneMapBit) == 0)
        return false;
    if (!s.on || s.answeredFrame == g_frame)
        return false;
    ID3D11ShaderResourceView* scene = nullptr;
    context->PSGetShaderResources(0, 1, &scene);
    ++s.calls;
    s.saw = scene;
    s.answeredFrame = g_frame;
    ++s.answers;
    ID3D11ShaderResourceView* ours = s.plate.view.Get();
    ID3D11ShaderResourceView* composed = nextScene != nullptr ? nextScene(context, g_facts, ours) : nullptr;
    BindDraw(context, composed != nullptr ? composed : ours, draw, user);
    if (scene != nullptr)
        scene->Release();
    return true;
}

// The light bounce's ANSWER (its pass needs the engine's G-buffer): a texture of its own, the next stages composed on it through the dispatcher the real stage asks.
enum class GiMode
{
    Off,      // the real stage, key off
    Declines, // the real stage, key on, nothing captured, never fed the composite: its first call declines, then it is already answered every frame
    DeclinesFirstCall, // the real stage, key on, fed the composite every frame: every call is its first of the frame and declines for real (INT2)
    Answers   // the stand-in: it answers its own texture and composes the stages behind it on it, through the dispatcher
};
GiMode g_giMode = GiMode::Off;
Plate g_giPlate;
int g_giAnswers = 0;
ID3D11ShaderResourceView* GiAnswerChain(ID3D11DeviceContext* context, const sh::ToneMapFacts& facts, ID3D11ShaderResourceView*)
{
    ++g_giAnswers;
    ID3D11ShaderResourceView* ours = g_giPlate.view.Get();
    if (ID3D11ShaderResourceView* composed = eng::chainAfterGiScene(context, facts, ours))
        return composed;
    return ours;
}
bool GiAnswerOwner(ID3D11DeviceContext* context, const sh::ToneMapFacts& facts, ID3D11ShaderResourceView*, eng::ChainDrawFn draw, void* user)
{
    ++g_giAnswers;
    ID3D11ShaderResourceView* ours = g_giPlate.view.Get();
    ID3D11ShaderResourceView* composed = eng::chainAfterGiScene(context, facts, ours);
    BindDraw(context, composed != nullptr ? composed : ours, draw, user);
    return true;
}

// ---- the seam model: the engine pass dispatch's order (look_chain_order_pins pins this function's call order against EnginePass.cpp's) ----------------------------
bool Seam_Gi(ID3D11DeviceContext* context, eng::Anchors anchors, eng::ChainDrawFn draw, void* user)
{
    if (g_giMode == GiMode::Answers)
    {
        if ((anchors & eng::kShaftsToneMapBit) == 0)
            return false;
        ID3D11ShaderResourceView* scene = nullptr;
        context->PSGetShaderResources(0, 1, &scene);
        const bool drew = GiAnswerOwner(context, g_facts, scene, draw, user);
        if (scene != nullptr)
            scene->Release();
        return drew;
    }
    return gi::OnSeamDraw(context, anchors, draw, user); // the real stage
}
bool Seam_Night(ID3D11DeviceContext* context, eng::Anchors anchors, eng::ChainDrawFn draw, void* user)
{
    return ng::OnSeamDraw(context, anchors, draw, user); // the real stage
}
bool Seam_Atmo(ID3D11DeviceContext* context, eng::Anchors anchors, eng::ChainDrawFn draw, void* user)
{
    return StandSeam(g_atmo, eng::chainDofScene, anchors, context, draw, user);
}
bool Seam_Dof(ID3D11DeviceContext* context, eng::Anchors anchors, eng::ChainDrawFn draw, void* user)
{
    return StandSeam(g_dof, nullptr, anchors, context, draw, user);
}
bool SeamDispatch(ID3D11DeviceContext* context, eng::Anchors anchors, eng::ChainDrawFn draw, void* user)
{
    if (Seam_Gi(context, anchors, draw, user))
        return true;
    if (Seam_Night(context, anchors, draw, user))
        return true;
    if (Seam_Atmo(context, anchors, draw, user))
        return true;
    if (Seam_Dof(context, anchors, draw, user))
        return true;
    return false;
}

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Plate scene;                                   // the engine's scene the tone map reads: stone 0.10 / 0.11 / 0.13, lamp 2.0 / 1.8 / 1.2
    ComPtr<ID3D11Texture2D> toneTarget;
    ComPtr<ID3D11RenderTargetView> sceneRtv, toneRtv;

    static std::vector<uint16_t> Data(float stone[3], float lamp[3])
    {
        std::vector<uint16_t> input(size_t(kWidth) * kHeight * 4, 0);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const bool isLamp = x >= kWidth / 2;
                const size_t at = (size_t(y) * kWidth + x) * 4;
                for (int c = 0; c < 3; ++c)
                    input[at + c] = HalfOf(isLamp ? lamp[c] : stone[c]);
                input[at + 3] = HalfOf(1.0f);
            }
        return input;
    }

    bool MakePlate(Plate& plate, std::vector<uint16_t> data)
    {
        plate.data = std::move(data);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA initial { plate.data.data(), kWidth * 8, 0 };
        return SUCCEEDED(device->CreateTexture2D(&desc, &initial, &plate.texture)) && SUCCEEDED(device->CreateShaderResourceView(plate.texture.Get(), nullptr, &plate.view));
    }

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;
        float stone[3] { 0.10f, 0.11f, 0.13f };
        float lamp[3] { 2.0f, 1.8f, 1.2f };
        if (!MakePlate(scene, Data(stone, lamp)))
            return false;
        // the light bounce's answer: a different picture (its stone brighter but still well inside the grade, its lamp another value: the lamp's bits are KEPT by the
        // grade, so the night grade's output tells which of the two it was handed)
        float giStone[3] { 0.15f, 0.16f, 0.18f };
        float giLamp[3] { 1.5f, 1.4f, 1.1f };
        if (!MakePlate(g_giPlate, Data(giStone, giLamp)))
            return false;
        float flat[3] { 0.3f, 0.3f, 0.3f };
        if (!MakePlate(g_atmo.plate, Data(flat, flat)) || !MakePlate(g_dof.plate, Data(flat, flat)))
            return false;
        D3D11_TEXTURE2D_DESC desc {};
        scene.texture->GetDesc(&desc);
        if (FAILED(device->CreateRenderTargetView(scene.texture.Get(), nullptr, &sceneRtv)))
            return false;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &toneTarget)) || FAILED(device->CreateRenderTargetView(toneTarget.Get(), nullptr, &toneRtv)))
            return false;
        return true;
    }

    // The frame's composite: the scene is the one render target (the seam records it).
    void Composite()
    {
        ID3D11RenderTargetView* targets[1] { sceneRtv.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        CHECK(!ng::OnSeamDraw(context.Get(), eng::kShaftsCompositeBit, NoDraw, nullptr)); // a composite is never the night grade's draw
        // The engine pass dispatch hands the composite to every armed stage: the light bounce's rendered-frame count opens there. Only the first-call mode feeds it (the
        // "declines" mode is the already-answered exit by construction).
        if (g_giMode == GiMode::DeclinesFirstCall)
            CHECK(!gi::OnSeamDraw(context.Get(), eng::kShaftsCompositeBit, NoDraw, nullptr));
    }
    // The tone map's own state: its target bound, the scene at t0, the window as the viewport.
    void ToneMapState()
    {
        ID3D11RenderTargetView* targets[1] { toneRtv.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        ID3D11ShaderResourceView* views[1] { scene.view.Get() };
        context->PSSetShaderResources(0, 1, views);
        ID3D11Buffer* none[1] { nullptr };
        context->PSSetConstantBuffers(0, 1, none);
        const D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
    }
    sh::ToneMapFacts Facts() const
    {
        sh::ToneMapFacts facts;
        facts.renderTargets = 1;
        facts.targetWidth = kWidth;
        facts.targetHeight = kHeight;
        facts.targetFormat = 10;
        facts.targetIsScene = false;
        facts.sceneFormat = 10;
        facts.sceneWidth = kWidth;
        facts.sceneHeight = kHeight;
        facts.sceneSampled = true;
        facts.sceneIsScene = true;
        facts.window = sh::Window { 0.0f, 0.0f, float(kWidth), float(kHeight) };
        return facts;
    }

    std::vector<uint16_t> ReadRgba(ID3D11ShaderResourceView* view)
    {
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        resource.As(&texture);
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Texture2D> staging;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CHECK_SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        CHECK_SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint16_t> out(size_t(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(out.data() + size_t(y) * desc.Width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, desc.Width * 8);
        context->Unmap(staging.Get(), 0);
        return out;
    }
};

// The game's draw call: records what is at t0 while it runs (the view and what it holds).
struct DrawProbe
{
    ID3D11DeviceContext* context = nullptr;
    int calls = 0;
    ID3D11ShaderResourceView* boundAtDraw = nullptr; // weak: the view at t0 while the draw ran
    std::vector<uint16_t> content;                   // ... and its pixels
};
void ProbeDraw(void* user)
{
    DrawProbe& probe = *static_cast<DrawProbe*>(user);
    ++probe.calls;
    ID3D11ShaderResourceView* view = nullptr;
    probe.context->PSGetShaderResources(0, 1, &view);
    probe.boundAtDraw = view;
    if (view != nullptr)
    {
        probe.content = g_fx->ReadRgba(view);
        view->Release();
    }
}

enum class NightMode
{
    KeyOff,
    Day,   // key on, weight 0: the engine's own picture, no pass
    Night, // key on, weight ~1
};

struct Cell
{
    GiMode gi;
    NightMode night;
    bool atmoLinked;
    bool atmoOn;
    bool dofOn;
};

const char* Name(GiMode mode)
{
    switch (mode)
    {
    case GiMode::Off: return "gi-off";
    case GiMode::Declines: return "gi-declines";
    case GiMode::DeclinesFirstCall: return "gi-declines-first-call";
    case GiMode::Answers: return "gi-answers";
    }
    return "gi";
}
const char* Name(NightMode mode) { return mode == NightMode::KeyOff ? "night-keyoff" : (mode == NightMode::Day ? "night-day" : "night-night"); }

void SetStandIns(const Cell& cell)
{
    LinkAtmo(cell.atmoLinked);
    g_atmo.on = cell.atmoOn;
    g_dof.on = cell.dofOn;
    g_atmo.answeredFrame = g_dof.answeredFrame = ~0ull;
    g_atmo.calls = g_dof.calls = 0;
    g_atmo.answers = g_dof.answers = 0;
    g_atmo.saw = g_dof.saw = nullptr;
    g_giAnswers = 0;
}

// What the model says the cell does.
struct Model
{
    bool giAnswers, nightAnswers, atmoAnswers, dofAnswers;
    bool any() const { return giAnswers || nightAnswers || atmoAnswers || dofAnswers; }
};
Model ModelOf(const Cell& cell)
{
    Model model {};
    model.giAnswers = cell.gi == GiMode::Answers;
    model.nightAnswers = cell.night == NightMode::Night;
    model.atmoAnswers = cell.atmoLinked && cell.atmoOn;
    model.dofAnswers = cell.dofOn;
    return model;
}

void BeginFrame(Fixture& fx)
{
    ++g_frame;
    Advance(1.0 / 60.0);
    const uint64_t giFramesBefore = gi::CurrentStatus().frames;
    fx.Composite();
    if (g_giMode == GiMode::DeclinesFirstCall)
        CHECK_EQ(gi::CurrentStatus().frames, giFramesBefore + 1); // the composite opened the real stage's count for this frame
    fx.ToneMapState();
    g_facts = fx.Facts();
}

// The first-call mode's proof: the real light bounce's first call of the frame ran its pass's gate and declined for real - one more skipped frame, one more "no G-buffer", nothing
// answered, nothing counted against its stand-down, and it is still armed (a streak of declines must not have switched it off before the cells that follow).
struct GiBefore
{
    gi::Status status;
    GiBefore() : status(gi::CurrentStatus()) {}
    void CheckFirstCallDeclined(const char* label) const
    {
        const gi::Status after = gi::CurrentStatus();
        const bool ok = after.skipped == status.skipped + 1 && after.noGBuffer == status.noGBuffer + 1 && after.runs == status.runs && after.declined == status.declined && after.armed;
        if (!ok)
            std::printf("    [%s] skipped %u->%u noGBuffer %u->%u runs %u->%u declined %u->%u armed=%d\n", label, status.skipped, after.skipped, status.noGBuffer, after.noGBuffer,
                        status.runs, after.runs, status.declined, after.declined, after.armed ? 1 : 0);
        CHECK(ok);
    }
};

constexpr float kManualFactor = 0.5f; // the factor the harness arms the exposure stage with (manual mode)

// The exposure stage's output, told apart from its input: the harness arms the stage with the manual factor 0.5, so at full night weight every pixel is its input's half -
// the stone AND the lamp (the retired luminance grade kept the lamp's bits; this stage scales the whole stored scene).
void CheckGraded(Fixture& fx, ID3D11ShaderResourceView* output, const std::vector<uint16_t>& input, const char* what)
{
    const std::vector<uint16_t> graded = fx.ReadRgba(output);
    const bool stoneHalf = std::fabs(FromHalf(graded[kStone + 1]) - FromHalf(input[kStone + 1]) * kManualFactor) <= 0.02f * FromHalf(input[kStone + 1]) + 1e-5f;
    const bool lampHalf = std::fabs(FromHalf(graded[kLamp + 1]) - FromHalf(input[kLamp + 1]) * kManualFactor) <= 0.02f * FromHalf(input[kLamp + 1]) + 1e-5f;
    if (!stoneHalf || !lampHalf)
        std::printf("    [%s] stoneHalf=%d lampHalf=%d\n", what, stoneHalf ? 1 : 0, lampHalf ? 1 : 0);
    CHECK(stoneHalf);
    CHECK(lampHalf);
}

// The harness arms the exposure stage in its manual mode with every light arm at the engine's value: the stage answers a texture (the whole stored scene x the factor, eased
// with the night weight) with no exposure reading and no settle, which is all the chain-order cells need from it. Key off = the master off.
ng::Params KeyOnParams()
{
    ng::Params p;
    p.moonIntensity = p.ambientIntensity = p.waterMoon = p.waterAmbient = 1.0f;
    p.lampIntensity = 1.0f;
    p.exposureMode = ng::kExposureManual;
    p.exposureManual = kManualFactor;
    return p;
}
ng::Params KeyOffParams()
{
    ng::Params p = KeyOnParams();
    p.master = false;
    return p;
}

// ---- the three entries ---------------------------------------------------------------------------------------------------------------------

void RunChained(Fixture& fx, const Cell& cell)
{
    char label[160];
    std::snprintf(label, sizeof label, "chained %s %s atmo=%d/%d dof=%d", Name(cell.gi), Name(cell.night), cell.atmoLinked ? 1 : 0, cell.atmoOn ? 1 : 0, cell.dofOn ? 1 : 0);
    SetStandIns(cell);
    BeginFrame(fx);
    const Model model = ModelOf(cell);
    const uint32_t runsBefore = ng::CurrentStatus().runs;
    ID3D11ShaderResourceView* incoming = fx.scene.view.Get();
    ID3D11ShaderResourceView* afterGi = model.giAnswers ? g_giPlate.view.Get() : incoming;
    const std::vector<uint16_t>& afterGiData = model.giAnswers ? g_giPlate.data : fx.scene.data;

    const GiBefore giBefore;
    ID3D11ShaderResourceView* answer = model.giAnswers ? GiAnswerChain(fx.context.Get(), g_facts, incoming) : eng::chainGiScene(fx.context.Get(), g_facts, incoming);

    const int failuresBefore = g_failures;
    if (cell.gi == GiMode::DeclinesFirstCall)
        giBefore.CheckFirstCallDeclined(label);
    // depth of field is asked exactly once and the atmosphere, when linked, exactly once
    CHECK_EQ(g_dof.calls, 1);
    CHECK_EQ(g_atmo.calls, cell.atmoLinked ? 1 : 0);
    // the night grade's output is what the next linked stage was handed
    ID3D11ShaderResourceView* nightOut = cell.atmoLinked ? g_atmo.saw : g_dof.saw;
    if (model.nightAnswers)
    {
        CHECK(nightOut != nullptr && nightOut != afterGi && nightOut != incoming);
        if (nightOut != nullptr)
            CheckGraded(fx, nightOut, afterGiData, label);
        CHECK_EQ(ng::CurrentStatus().runs, runsBefore + 1);
    }
    else
    {
        CHECK(nightOut == afterGi); // a bit-exact bypass: the very same view, no copy
        CHECK_EQ(ng::CurrentStatus().runs, runsBefore);
    }
    // depth of field is handed the previous ENABLED stage's texture
    ID3D11ShaderResourceView* expectedForDof = model.atmoAnswers ? g_atmo.plate.view.Get() : nightOut;
    CHECK(g_dof.saw == expectedForDof);
    // the answer: the last answering stage's texture; none answered = the hold stands (null)
    ID3D11ShaderResourceView* expectedAnswer = model.dofAnswers ? g_dof.plate.view.Get()
                                                                : (model.atmoAnswers ? g_atmo.plate.view.Get() : (model.nightAnswers ? nightOut : (model.giAnswers ? g_giPlate.view.Get() : nullptr)));
    CHECK(answer == expectedAnswer);
    CHECK_EQ(g_atmo.answers, model.atmoAnswers ? 1 : 0);
    CHECK_EQ(g_dof.answers, model.dofAnswers ? 1 : 0);
    // a chained entry never binds
    ID3D11ShaderResourceView* bound = nullptr;
    fx.context->PSGetShaderResources(0, 1, &bound);
    CHECK(bound == incoming);
    if (bound != nullptr)
        bound->Release();
    if (g_failures != failuresBefore)
        std::printf("    ^ in cell: %s\n", label);
}

void RunOwner(Fixture& fx, const Cell& cell, bool viaSeam)
{
    char label[160];
    std::snprintf(label, sizeof label, "%s %s %s atmo=%d/%d dof=%d", viaSeam ? "seam" : "owner-hook", Name(cell.gi), Name(cell.night), cell.atmoLinked ? 1 : 0, cell.atmoOn ? 1 : 0,
                  cell.dofOn ? 1 : 0);
    SetStandIns(cell);
    BeginFrame(fx);
    const Model model = ModelOf(cell);
    const uint32_t runsBefore = ng::CurrentStatus().runs;
    ID3D11ShaderResourceView* incoming = fx.scene.view.Get();
    ID3D11ShaderResourceView* afterGi = model.giAnswers ? g_giPlate.view.Get() : incoming;
    const std::vector<uint16_t>& afterGiData = model.giAnswers ? g_giPlate.data : fx.scene.data;

    DrawProbe probe;
    probe.context = fx.context.Get();
    const int failuresBefore = g_failures;
    const GiBefore giBefore;
    bool entryDrew;
    if (viaSeam)
        entryDrew = SeamDispatch(fx.context.Get(), eng::kShaftsToneMapBit, ProbeDraw, &probe);
    else if (model.giAnswers)
        entryDrew = GiAnswerOwner(fx.context.Get(), g_facts, incoming, ProbeDraw, &probe);
    else
        entryDrew = eng::toneMapGiOwner(fx.context.Get(), g_facts, incoming, ProbeDraw, &probe);
    if (cell.gi == GiMode::DeclinesFirstCall)
        giBefore.CheckFirstCallDeclined(label);
    const int callsAtEntry = probe.calls;
    const std::vector<uint16_t> contentAtEntry = probe.content;
    ID3D11ShaderResourceView* boundAtEntry = probe.boundAtDraw;
    // the owner-return rule: an entry answers true exactly when it drew, and never draws twice
    CHECK(callsAtEntry <= 1);
    CHECK_EQ(entryDrew, callsAtEntry == 1);
    CHECK_EQ(entryDrew, model.any());
    if (!entryDrew)
    {
        // the caller draws the game's call once, on the binding it already has
        ProbeDraw(&probe);
        CHECK(probe.boundAtDraw == incoming);
    }
    CHECK_EQ(probe.calls, 1); // the game's tone-map draw happened exactly once
    if (entryDrew)
    {
        // the draw saw the last answering stage's texture at t0
        if (model.dofAnswers)
            CHECK(boundAtEntry == g_dof.plate.view.Get());
        else if (model.atmoAnswers)
            CHECK(boundAtEntry == g_atmo.plate.view.Get());
        else if (model.nightAnswers)
        {
            CHECK(boundAtEntry != nullptr && boundAtEntry != incoming && boundAtEntry != afterGi);
            if (!contentAtEntry.empty())
            {
                const bool stoneHalf = std::fabs(FromHalf(contentAtEntry[kStone + 1]) - FromHalf(afterGiData[kStone + 1]) * kManualFactor) <= 0.02f * FromHalf(afterGiData[kStone + 1]) + 1e-5f;
                const bool lampHalf = std::fabs(FromHalf(contentAtEntry[kLamp + 1]) - FromHalf(afterGiData[kLamp + 1]) * kManualFactor) <= 0.02f * FromHalf(afterGiData[kLamp + 1]) + 1e-5f;
                CHECK(stoneHalf);
                CHECK(lampHalf);
            }
        }
        else
            CHECK(boundAtEntry == g_giPlate.view.Get());
    }
    // the night grade ran exactly when it was the one that answered; a bypassed night grade ran nothing
    CHECK_EQ(ng::CurrentStatus().runs, runsBefore + (model.nightAnswers ? 1u : 0u));
    // each stand-in answered at most once and only when enabled
    CHECK(g_atmo.answers <= (model.atmoAnswers ? 1 : 0));
    CHECK(g_dof.answers <= (model.dofAnswers ? 1 : 0));
    // the game's own view is back at t0
    ID3D11ShaderResourceView* bound = nullptr;
    fx.context->PSGetShaderResources(0, 1, &bound);
    CHECK(bound == incoming);
    if (bound != nullptr)
        bound->Release();
    if (g_failures != failuresBefore)
        std::printf("    ^ in cell: %s\n", label);
}

void RunAllRoles(Fixture& fx, const Cell& cell)
{
    RunChained(fx, cell);
    RunOwner(fx, cell, false);
    RunOwner(fx, cell, true);
}
} // namespace

int main()
{
    Fixture fx;
    g_fx = &fx;
    if (!fx.Create())
    {
        std::printf("look_chain_order_warp: no WARP device\n");
        return 1;
    }
    // The wiring a product build gets from its static initialisers: the real stages registered their pairs; the harness installs the stand-ins'.
    CHECK(eng::chainGiScene == &gi::ChainScene && eng::toneMapGiOwner == &gi::ToneMapOwner);
    CHECK(eng::chainNightScene == &ng::ChainScene && eng::toneMapNightOwner == &ng::ToneMapOwner);
    eng::chainDofScene = &DofChain;
    eng::toneMapDofOwner = &DofOwner;
    LinkAtmo(true);
    sd::Hooks::nightNow = &NightNowStub;
    sd::Hooks::nightFeed = &NightFeedStub;
    ng::SetClockForTest(&Clock, 1000);

    int cells = 0;
    const GiMode gis[] { GiMode::Off, GiMode::Declines, GiMode::DeclinesFirstCall, GiMode::Answers };
    const NightMode nights[] { NightMode::KeyOff, NightMode::Day, NightMode::Night };
    for (const NightMode night : nights)
    {
        std::printf("-- %s --\n", Name(night));
        ng::Configure(KeyOffParams());
        g_now = night == NightMode::Night ? kNightReading : kDayReading;
        if (night != NightMode::KeyOff)
        {
            ng::Configure(KeyOnParams());
            // ramp the weight on the injected clock (the stage after is whatever the stand-ins are; nothing here is checked)
            SetStandIns(Cell { GiMode::Off, night, false, false, false });
            for (int i = 0; i < 60 * 6; ++i)
            {
                Advance(1.0 / 60.0);
                fx.Composite();
                fx.ToneMapState();
                eng::chainNightScene(fx.context.Get(), fx.Facts(), fx.scene.view.Get());
            }
            if (night == NightMode::Night)
                CHECK(ng::CurrentStatus().weight > 0.95f);
            else
                CHECK_EQ(ng::CurrentStatus().weight, 0.0f);
        }
        CHECK_EQ(ng::Armed(), night != NightMode::KeyOff);
        for (const GiMode giMode : gis)
        {
            g_giMode = giMode;
            const bool realStageOn = giMode == GiMode::Declines || giMode == GiMode::DeclinesFirstCall;
            gi::OnConfigLoaded(false); // a clean light bounce for each mode: key off resets its counters and its answered-frame memory
            gi::OnConfigLoaded(realStageOn); // the real stage: key off, or key on with nothing captured
            CHECK_EQ(gi::Armed(), realStageOn); // the "declines" cells really run an ARMED light bounce (else they would be "off" cells under another name)
            for (int atmo = 0; atmo < 3; ++atmo) // 0 = not linked, 1 = linked and off, 2 = linked and on
                for (int dof = 0; dof < 2; ++dof)
                {
                    const Cell cell { giMode, night, atmo != 0, atmo == 2, dof == 1 };
                    RunAllRoles(fx, cell);
                    ++cells;
                }
        }
        gi::OnConfigLoaded(false);
    }
    std::printf("-- %d cells x 3 entries run --\n", cells);

    std::printf("-- the sky's sun draw (kShaftsSunParamBit): the light bounce and the night grade pass it on untouched, so it reaches the atmosphere's own seam entry --\n");
    {
        // The atmosphere captures the sky's sun position (its lobe follows the visible disc) at this draw, in its own seam entry: if a stage in front of it took the draw (answered true),
        // or drew it, the atmosphere would never see it - on the night grade's hand-off path or any other. Both real stages are armed here.
        g_giMode = GiMode::DeclinesFirstCall;
        gi::OnConfigLoaded(false);
        gi::OnConfigLoaded(true);
        ng::Configure(KeyOnParams());
        CHECK(gi::Armed());
        CHECK(ng::Armed());
        const gi::Status giBefore = gi::CurrentStatus();
        const ng::Status ngBefore = ng::CurrentStatus();
        DrawProbe probe;
        probe.context = fx.context.Get();
        const eng::Anchors sunDraw = eng::kShaftsSunParamBit | ((eng::Anchors(5) << eng::kShaftsSlotShift) & eng::kShaftsSlotMask);
        CHECK(eng::IsShaftsSunParamAnchors(sunDraw) && !eng::IsShaftsToneMapAnchors(sunDraw) && !eng::IsShaftsCompositeAnchors(sunDraw));
        CHECK(!gi::OnSeamDraw(fx.context.Get(), sunDraw, ProbeDraw, &probe));
        CHECK(!ng::OnSeamDraw(fx.context.Get(), sunDraw, ProbeDraw, &probe));
        CHECK_EQ(probe.calls, 0); // neither drew the game's call (the engine pass draws it once after the whole dispatch)
        const gi::Status giAfter = gi::CurrentStatus();
        const ng::Status ngAfter = ng::CurrentStatus();
        CHECK(giAfter.runs == giBefore.runs && giAfter.declined == giBefore.declined && giAfter.skipped == giBefore.skipped);
        CHECK(ngAfter.runs == ngBefore.runs && ngAfter.declined == ngBefore.declined);
        gi::OnConfigLoaded(false);
        g_giMode = GiMode::Off;
    }

    std::printf("-- one answer a frame: a second tone-shaped call in the same frame answers nothing --\n");
    {
        g_giMode = GiMode::Off;
        gi::OnConfigLoaded(false);
        ng::Configure(KeyOffParams());
        g_now = kNightReading;
        ng::Configure(KeyOnParams());
        for (int i = 0; i < 60 * 6; ++i)
        {
            Advance(1.0 / 60.0);
            fx.Composite();
            fx.ToneMapState();
            eng::chainNightScene(fx.context.Get(), fx.Facts(), fx.scene.view.Get());
        }
        const Cell allOn { GiMode::Off, NightMode::Night, true, true, true };
        SetStandIns(allOn);
        BeginFrame(fx);
        ID3D11ShaderResourceView* first = eng::chainGiScene(fx.context.Get(), g_facts, fx.scene.view.Get());
        CHECK(first == g_dof.plate.view.Get());
        const uint32_t runs = ng::CurrentStatus().runs;
        const int atmoAnswers = g_atmo.answers, dofAnswers = g_dof.answers;
        ID3D11ShaderResourceView* second = eng::chainGiScene(fx.context.Get(), g_facts, fx.scene.view.Get());
        CHECK(second == nullptr);                                  // nobody answered: the hold stands
        CHECK_EQ(ng::CurrentStatus().runs, runs);                  // the night grade did not grade again
        CHECK_EQ(g_atmo.answers, atmoAnswers);
        CHECK_EQ(g_dof.answers, dofAnswers);
        CHECK(g_atmo.saw == fx.scene.view.Get() && g_dof.saw == fx.scene.view.Get()); // each was handed the engine's scene, the stage in front having none
    }

    ng::SetClockForTest(nullptr, 1);
    ng::Configure(KeyOffParams());
    gi::OnConfigLoaded(false);
    return Finish("look_chain_order_warp");
}
