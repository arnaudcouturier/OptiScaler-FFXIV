// Engine light controls at the tone map and at the composite: the module's own entries on D3D11 WARP, with the neighbours stubbed by this harness.
//
// The pass test (engine_light_pass_warp) says what the shader does with a scene and says nothing about who hands it one, when the exposure was read or what happens to the
// draw. This test drives the module's entries instead, with the state they read: the composite (OnSeamDraw at the composite anchors, with a real pixel shader that carries the
// composite's reflection and the module's stamp, and a g_CommonParameter buffer at a slot that is NOT 0), the chained role (ChainScene), the fallback-owner role (OnSeamDraw at the
// tone-map anchors and ToneMapOwner) and the chain's next stage (depth of field), which this harness is instead of. The night curve's reading is a stub behind the shadow module's
// hook, the cb family's receiver is a stub behind the contract's PublishCbFamily, and the wall clock is the module's injectable test clock, so the seconds are exact.
//
// What a wrong chain position, hand-over, feed or latch breaks, and what is checked:
//   - master off: nothing armed, no feed asked for, the seam is not this module's draw, and the stage after it is still asked with the incoming scene;
//   - lamps only (exposure off): armed, the feed IS asked for (the retired grade's feed ran only on its own request: the "starved cell"), the weight reaches 1 at night with no
//     exposure reading at all, the lamp factor follows it, the receiver of the cb family is called with each change of the weight and of the keys, and the weight is exactly
//     0 by day;
//   - auto hold: the controls wait (weight 0, no pass) while the engine's exposure settles on its own light, latch it, then ramp; the answer is the stored scene times
//     s_hold / s_now with s_now THIS frame's value read on the GPU (a jump of the exposure between two frames is followed in the same frame, not a frame late), the stage
//     after is handed that texture, and the same bits come back where the gain is 1;
//   - the engine / reference A/B and the manual factor;
//   - the owner role draws the game's call exactly once with the stage's texture at t0 and puts the game's view back; a throw out of the draw is not retried;
//   - one answer a frame; sixty-four shape declines stand the exposure stage down while the lamps and the weight go on;
//   - the forced weight reaches 1 without a reading and without asking for the feed.
#include "check.h"
#include "context_set_counter.h"

#include "ext/ffxiv/engine/ChainHooks.h"
#include "ext/ffxiv/engine/EnginePass.h" // OwnWork: this harness defines its two members (the engine module is not linked)
#include "ext/ffxiv/enginelight/EngineLight.h"
#include "ext/ffxiv/shadows/ShadowsHooks.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace el = ext::ffxiv::enginelight;
namespace eng = ext::ffxiv::engine;
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
constexpr uint32_t kCommonSlot = 3; // the composite's g_CommonParameter sits at b3 here: the module must take the slot from the shader's reflection, not guess 0

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

bool Near(float a, float b, float relative) { return std::fabs(a - b) <= relative * std::fabs(b) + 1e-6f; }

// The injected wall clock: 1000 ticks a second.
int64_t g_ticks = 0;
int64_t Clock() { return g_ticks; }
void Advance(double seconds) { g_ticks += int64_t(seconds * 1000.0 + 0.5); }

// The night curve's stub: what the shadow module's hook would answer.
sd::NightNow g_now;
sd::NightNow NightNowStub(ID3D11DeviceContext*) { return g_now; }
sd::NightNow Night(float curve) { return sd::NightNow { true, curve, 1, 2, true, 0.5f, true, 0.1f, true }; }
int g_feedCalls = 0;
bool g_feedLast = false;
void NightFeedStub(bool wanted)
{
    ++g_feedCalls;
    g_feedLast = wanted;
}

// The cb family's receiver (writer 1's module in the product): what it was last told.
int g_publishCalls = 0;
el::Params g_publishedParams;
float g_publishedWeight = -1.0f;
void PublishStub(const el::Params& params, float weight)
{
    ++g_publishCalls;
    g_publishedParams = params;
    g_publishedWeight = weight;
}

// The stage after this one (depth of field) as a recorder.
int g_nextChainCalls = 0;
ID3D11ShaderResourceView* g_nextChainSaw = nullptr;
ID3D11ShaderResourceView* NextChainStub(ID3D11DeviceContext*, const sh::ToneMapFacts&, ID3D11ShaderResourceView* scene)
{
    ++g_nextChainCalls;
    g_nextChainSaw = scene;
    return nullptr;
}
int g_nextOwnerCalls = 0;
ID3D11ShaderResourceView* g_nextOwnerSaw = nullptr;
bool NextOwnerStub(ID3D11DeviceContext*, const sh::ToneMapFacts&, ID3D11ShaderResourceView* scene, eng::ChainDrawFn, void*)
{
    ++g_nextOwnerCalls;
    g_nextOwnerSaw = scene;
    return false;
}

struct DrawProbe
{
    ID3D11DeviceContext* context = nullptr;
    int calls = 0;
    ID3D11ShaderResourceView* boundAtDraw = nullptr; // weak: the view at t0 while the draw ran
    bool throwInside = false;
};
void ProbeDraw(void* user)
{
    DrawProbe& probe = *static_cast<DrawProbe*>(user);
    ++probe.calls;
    ID3D11ShaderResourceView* view = nullptr;
    probe.context->PSGetShaderResources(0, 1, &view);
    probe.boundAtDraw = view;
    if (view != nullptr)
        view->Release();
    if (probe.throwInside)
        throw 1;
}

const char* kCompositeHlsl = R"hlsl(
cbuffer g_AmbientParamArray : register(b2) { float4 AmbientProbe; };
cbuffer g_CommonParameter : register(b3) { float4 RowA; float4 RowB; float4 RowC; float4 Misc2; };
float4 PS(float4 pos : SV_Position) : SV_Target { return AmbientProbe + Misc2 + RowA + RowB + RowC; }
)hlsl";

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sceneTexture, toneTarget;
    ComPtr<ID3D11ShaderResourceView> sceneView;
    ComPtr<ID3D11RenderTargetView> sceneRtv, toneRtv;
    ComPtr<ID3D11PixelShader> compositePs;
    ComPtr<ID3D11Buffer> common, ambient;
    std::vector<uint16_t> input;
    DrawProbe composite;
    float sNow = 1.4f; // the exposure the "engine" wrote into g_CommonParameter this frame

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;
        // The scene: left half a moonlit stone (stored 0.10 / 0.11 / 0.13), right half a lamp (stored 2.0 / 1.8 / 1.2).
        input.assign(size_t(kWidth) * kHeight * 4, 0);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const bool lamp = x >= kWidth / 2;
                const size_t at = (size_t(y) * kWidth + x) * 4;
                input[at + 0] = HalfOf(lamp ? 2.0f : 0.10f);
                input[at + 1] = HalfOf(lamp ? 1.8f : 0.11f);
                input[at + 2] = HalfOf(lamp ? 1.2f : 0.13f);
                input[at + 3] = HalfOf(1.0f);
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA initial { input.data(), kWidth * 8, 0 };
        if (FAILED(device->CreateTexture2D(&desc, &initial, &sceneTexture)) || FAILED(device->CreateShaderResourceView(sceneTexture.Get(), nullptr, &sceneView)) ||
            FAILED(device->CreateRenderTargetView(sceneTexture.Get(), nullptr, &sceneRtv)))
            return false;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &toneTarget)) || FAILED(device->CreateRenderTargetView(toneTarget.Get(), nullptr, &toneRtv)))
            return false;

        // The composite's pixel shader: a real compile, so its reflection names g_AmbientParamArray and g_CommonParameter (at b3), and the module stamps it.
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3DCompile(kCompositeHlsl, std::strlen(kCompositeHlsl), "composite", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &blob, &error)))
            return false;
        if (FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &compositePs)))
            return false;
        el::NoteShader(compositePs.Get(), blob->GetBufferPointer(), blob->GetBufferSize());
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &common)))
            return false;
        bd.ByteWidth = 16;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &ambient)))
            return false;
        composite.context = context.Get();
        return true;
    }

    // The engine writes its exposure for this frame (s, y = max(1, 2.04 s^2), FLT_MAX, 0) into the scene-pass buffer.
    void WriteExposure(float s, bool fingerprintOk = true)
    {
        sNow = s;
        float bytes[16] {};
        bytes[12] = s;
        bytes[13] = fingerprintOk ? el::YOf(s) : 123.0f;
        bytes[14] = std::numeric_limits<float>::max();
        context->UpdateSubresource(common.Get(), 0, nullptr, bytes, 0, 0);
    }

    // Waits until everything submitted so far has run (an event query): the module's reader maps without waiting, and the software adapter can fall arbitrarily far behind.
    void WaitIdle()
    {
        const D3D11_QUERY_DESC desc { D3D11_QUERY_EVENT, 0 };
        ComPtr<ID3D11Query> query;
        if (FAILED(device->CreateQuery(&desc, &query)))
            return;
        context->End(query.Get());
        context->Flush();
        BOOL done = FALSE;
        for (int i = 0; i < 5000 && !done; ++i)
            if (context->GetData(query.Get(), &done, sizeof done, 0) != S_OK)
                Sleep(1);
    }

    // The frame's composite: the scene is the one render target, the composite's shader and buffers bound; the seam records the scene and ticks.
    void Composite()
    {
        WaitIdle();
        ID3D11RenderTargetView* targets[1] { sceneRtv.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        context->PSSetShader(compositePs.Get(), nullptr, 0);
        ID3D11Buffer* buffers[4] { nullptr, nullptr, ambient.Get(), common.Get() };
        context->PSSetConstantBuffers(0, 4, buffers);
        setcount::Scope counting; // the module's own calls are counted: every pipeline-state setter it calls from here to the end of the draw
        CHECK(!el::OnSeamDraw(context.Get(), eng::kShaftsCompositeBit, ProbeDraw, &composite)); // a composite is never this module's draw
    }
    void ToneMapState()
    {
        ID3D11RenderTargetView* targets[1] { toneRtv.Get() };
        context->OMSetRenderTargets(1, targets, nullptr);
        ID3D11ShaderResourceView* views[1] { sceneView.Get() };
        context->PSSetShaderResources(0, 1, views);
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
    // One frame: the engine writes `s`, the composite ticks, the chained role is asked. Returns its answer.
    ID3D11ShaderResourceView* Frame(double seconds, float s)
    {
        Advance(seconds);
        WriteExposure(s);
        Composite();
        ToneMapState();
        setcount::Scope counting;
        return eng::chainNightScene(context.Get(), Facts(), sceneView.Get());
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
    // A pixel's three channels from a read-back.
    static void Pixel(const std::vector<uint16_t>& image, uint32_t x, uint32_t y, float out[3])
    {
        const size_t at = (size_t(y) * kWidth + x) * 4;
        for (int c = 0; c < 3; ++c)
            out[c] = FromHalf(image[at + c]);
    }
};

void ResetStubs()
{
    g_nextChainCalls = 0;
    g_nextChainSaw = nullptr;
    g_nextOwnerCalls = 0;
    g_nextOwnerSaw = nullptr;
}

// The defaults of the product (lamps 3, moon 0.1, ambient 0.5, water 0.2 / 0.5, auto hold).
el::Params Defaults() { return el::Params {}; }
el::Params LampsOnly()
{
    el::Params p;
    p.moonIntensity = p.ambientIntensity = p.waterMoon = p.waterAmbient = 1.0f;
    p.exposureMode = el::kExposureOff;
    return p;
}

// The "engine": its exposure follows a first-order lag towards the identity value times (1 + takeBack x the weight that really acts).
struct Engine
{
    float identity = 1.4f;
    float takeBack = 0.3f;
    float s = 1.4f;
    float tau = 3.0f;
    void Step(double dt, float acting)
    {
        const float target = identity * (1.0f + takeBack * acting);
        s += (target - s) * (1.0f - std::exp(-float(dt) / tau));
    }
};
} // namespace

int main()
{
    Fixture fx;
    if (!fx.Create())
    {
        std::printf("engine_light_chain_warp: no WARP device\n");
        return 1;
    }
    // The wiring a product build gets from its static initialisers (this module registers its own pair; the harness installs the neighbours').
    CHECK(eng::chainNightScene == &el::ChainScene);
    CHECK(eng::toneMapNightOwner == &el::ToneMapOwner);
    eng::chainDofScene = &NextChainStub;
    eng::toneMapDofOwner = &NextOwnerStub;
    sd::Hooks::nightNow = &NightNowStub;
    sd::Hooks::nightFeed = &NightFeedStub;
    el::publishCbFamily = &PublishStub;
    el::SetClockForTest(&Clock, 1000);
    if (!setcount::Install(fx.context.Get()))
        return 2;

    std::printf("-- master off --\n");
    {
        setcount::g_counts = 0; // nothing of the module's may call a pipeline-state setter in this whole block (the Composite/Frame helpers count only the module's calls)
        el::Params off = Defaults();
        off.master = false;
        el::Configure(off);
        CHECK(!el::Armed());
        CHECK(g_feedLast == false);
        CHECK(g_publishedParams.master == false);       // the cb family was told: every arm at the engine's value
        CHECK_EQ(el::LampFactorNow(), 1.0f);
        ResetStubs();
        g_now = Night(1.0f);
        ID3D11ShaderResourceView* answer = fx.Frame(0.1, 1.4f);
        CHECK(answer == nullptr);                       // no answer: the chain's hold stands
        CHECK_EQ(g_nextChainCalls, 1);                  // ... and the stage after is still asked
        CHECK(g_nextChainSaw == fx.sceneView.Get());    // ... with the incoming scene
        DrawProbe probe { fx.context.Get() };
        {
            setcount::Scope counting;
            CHECK(!el::OnSeamDraw(fx.context.Get(), eng::kShaftsToneMapBit, ProbeDraw, &probe));
            CHECK_EQ(probe.calls, 0);
            CHECK(!eng::toneMapNightOwner(fx.context.Get(), fx.Facts(), fx.sceneView.Get(), ProbeDraw, &probe));
        }
        CHECK_EQ(g_nextOwnerCalls, 1);
        CHECK_EQ(probe.calls, 0);
        CHECK_EQ(setcount::g_counts, 0L); // master off: the composite tick, the chained role and both tone-map entries rebound NOTHING
    }

    std::printf("-- master on at weight 0 (a day frame): nothing of the module rebinds or changes any game state --\n");
    {
        el::Configure(Defaults());
        g_now = Night(0.0f);
        for (int i = 0; i < 5; ++i)
            fx.Frame(0.1, 1.4f);
        setcount::g_counts = 0;
        ResetStubs();
        ID3D11ShaderResourceView* answer = nullptr;
        for (int i = 0; i < 30; ++i)
            answer = fx.Frame(0.1, 1.4f);
        CHECK(answer == nullptr);
        CHECK_EQ(el::CurrentStatus().weight, 0.0f);
        CHECK_EQ(el::LampFactorNow(), 1.0f);
        CHECK_EQ(setcount::g_counts, 0L);
        el::Configure(el::Params { false });
    }

    std::printf("-- lamps only: armed, fed, weight and lamps follow the night with no exposure reading --\n");
    {
        el::Configure(LampsOnly());
        CHECK(el::Armed());
        CHECK(g_feedLast == true);                      // the feed is the module's own request, not the exposure stage's (M4)
        g_now = Night(1.0f);
        g_publishCalls = 0;
        ResetStubs();
        ID3D11ShaderResourceView* answer = nullptr;
        for (int i = 0; i < 60; ++i)
            answer = fx.Frame(0.1, 1.4f);
        CHECK(el::CurrentStatus().weight > 0.99f);
        CHECK(Near(el::LampFactorNow(), 3.0f, 0.01f));
        CHECK(g_publishCalls > 0);                      // the receiver heard every change (M6)
        CHECK(Near(g_publishedWeight, el::CurrentStatus().weight, 1e-6f));
        CHECK(answer == nullptr);                       // exposure off: no pass, the engine's own scene goes on
        CHECK(g_nextChainSaw == fx.sceneView.Get());
        // by day: exactly 0, the lamps back to exactly 1
        g_now = Night(0.0f);
        for (int i = 0; i < 80; ++i)
            fx.Frame(0.1, 1.4f);
        CHECK_EQ(el::CurrentStatus().weight, 0.0f);
        CHECK_EQ(el::LampFactorNow(), 1.0f);
        CHECK_EQ(g_publishedWeight, 0.0f);
        // the A/B: the lamp factor is exactly 1 while it shows the engine's values, the receiver is told
        g_now = Night(1.0f);
        for (int i = 0; i < 60; ++i)
            fx.Frame(0.1, 1.4f);
        CHECK(Near(el::LampFactorNow(), 3.0f, 0.01f));
        el::Params ab = LampsOnly();
        ab.referenceAB = true;
        el::Configure(ab);
        CHECK_EQ(el::LampFactorNow(), 1.0f);
        CHECK(g_publishedParams.referenceAB);           // (M5: the A/B reaches both consumers)
        el::Configure(LampsOnly());
        CHECK(Near(el::LampFactorNow(), 3.0f, 0.01f));
    }

    std::printf("-- the forced weight reaches 1 with no reading and no feed --\n");
    {
        el::Params forced = LampsOnly();
        forced.forceNightWeight = true;
        el::Configure(forced);
        CHECK(g_feedLast == false);
        g_now = sd::NightNow {};                        // no reading at all
        for (int i = 0; i < 50; ++i)
            fx.Frame(0.1, 1.4f);
        CHECK(el::CurrentStatus().weight > 0.98f);      // (M8)
        el::Configure(LampsOnly());
        g_now = Night(0.0f);
        for (int i = 0; i < 80; ++i)
            fx.Frame(0.1, 1.4f);
    }

    std::printf("-- auto hold: wait for the settle, latch, ramp, then s_hold / s_now with this frame's s --\n");
    {
        el::Configure(Defaults());
        CHECK(el::Armed() && g_feedLast == true);
        Engine engine;
        engine.identity = 1.4f;
        engine.s = 0.7f;                                // the day's exposure: the night starts now
        g_now = Night(1.0f);
        ResetStubs();
        bool sawSettling = false, settlingWeightZero = true, settlingNoPass = true;
        ID3D11ShaderResourceView* answer = nullptr;
        for (int i = 0; i < 600; ++i)
        {
            const el::Status before = el::CurrentStatus();
            engine.Step(0.1, before.weight);
            answer = fx.Frame(0.1, engine.s);
            const el::Status status = el::CurrentStatus();
            if (status.phase == el::Phase::Settling)
            {
                sawSettling = true;
                settlingWeightZero = settlingWeightZero && status.weight == 0.0f;
                settlingNoPass = settlingNoPass && answer == nullptr;
            }
            if (status.phase == el::Phase::Active && status.weight > 0.995f && i > 100)
                break;
        }
        const el::Status status = el::CurrentStatus();
        CHECK(sawSettling && settlingWeightZero && settlingNoPass);   // the controls waited for the engine's exposure, with no pass and no weight
        CHECK(status.phase == el::Phase::Active && status.haveHold);
        CHECK(status.sHold > 1.25f && status.sHold < 1.45f);          // the settled identity exposure (1.4), never the day's 0.7
        CHECK(status.weight > 0.99f);
        // the controls act: the engine's exposure rises by the take-back; let it get there
        for (int i = 0; i < 100; ++i)
        {
            engine.Step(0.1, el::CurrentStatus().weight);
            fx.Frame(0.1, engine.s);
        }
        CHECK(engine.s > 1.4f * 1.25f);
        const float hold = el::CurrentStatus().sHold;
        CHECK(Near(hold, status.sHold, 1e-6f));                       // the hold did not follow the rise (M3)
        // one frame, looked at: the answer is the stored scene times s_hold / s_now, with s_now THIS frame's exposure
        ResetStubs();
        engine.Step(0.1, el::CurrentStatus().weight);
        setcount::g_counts = 0;
        answer = fx.Frame(0.1, engine.s);
        CHECK(setcount::g_counts > 0); // the positive control: an acting hold DOES set pipeline state (so the zeros elsewhere mean something)
        CHECK(answer != nullptr);
        CHECK(answer != fx.sceneView.Get());
        CHECK(g_nextChainSaw == answer);                              // the chain position: the stage after is handed THIS stage's answer
        const float expectedGain = hold / engine.s;
        std::vector<uint16_t> image = fx.ReadRgba(answer);
        float stone[3], lamp[3];
        Fixture::Pixel(image, 20, 40, stone);
        Fixture::Pixel(image, 120, 40, lamp);
        CHECK(expectedGain < 0.8f && expectedGain > 0.7f);
        CHECK(Near(stone[0], 0.10f * expectedGain, 0.01f) && Near(stone[1], 0.11f * expectedGain, 0.01f) && Near(stone[2], 0.13f * expectedGain, 0.01f));
        CHECK(Near(lamp[0], 2.0f * expectedGain, 0.01f) && Near(lamp[1], 1.8f * expectedGain, 0.01f) && Near(lamp[2], 1.2f * expectedGain, 0.01f));
        // the exposure jumps by 10 % between two frames: the same frame follows it (the GPU read, no frame of lag)
        ResetStubs();
        const float jumped = engine.s * 1.10f;
        answer = fx.Frame(0.1, jumped);
        CHECK(answer != nullptr);
        image = fx.ReadRgba(answer);
        Fixture::Pixel(image, 20, 40, stone);
        CHECK(Near(stone[0], 0.10f * (hold / jumped), 0.01f));
        // a second tone-shaped call of the same frame is not answered again, and the stage after is still asked
        ResetStubs();
        fx.ToneMapState();
        CHECK(eng::chainNightScene(fx.context.Get(), fx.Facts(), fx.sceneView.Get()) == nullptr);
        CHECK_EQ(g_nextChainCalls, 1);
        CHECK(g_nextChainSaw == fx.sceneView.Get());

        // the fingerprint: a buffer whose y does not fit s (another buffer) is gain 1 on the GPU: the answer's bits are the scene's
        fx.Frame(0.1, engine.s); // the frame the bad copy follows
        Advance(0.1);
        fx.WriteExposure(engine.s, false);
        fx.Composite();
        fx.ToneMapState();
        ID3D11ShaderResourceView* bad = eng::chainNightScene(fx.context.Get(), fx.Facts(), fx.sceneView.Get());
        if (bad != nullptr)
        {
            image = fx.ReadRgba(bad);
            Fixture::Pixel(image, 20, 40, stone);
            CHECK(Near(stone[0], 0.10f, 0.002f));
        }

        // the owner role: the game's call exactly once with the stage's texture at t0, the game's view back afterwards, a throw not retried
        fx.Frame(0.1, engine.s);
        ResetStubs();
        // (the chained call above answered this frame; a new frame for the owner)
        Advance(0.1);
        fx.WriteExposure(engine.s);
        fx.Composite();
        fx.ToneMapState();
        DrawProbe probe { fx.context.Get() };
        const bool drew = el::OnSeamDraw(fx.context.Get(), eng::kShaftsToneMapBit, ProbeDraw, &probe);
        CHECK(drew);
        CHECK_EQ(probe.calls, 1);
        CHECK(probe.boundAtDraw != nullptr && probe.boundAtDraw != fx.sceneView.Get());
        ID3D11ShaderResourceView* after = nullptr;
        fx.context->PSGetShaderResources(0, 1, &after);
        CHECK(after == fx.sceneView.Get());
        if (after != nullptr)
            after->Release();
        Advance(0.1);
        fx.WriteExposure(engine.s);
        fx.Composite();
        fx.ToneMapState();
        DrawProbe thrower { fx.context.Get() };
        thrower.throwInside = true;
        const bool threw = el::OnSeamDraw(fx.context.Get(), eng::kShaftsToneMapBit, ProbeDraw, &thrower);
        CHECK(threw);
        CHECK_EQ(thrower.calls, 1);                                   // not retried
        fx.context->PSGetShaderResources(0, 1, &after);
        CHECK(after == fx.sceneView.Get());
        if (after != nullptr)
            after->Release();

        // the A/B: no pass, the stage after gets the engine's scene; the hold keeps following the engine
        el::Params ab = Defaults();
        ab.referenceAB = true;
        el::Configure(ab);
        ResetStubs();
        setcount::g_counts = 0;
        answer = fx.Frame(0.1, engine.s);
        CHECK(answer == nullptr);
        CHECK_EQ(setcount::g_counts, 0L); // the A/B at night with a latched hold: no pass, no rebinding at all
        CHECK(g_nextChainSaw == fx.sceneView.Get());
        CHECK_EQ(el::LampFactorNow(), 1.0f);
        el::Configure(Defaults());
    }

    std::printf("-- manual factor --\n");
    {
        el::Params manual = Defaults();
        manual.exposureMode = el::kExposureManual;
        manual.exposureManual = 2.0f;
        el::Configure(manual);
        g_now = Night(0.0f);
        for (int i = 0; i < 90; ++i)
            fx.Frame(0.1, 1.4f);
        CHECK_EQ(el::CurrentStatus().weight, 0.0f);
        ID3D11ShaderResourceView* answer = fx.Frame(0.1, 1.4f);
        CHECK(answer == nullptr);                                     // a day frame: the engine's own scene
        g_now = Night(1.0f);
        for (int i = 0; i < 90; ++i)
            answer = fx.Frame(0.1, 1.4f);
        CHECK(el::CurrentStatus().weight > 0.99f);
        CHECK(answer != nullptr);
        std::vector<uint16_t> image = fx.ReadRgba(answer);
        float stone[3];
        Fixture::Pixel(image, 20, 40, stone);
        CHECK(Near(stone[0], 0.20f, 0.02f) && Near(stone[2], 0.26f, 0.02f));   // weight ~1: x2
    }

    std::printf("-- shape declines stand the exposure stage down; the lamps and the weight go on --\n");
    {
        el::Configure(Defaults());
        g_now = Night(1.0f);
        sh::ToneMapFacts bad = fx.Facts();
        bad.targetFormat = 28; // not FP16
        for (int i = 0; i < 70; ++i)
        {
            Advance(0.1);
            fx.WriteExposure(1.4f);
            fx.Composite();
            fx.ToneMapState();
            eng::chainNightScene(fx.context.Get(), bad, fx.sceneView.Get());
        }
        const el::Status status = el::CurrentStatus();
        CHECK(status.standDown);
        CHECK(status.declined >= 64u);
        CHECK(el::Armed());                                           // the module as a whole goes on
        CHECK(g_feedLast == true);
        CHECK(status.weight >= 0.0f);
    }

    el::Configure(el::Params { false });
    return Finish("engine_light_chain_warp");
}
