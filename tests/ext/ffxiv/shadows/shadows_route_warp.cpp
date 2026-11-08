#include "pch.h" // the route test's stand-in: windows, d3d11 and log macros that keep every line

// Shadow mapping with Contact shadows OFF, through the real draw seam and the real dispatch (D3D11 WARP).
//
// The capture session of 2026-11-05 found that the mask route and the light colour feed could never run with Contact shadows
// off: the draw hook only entered the relight seam for the contact key, the seam only took the route branch after contact had
// stood itself down, and the gate of the route (the lift key) was false for "lift 0, night ambient 2.0", so no light colour was
// ever captured and the ticked night curve stayed at its no-reading identity. The user saw no difference at all.
//
// This test links the real Inject.cpp (OnGameDraw, the pixel-shader tracker, the reflection index) and the real
// ShadowsDispatch.cpp / ShadowsPass.cpp, with test doubles only for the config hooks of the engine seam, the scene HDR state, the
// frame counter and the replay marker. The light-draw shader is compiled in the test with the four names the engine's sun light
// draw declares, so the index, the tracker and the layout the sink receives are the product's. The FfxivLightingCapture.h gates
// in front of OnGameDraw are text-pinned in shadows_pins.cpp (that header needs the hook layer and cannot be built on the host).
//
// Night colour fixture: the g_LightParam m_DiffuseColor of the night exterior of the capture session (20261105-220954-2, cb.csv
// row t=305.050; the same colour in 20261105-221130-3): (0.0779614, 0.116937, 0.16), luminance 0.11176, curve exactly 1.0.
#include "check.h"

#include "ext/ExtFeatures.h"
#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/engine/EnginePass.h"
#include "ext/ffxiv/engine/FogSun.h"
#include "ext/ffxiv/relight/Inject.h"
#include "ext/ffxiv/shadows/ShadowsHooks.h"
#include "ext/ffxiv/shadows/ShadowsRules.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"
#include "ext/present/PresentSeams.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace rl = ext::ffxiv::relight;
namespace sh = ext::ffxiv::shadows;
namespace eng = ext::ffxiv::engine;
namespace fogsun = ext::ffxiv::fogsun;

namespace
{
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 36;
constexpr float kNight[3] { 0.0779614f, 0.116937f, 0.16f };  // capture c2 / c3
constexpr float kDay[3] { 1.96f, 1.86885f, 1.76525f };       // capture c1
constexpr float kSunDirection[3] { -0.649539f, 0.705004f, 0.284726f }; // c2, cb.csv t=305.050

// The sun light draw as this game build's capture reflects it: the mask is g_SamplerShadowMask at t8, the depth at t0, the camera
// at cb1 and g_LightParam at cb2 (contact_reflection.cpp compiles the same text), and the directional light's own
// g_DirectionalShadowParameter (since 2026-11-07 only a directional light draw is indexed: the shadowed point lights declare the four
// names too, and their colours made the night curve flicker).
constexpr const char* kSunLight = R"(
Texture2D<float4> g_SamplerShadowMask : register(t8);
Texture2D<float> g_SamplerDepth : register(t0);
cbuffer g_CameraParameter : register(b1) { float4 cameraA; };
cbuffer g_LightParam : register(b2) { float4 lightA; float4 lightB; };
cbuffer g_DirectionalShadowParameter : register(b5) { float4 dirShadow; };
float4 PS() : SV_Target
{
    return g_SamplerShadowMask.Load(int3(0, 0, 0)) + g_SamplerDepth.Load(int3(0, 0, 0)) + cameraA + lightA + lightB + dirShadow;
}
)";
// Any other pixel shader: not a sun light draw, so the index never claims it.
constexpr const char* kOther = R"(
float4 PS() : SV_Target { return float4(1, 0, 0, 1); }
)";

std::string Compile(const char* source)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "shadows_route_test", nullptr, nullptr, "PS", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || code == nullptr)
    {
        if (errors)
            std::printf("    compile failed: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return {};
    }
    return std::string(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
}

// What one "game draw" observed while it ran.
struct Payload
{
    ID3D11DeviceContext* context = nullptr;
    uint32_t maskSlot = 8;
    int ran = 0; // how many times the game's draw ran
    ID3D11ShaderResourceView* mask = nullptr;
    int ownWorkDepth = -1;
};

void Draw(void* user)
{
    auto* payload = static_cast<Payload*>(user);
    ++payload->ran;
    payload->mask = nullptr;
    payload->context->PSGetShaderResources(payload->maskSlot, 1, &payload->mask);
    if (payload->mask != nullptr)
        payload->mask->Release();
    payload->ownWorkDepth = eng::g_ownWorkDepth;
}

// The spy in front of the product's route sink: it records what the seam handed over and forwards to the real sink.
sh::LightDrawRouteFn g_realRoute = nullptr;
int g_routeCalls = 0;
int g_routeCallsWithLayout = 0;
int g_routeOwnDepth = -1;
sh::RouteLayout g_lastLayout {};

bool SpyRoute(ID3D11DeviceContext* context, void (*draw)(void*), void* user, const sh::RouteLayout* layout)
{
    ++g_routeCalls;
    if (layout != nullptr)
    {
        ++g_routeCallsWithLayout;
        g_lastLayout = *layout;
    }
    g_routeOwnDepth = eng::g_ownWorkDepth;
    return g_realRoute(context, draw, user, layout);
}

struct Scene
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11PixelShader> lightPs, otherPs;
    ComPtr<ID3D11ShaderResourceView> mask, depth;
    ComPtr<ID3D11Buffer> light, smallLight, camera;
    std::string lightCode;

    ComPtr<ID3D11ShaderResourceView> MakeView(DXGI_FORMAT format, DXGI_FORMAT viewFormat, const void* data, uint32_t rowPitch)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial { data, rowPitch, 0 };
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)))
            return {};
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = viewFormat;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), &srv, &view)))
            return {};
        return view;
    }

    ComPtr<ID3D11Buffer> MakeBuffer(const void* data, uint32_t bytes)
    {
        D3D11_BUFFER_DESC desc {};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.ByteWidth = bytes;
        D3D11_SUBRESOURCE_DATA initial { data, 0, 0 };
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&desc, &initial, &buffer)))
            return {};
        return buffer;
    }

    // g_LightParam as the engine lays it out: m_Position @0, m_Direction @16, m_DiffuseColor @32 (512 bytes in the game).
    static std::vector<float> LightBytes(const float diffuse[3])
    {
        std::vector<float> bytes(128, 0.0f);
        bytes[4] = kSunDirection[0];
        bytes[5] = kSunDirection[1];
        bytes[6] = kSunDirection[2];
        bytes[8] = diffuse[0];
        bytes[9] = diffuse[1];
        bytes[10] = diffuse[2];
        return bytes;
    }

    void SetLightColour(const float diffuse[3])
    {
        const std::vector<float> bytes = LightBytes(diffuse);
        context->UpdateSubresource(light.Get(), 0, nullptr, bytes.data(), 0, 0);
    }

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;
        lightCode = Compile(kSunLight);
        const std::string otherCode = Compile(kOther);
        if (lightCode.empty() || otherCode.empty())
            return false;
        if (FAILED(device->CreatePixelShader(lightCode.data(), lightCode.size(), nullptr, &lightPs)) ||
            FAILED(device->CreatePixelShader(otherCode.data(), otherCode.size(), nullptr, &otherPs)))
            return false;
        std::vector<unsigned char> levels(size_t(kWidth) * kHeight, 0x60u);
        mask = MakeView(DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, levels.data(), kWidth); // the engine's pair: typeless texture, typed view
        std::vector<float> depths(size_t(kWidth) * kHeight, 0.05f);
        depth = MakeView(DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, depths.data(), kWidth * 4);
        const std::vector<float> lightBytes = LightBytes(kNight);
        light = MakeBuffer(lightBytes.data(), 512);
        const std::vector<float> smallBytes(4, 0.0f);
        smallLight = MakeBuffer(smallBytes.data(), 16);
        const std::vector<float> cameraBytes(256, 0.0f);
        camera = MakeBuffer(cameraBytes.data(), 1024);
        return mask && depth && light && smallLight && camera;
    }

    // Binds the draw's state as the game does it: the shader through the product's tracker, the mask at t8, the depth at t0, the camera at
    // cb1, the light at cb2 and the render window.
    void Bind(ID3D11PixelShader* shader, ID3D11Buffer* lightBuffer, float windowX = 0.0f)
    {
        context->PSSetShader(shader, nullptr, 0);
        rl::OnPixelShaderSet(shader, 0);
        ID3D11ShaderResourceView* views[16] {};
        views[8] = mask.Get();
        views[0] = depth.Get();
        context->PSSetShaderResources(0, 16, views);
        ID3D11Buffer* buffers[8] {};
        buffers[1] = camera.Get();
        buffers[2] = lightBuffer;
        context->PSSetConstantBuffers(0, 8, buffers);
        D3D11_VIEWPORT viewport { windowX, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
    }

    ID3D11ShaderResourceView* BoundMask()
    {
        ID3D11ShaderResourceView* view = nullptr;
        context->PSGetShaderResources(8, 1, &view);
        if (view != nullptr)
            view->Release();
        return view;
    }

    // Everything issued so far has run on the GPU (the staging copies have landed).
    void WaitGpu()
    {
        D3D11_QUERY_DESC desc { D3D11_QUERY_EVENT, 0 };
        ComPtr<ID3D11Query> query;
        if (FAILED(device->CreateQuery(&desc, &query)))
            return;
        context->End(query.Get());
        context->Flush();
        BOOL done = FALSE;
        for (int i = 0; i < 5000 && !done; ++i)
        {
            if (context->GetData(query.Get(), &done, sizeof done, 0) == S_OK && done)
                break;
            Sleep(1);
        }
    }
};

Scene g_scene;

// One game draw through the product's seam at the frame counter value `frame`.
Payload DrawAt(uint64_t frame)
{
    ext::present::g_testFrame = frame;
    Payload payload;
    payload.context = g_scene.context.Get();
    rl::OnGameDraw(g_scene.context.Get(), &Draw, &payload);
    return payload;
}

void Configure(float lift, float ambient, bool toggle)
{
    auto& cfg = ext::Cfg().ffxiv;
    cfg.shadowLift = lift;
    cfg.shadowEdgeSoftness = 0.0f;
    cfg.nightAmbientLift = ambient;
    cfg.nightCurveToggle = toggle;
    sh::OnConfigLoaded(false); // a fresh session: counters, log gate, reading
    sh::OnConfigLoaded(true);
    g_testLog.clear(); // the lines of a scenario are the lines since its own configuration
    g_routeCalls = 0;
    g_routeCallsWithLayout = 0;
}

size_t CountLogs(const std::string& line)
{
    size_t n = 0;
    for (const std::string& entry : g_testLog)
        if (entry == line)
            ++n;
    return n;
}

size_t CountLogsContaining(const std::string& part)
{
    size_t n = 0;
    for (const std::string& entry : g_testLog)
        if (entry.find(part) != std::string::npos)
            ++n;
    return n;
}
} // namespace

int main()
{
    CHECK(g_scene.Create());
    if (!g_scene.device)
        return Finish("shadows_route_warp");
    CHECK(sh::Hooks::lightDrawRoute != nullptr && sh::Hooks::maskWants != nullptr && sh::Hooks::ambientEntries != nullptr &&
          sh::Hooks::combineLift != nullptr && sh::Hooks::captureLightColour != nullptr);
    g_realRoute = sh::Hooks::lightDrawRoute;
    sh::Hooks::lightDrawRoute = &SpyRoute;

    // The product's index takes the compiled light shader whatever any key says.
    rl::Publish(false);
    rl::OnShaderCreated(g_scene.lightPs.Get(), g_scene.lightCode.data(), g_scene.lightCode.size(), 0x31882942u);
    CHECK(rl::CurrentStatus().indexed >= 1u);
    CHECK(!ext::Active(ext::F::ContactShadows));

    // ---- A: Contact shadows OFF, Shadow mapping on, lift 0.5, the ticked night curve ------------------------------------
    // The route is entered with a non-null layout; the first draw has no light colour yet (the game's own mask), the colour
    // lands, and the next draw binds the lifted copy for exactly one game draw. Contact shadows never runs.
    {
        Configure(0.5f, 1.0f, true);
        CHECK(ext::Active(ext::F::ShadowMapping)); // the draw hook's bit: the route has work
        CHECK(!ext::Active(ext::F::ContactShadows));
        CHECK(sh::Hooks::maskWants());
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        const rl::Status contactBefore = rl::CurrentStatus();

        const Payload first = DrawAt(100);
        CHECK_EQ(first.ran, 1);
        CHECK_EQ(g_routeCalls, 1);
        CHECK_EQ(g_routeCallsWithLayout, 1);   // LightDrawRouteSink is called with a non-null layout
        CHECK(g_lastLayout.id != 0u);
        CHECK_EQ(g_lastLayout.maskSlot, 8u);
        CHECK_EQ(g_lastLayout.depthSlot, 0u);
        CHECK_EQ(g_lastLayout.lightParamSlot, 2u);
        CHECK_EQ(g_lastLayout.cameraParamSlot, 1u);
        CHECK_EQ(g_routeOwnDepth, 1);          // the route runs under the engine seam's own-work guard
        CHECK(first.mask == g_scene.mask.Get()); // no light colour has landed yet: the game's own mask
        sh::Status status = sh::CurrentStatus();
        CHECK_EQ(status.routeDrawsSeen, uint64_t(1));
        CHECK_EQ(status.colourCopies, 1u);
        CHECK_EQ(status.colourReads, 0u);
        CHECK(!status.haveReading);
        CHECK_EQ(CountLogs(sh::FormatRouteFirst(kWidth, kHeight, uint32_t(DXGI_FORMAT_R8_UNORM), 8, 2, 0.5f, 1.0f, true)), size_t(1));

        g_scene.WaitGpu();
        const Payload second = DrawAt(101);
        CHECK_EQ(second.ran, 1);
        CHECK(second.mask != nullptr && second.mask != g_scene.mask.Get()); // the lifted copy was bound for the draw
        CHECK(g_scene.BoundMask() == g_scene.mask.Get());                   // and the game's own binding is back
        status = sh::CurrentStatus();
        CHECK_EQ(status.colourReads, 1u);
        CHECK(status.haveReading);
        CHECK_EQ(status.curve, 1.0f);
        CHECK_EQ(status.routeRuns, 1u);
        CHECK_EQ(status.routeDrawsSeen, uint64_t(2));
        CHECK_EQ(CountLogs(sh::FormatRouteFirst(kWidth, kHeight, uint32_t(DXGI_FORMAT_R8_UNORM), 8, 2, 0.5f, 1.0f, true)), size_t(1)); // still once
        CHECK_EQ(CountLogsContaining("Shadow mapping: the first light colour landed (0.0780, 0.1169, 0.1600)"), size_t(1));
        CHECK_EQ(CountLogs(sh::FormatMaskRun(kWidth, kHeight, uint32_t(DXGI_FORMAT_R8_UNORM), 0.5f, 0.0f, 1.0f, true)), size_t(1));

        // A slider move publishes the config again: the once-per-state lines are not written a second time.
        sh::OnConfigLoaded(true);
        sh::OnConfigLoaded(true);
        (void) DrawAt(102);
        CHECK_EQ(CountLogs(sh::FormatRouteFirst(kWidth, kHeight, uint32_t(DXGI_FORMAT_R8_UNORM), 8, 2, 0.5f, 1.0f, true)), size_t(1));
        CHECK_EQ(CountLogsContaining("Shadow mapping: the first light colour landed"), size_t(1));
        CHECK(sh::CurrentStatus().haveReading); // and the reading survives it

        // Contact shadows did not run at any point (its pass has not seen a draw): the route is the module's own.
        const rl::Status contactAfter = rl::CurrentStatus();
        CHECK_EQ(contactAfter.runs, contactBefore.runs);
        CHECK_EQ(contactAfter.declined, contactBefore.declined);
        CHECK(!ext::Active(ext::F::ContactShadows));
    }

    // ---- B: lift 0, night ambient 2.0, the curve ticked (the capture session's setting) ----------------------------------
    // The mask route follows the light draws only to capture the colour; the game's draw runs once with the game's mask; the
    // colour lands; the ambient provider then answers its two entries at the curve (1.0 at the night colour).
    {
        Configure(0.0f, 2.0f, true);
        CHECK(ext::Active(ext::F::ShadowMapping)); // the colour feed opens the draw hook for the lift-less case too
        CHECK(sh::CurrentStatus().colourReads == 0u && !sh::CurrentStatus().haveReading); // a fresh reading state
        CHECK(sh::Hooks::maskWants());            // the route is asked for the feed, though the lift key is at its identity
        CHECK(sh::Hooks::ambientWants());
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 0u); // before any colour: the identity

        const Payload first = DrawAt(200);
        CHECK_EQ(first.ran, 1);
        CHECK_EQ(g_routeCallsWithLayout, 1);
        CHECK(first.mask == g_scene.mask.Get());   // lift 0: the game's own mask
        sh::Status status = sh::CurrentStatus();
        CHECK_EQ(status.colourCopies, 1u);          // the colour ring got a copy
        CHECK_EQ(status.routeDrawsSeen, uint64_t(1));
        CHECK_EQ(status.routeRuns, 0u);
        CHECK_EQ(CountLogs(sh::FormatRouteFirst(kWidth, kHeight, uint32_t(DXGI_FORMAT_R8_UNORM), 8, 2, 0.0f, 2.0f, true)), size_t(1));

        g_scene.WaitGpu();
        const Payload second = DrawAt(201);
        CHECK_EQ(second.ran, 1);
        CHECK(second.mask == g_scene.mask.Get());
        status = sh::CurrentStatus();
        CHECK_EQ(status.colourReads, 1u);
        CHECK(status.haveReading);
        CHECK_EQ(CountLogsContaining("Shadow mapping: the first light colour landed"), size_t(1));
        const uint32_t count = sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve);
        CHECK_EQ(count, 2u);                        // ambientRuns became true once the colour landed
        CHECK_EQ(curve, 1.0f);
        CHECK_EQ(entries[0].multiplier[1], 2.0f);   // the header skyShRecipScale field
        CHECK_EQ(entries[1].multiplier[3], 2.0f);   // the per-probe shRecipScale field

        // The contact combine stays at identity: lift 0 with ambient 2.0 writes no lift, copies nothing.
        float lift = -1.0f, softness = -1.0f;
        const uint32_t copiesBefore = sh::CurrentStatus().colourCopies;
        CHECK(!sh::Hooks::combineLift(g_scene.context.Get(), &lift, &softness));
        CHECK_EQ(lift, -1.0f);
        CHECK_EQ(softness, -1.0f);
        CHECK_EQ(sh::CurrentStatus().colourCopies, copiesBefore);

        // A day colour: the curve is 0, the ambient table is the engine's own again.
        g_scene.SetLightColour(kDay);
        const Payload day1 = DrawAt(202);
        g_scene.WaitGpu();
        const Payload day2 = DrawAt(203);
        CHECK_EQ(day1.ran + day2.ran, 2);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 0u);
    }

    // ---- B2 (L1, 2026-11-07): the night grade's feed. Both of the module's own keys at their identity: nothing of the module's own wants the ---
    // route (the starved cell: night grade on, shadow lift 0, night ambient lift 1), so the grade asks for the feed (Hooks::nightFeed): the route is
    // asked, the colour is captured and read back, and Hooks::nightNow answers the shared curve (ticked) from it - first with the master key on, then
    // with the master key off (the feed does not need it).
    {
        Configure(0.0f, 1.0f, true);
        CHECK(sh::Hooks::nightFeed != nullptr && sh::Hooks::nightNow != nullptr);
        CHECK(!sh::Hooks::maskWants());                  // the starved cell: no feed, no route
        CHECK(!ext::Active(ext::F::ShadowMapping));
        CHECK(!fogsun::Wanted());
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        CHECK(!sh::Hooks::nightNow(g_scene.context.Get()).have); // no reading at all
        sh::Hooks::nightFeed(true);
        CHECK(sh::Hooks::maskWants());                   // the request opens the route
        CHECK(ext::Active(ext::F::ShadowMapping));
        CHECK(fogsun::Wanted());                         // and the fog sun capture's flag
        const Payload first = DrawAt(300);
        CHECK_EQ(first.ran, 1);
        CHECK(first.mask == g_scene.mask.Get());         // lift 0: the game's own mask, nothing lifted
        CHECK_EQ(sh::CurrentStatus().colourCopies, 1u);  // the colour ring got a copy
        g_scene.WaitGpu();
        const Payload second = DrawAt(301);
        CHECK_EQ(second.ran, 1);
        CHECK(second.mask == g_scene.mask.Get());
        sh::NightNow night = sh::Hooks::nightNow(g_scene.context.Get());
        CHECK(night.have && night.haveColour);
        CHECK(night.masterOn);
        CHECK_EQ(night.curve, 1.0f);                     // the night colour, no fog sun: the colour's own curve
        CHECK(std::fabs(night.luminance - 0.11176f) < 1.0e-3f);
        // the card's toggle unticked must not reach the grade: its reading is the ticked curve (a day colour reads 0, not the pin at 1)
        ext::Cfg().ffxiv.nightCurveToggle = false;
        sh::OnConfigLoaded(true);
        g_scene.SetLightColour(kDay);
        DrawAt(302);
        g_scene.WaitGpu();
        DrawAt(303);
        const sh::NightNow dayReading = sh::Hooks::nightNow(g_scene.context.Get());
        CHECK(dayReading.have && dayReading.haveColour);
        CHECK_EQ(dayReading.curve, 0.0f);                // ticked inside the hook: day is 0
        CHECK(sh::CurrentStatus().curve == 0.0f || sh::CurrentStatus().curve == 1.0f); // (the module's own curve is its own business)
        // the request withdrawn: back to the starved state
        sh::Hooks::nightFeed(false);
        CHECK(!sh::Hooks::maskWants());
        CHECK(!ext::Active(ext::F::ShadowMapping));
        CHECK(!fogsun::Wanted());

        // the master key OFF with the request up: the route still runs for the grade, the module's own patches stay off
        Configure(0.0f, 1.0f, true);
        sh::OnConfigLoaded(false);
        CHECK(!sh::Hooks::maskWants());
        sh::Hooks::nightFeed(true);
        CHECK(sh::Hooks::maskWants());
        CHECK(fogsun::Wanted());
        g_scene.SetLightColour(kNight);
        DrawAt(310);
        g_scene.WaitGpu();
        const Payload offSecond = DrawAt(311);
        CHECK(offSecond.mask == g_scene.mask.Get());     // nothing lifted with the master off
        const sh::NightNow offNight = sh::Hooks::nightNow(g_scene.context.Get());
        CHECK(offNight.have && offNight.haveColour && !offNight.masterOn);
        CHECK_EQ(offNight.curve, 1.0f);
        eng::ScaleEntry offEntries[8] {};
        float offCurve = -1.0f;
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), offEntries, 8, &offCurve), 0u); // the module's own ambient arm stays off
        sh::Hooks::nightFeed(false);
        sh::OnConfigLoaded(true);
        ext::Cfg().ffxiv.nightCurveToggle = true;
    }

    // ---- C: the keys back at identity after a route was tracked: nothing is asked, copied or dispatched -----------------
    // The tracker still holds the light shader's id from B (it stops updating when the gate closes); the seam must not call the sink.
    {
        Configure(0.0f, 1.0f, true);
        CHECK(!ext::Active(ext::F::ShadowMapping)); // the key is on, but the draw hook stays at one relaxed load: the bit follows the route's gate
        CHECK(!sh::Hooks::maskWants());
        const rl::Status contactBefore = rl::CurrentStatus();
        const Payload draw = DrawAt(300);
        CHECK_EQ(draw.ran, 1);
        CHECK(draw.mask == g_scene.mask.Get());
        CHECK_EQ(g_routeCalls, 0);                  // the seam did not even ask the sink
        const sh::Status status = sh::CurrentStatus();
        CHECK_EQ(status.colourCopies, 0u);
        CHECK_EQ(status.routeDrawsSeen, uint64_t(0));
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 0u);
        float lift = -1.0f, softness = -1.0f;
        CHECK(!sh::Hooks::combineLift(g_scene.context.Get(), &lift, &softness));
        const rl::Status contactAfter = rl::CurrentStatus();
        CHECK_EQ(contactAfter.runs, contactBefore.runs);        // and the contact pass never served this draw
        CHECK_EQ(contactAfter.declined, contactBefore.declined);
    }

    // ---- D: the curve unticked, lift 0, ambient 2.0: the curve is pinned, no colour is needed, the route is not asked ------
    {
        Configure(0.0f, 2.0f, false);
        CHECK(!sh::Hooks::maskWants());
        CHECK(!ext::Active(ext::F::ShadowMapping));
        const Payload draw = DrawAt(400);
        CHECK_EQ(draw.ran, 1);
        CHECK_EQ(g_routeCalls, 0);
        CHECK_EQ(sh::CurrentStatus().colourCopies, 0u);
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 2u); // pinned at full effect, no reading needed
        CHECK_EQ(curve, 1.0f);
    }

    // ---- E: Contact shadows ON and armed: the seam takes the same branch as before (its own pass, never the route) -------
    {
        Configure(0.5f, 1.0f, true);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        rl::Publish(true);
        CHECK(ext::Active(ext::F::ContactShadows));
        CHECK(rl::Armed());
        const rl::Status contactBefore = rl::CurrentStatus();
        const int routeBefore = g_routeCalls;
        const Payload draw = DrawAt(500);
        CHECK_EQ(draw.ran, 1);
        CHECK_EQ(g_routeCalls, routeBefore);        // the shadows route is not asked while the contact pass is armed
        const rl::Status contactAfter = rl::CurrentStatus();
        CHECK(contactAfter.runs + contactAfter.declined > contactBefore.runs + contactBefore.declined); // ApplyDraw served (or refused) it
        rl::Publish(false);
        CHECK(!ext::Active(ext::F::ContactShadows));
    }

    // ---- F: the frame counter steps by 4 (a 4x frame generation present count): the ring is keyed by capture, not by frame ----
    {
        Configure(0.5f, 1.0f, true);
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        const Payload one = DrawAt(600);
        g_scene.WaitGpu();
        const Payload two = DrawAt(604);
        CHECK_EQ(one.ran + two.ran, 2);
        CHECK(sh::CurrentStatus().haveReading);
        CHECK(two.mask != nullptr && two.mask != g_scene.mask.Get()); // the lift ran on the colour read across a step of 4
        g_scene.WaitGpu();
        const Payload three = DrawAt(612); // and a step of 8
        CHECK(three.mask != nullptr && three.mask != g_scene.mask.Get());
    }

    // ---- G: the route stands itself down after 64 declines: it is not asked again, the game's draw runs once each time ----
    {
        Configure(0.5f, 1.0f, true);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get(), -100.0f); // a render window that leaves the mask: a non-shape decline
        int drawn = 0;
        for (uint32_t i = 0; i < sh::kConsecutiveDeclineLimit; ++i)
            drawn += DrawAt(700 + i).ran;
        CHECK_EQ(drawn, int(sh::kConsecutiveDeclineLimit)); // the game's draw ran exactly once per call, declined or not
        CHECK(sh::CurrentStatus().standDown);
        CHECK_EQ(CountLogs(sh::FormatSwitchedOff()), size_t(1));
        CHECK(!sh::Hooks::maskWants());
        CHECK(!ext::Active(ext::F::ShadowMapping)); // and the draw hook is back to one relaxed load
        const int callsBefore = g_routeCalls;
        const Payload after = DrawAt(900);
        CHECK_EQ(after.ran, 1);
        CHECK_EQ(g_routeCalls, callsBefore);        // a stood-down route is not asked again
        CHECK(CountLogsContaining("Shadow mapping: the light draw was left alone") >= size_t(1));
    }

    // ---- G2: the route stands down with a night reading in hand: the reading is forgotten, the ambient arm stops patching ----
    // The colour is only ever refreshed by the route's own served draws, so once the route is gone nothing could age a reading
    // out: a night colour would lift a later daytime scene for the rest of the session. Declines before the stand-down leave the
    // reading alone, and a served draw on the colour-only path (the game's own mask) ends the streak like a lifted draw does.
    {
        Configure(0.0f, 2.0f, true);
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        DrawAt(1000);
        g_scene.WaitGpu();
        DrawAt(1001);
        CHECK(sh::CurrentStatus().haveReading);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 2u); // the arm patches at night
        CHECK_EQ(CountLogsContaining("Shadow mapping: the first light colour landed"), size_t(1));

        // Sixty-three refusals in a row: not yet a stand-down, and the reading stays.
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get(), -100.0f);
        for (uint32_t i = 0; i + 1 < sh::kConsecutiveDeclineLimit; ++i)
            DrawAt(1100 + i);
        CHECK(!sh::CurrentStatus().standDown);
        CHECK(sh::CurrentStatus().haveReading);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 2u);

        // A served draw on the colour-only path ends the streak: sixty-three more are still not a stand-down.
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        DrawAt(1200);
        g_scene.WaitGpu();
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get(), -100.0f);
        for (uint32_t i = 0; i + 1 < sh::kConsecutiveDeclineLimit; ++i)
            DrawAt(1300 + i);
        CHECK(!sh::CurrentStatus().standDown);
        CHECK(sh::CurrentStatus().haveReading);

        // The sixty-fourth in a row: the stand-down. The reading goes with it, so the arm answers nothing and the engine's own
        // ambient shows; asking again does not bring the ring's older copies back.
        DrawAt(1400);
        CHECK(sh::CurrentStatus().standDown);
        CHECK(!sh::CurrentStatus().haveReading);
        curve = -1.0f;
        for (int ask = 0; ask < 3; ++ask)
        {
            CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 0u);
            CHECK(!sh::CurrentStatus().haveReading);
        }
        CHECK_EQ(curve, -1.0f);
        CHECK_EQ(sh::CurrentStatus().curve, 0.0f);

        // The player's re-arm (a config load with the master on) starts afresh: a new colour lands, the arm patches again, and the
        // "first light colour" line is written once more for the new reading.
        sh::OnConfigLoaded(true);
        CHECK(!sh::CurrentStatus().standDown);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        DrawAt(1500);
        g_scene.WaitGpu();
        DrawAt(1501);
        CHECK(sh::CurrentStatus().haveReading);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 2u);
        CHECK_EQ(CountLogsContaining("Shadow mapping: the first light colour landed"), size_t(2));
    }

    // ---- G3: the colour-change diagnostic and the two-light-draws line (diagnostics only: nothing here changes what the curve does) ----
    {
        Configure(0.0f, 2.0f, true);
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        DrawAt(2000);
        g_scene.WaitGpu();
        DrawAt(2001);
        CHECK(sh::CurrentStatus().haveReading);
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 2u); // the curve at night is unchanged: 2 entries at 1.0
        // The first reading is a change from "nothing": one line, with the colour, the luminance, the curve and the route id.
        CHECK_EQ(CountLogsContaining("Shadow mapping: the light colour read changed to (0.0780, 0.1169, 0.1600)"), size_t(1));
        CHECK_EQ(CountLogsContaining("from the light draw with route id"), size_t(1));
        // The same colour asked for again writes nothing more.
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 2u);
        CHECK_EQ(CountLogsContaining("the light colour read changed"), size_t(1));
        // The day colour (4 binary orders of magnitude of luminance away) is a change: a second line.
        g_scene.SetLightColour(kDay);
        DrawAt(2002);
        g_scene.WaitGpu();
        DrawAt(2003);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 0u); // and the day curve is the engine's own table, as before
        CHECK_EQ(CountLogsContaining("Shadow mapping: the light colour read changed to (1.9600, 1.8688, 1.7652)"), size_t(1));
        CHECK_EQ(CountLogsContaining("the light colour read changed"), size_t(2));
        // Two light draws under one frame key: the capture keeps the later copy, and the line says so once.
        CHECK_EQ(CountLogsContaining("two light draws were served in one frame"), size_t(0));
        DrawAt(2010);
        DrawAt(2010);
        CHECK_EQ(CountLogsContaining("two light draws were served in one frame"), size_t(1));
        DrawAt(2011);
        DrawAt(2011);
        CHECK_EQ(CountLogsContaining("two light draws were served in one frame"), size_t(1)); // once per state
    }

    // ---- H: idle reasons, each with its own once-per-state line ------------------------------------------------------------
    {
        // H1 no sun light draw ever reaches the route: the bound shader is not in the index
        Configure(0.5f, 1.0f, true);
        g_scene.Bind(g_scene.otherPs.Get(), g_scene.light.Get());
        int drawn = 0;
        for (uint32_t i = 0; i < sh::kNoLayoutAsks + 8; ++i)
            drawn += DrawAt(1000 + i).ran;
        CHECK_EQ(drawn, int(sh::kNoLayoutAsks + 8));
        CHECK_EQ(g_routeCallsWithLayout, 0);
        CHECK_EQ(CountLogs(sh::FormatIdleNoLayout(sh::kNoLayoutAsks)), size_t(1));

        // H2 g_LightParam smaller than the members the curve reads
        Configure(0.0f, 2.0f, true);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.smallLight.Get());
        const Payload tiny = DrawAt(2000);
        CHECK_EQ(tiny.ran, 1);
        CHECK_EQ(sh::CurrentStatus().colourCopies, 0u);
        CHECK_EQ(CountLogs(sh::FormatIdleColourBuffer(16u, 0x30u)), size_t(1));
        (void) DrawAt(2001);
        CHECK_EQ(CountLogs(sh::FormatIdleColourBuffer(16u, 0x30u)), size_t(1)); // once per state

        // H3 the ticked curve waits for a colour that never lands (no light draw reaches the module)
        Configure(0.0f, 2.0f, true);
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        for (uint32_t i = 0; i < sh::kNoColourAsks + 5; ++i)
            CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 0u);
        CHECK_EQ(CountLogs(sh::FormatIdleNoColour(sh::kNoColourAsks, 0, 0, 0, 0)), size_t(1));

        // H4 the entries do not fit the caller's room
        Configure(0.0f, 2.0f, true);
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        (void) DrawAt(3000);
        g_scene.WaitGpu();
        (void) DrawAt(3001);
        CHECK(sh::CurrentStatus().haveReading);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 1, &curve), 0u);
        CHECK_EQ(CountLogs(sh::FormatIdleEntries()), size_t(1));
    }

    // ---- H5: the route is served but the engine seam never asks the ambient provider (the composite was not classified) ------
    {
        Configure(0.0f, 2.0f, true);
        g_scene.SetLightColour(kNight);
        g_scene.Bind(g_scene.lightPs.Get(), g_scene.light.Get());
        for (uint32_t i = 0; i < sh::kNoCompositeDraws + 10; ++i)
            (void) DrawAt(5000 + i);
        CHECK_EQ(CountLogs(sh::FormatIdleNoComposite(sh::kNoCompositeDraws, 2.0f)), size_t(1));

        // when the seam does ask, the line is never written
        Configure(0.0f, 2.0f, true);
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        (void) sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve);
        for (uint32_t i = 0; i < sh::kNoCompositeDraws + 10; ++i)
            (void) DrawAt(7000 + i);
        CHECK_EQ(CountLogsContaining("was never asked for entries"), size_t(0));

        // and a layout seen once keeps the no-layout line away whatever is bound afterwards
        Configure(0.5f, 1.0f, true);
        (void) DrawAt(8000);
        g_scene.Bind(g_scene.otherPs.Get(), g_scene.light.Get());
        for (uint32_t i = 0; i < sh::kNoLayoutAsks + 8; ++i)
            (void) DrawAt(8100 + i);
        CHECK_EQ(CountLogs(sh::FormatIdleNoLayout(sh::kNoLayoutAsks)), size_t(0));
    }

    // ---- I: Shadow mapping off: the module is not asked at all ------------------------------------------------------------
    {
        sh::OnConfigLoaded(false);
        CHECK(!ext::Active(ext::F::ShadowMapping));
        CHECK(!sh::Hooks::maskWants());
        const int callsBefore = g_routeCalls;
        const Payload draw = DrawAt(4000);
        CHECK_EQ(draw.ran, 1);
        CHECK_EQ(g_routeCalls, callsBefore);
    }

    // ---- J (2026-11-07, LAST: the index's main-view flag is process-wide): three light draws a frame, as the rc6 captures show them.
    // The main view's sun (g_FakeSpecularParam, the night colour), the character view's (directional, no fake specular, a day colour) drawn
    // AFTER it, and a shadowed point light (the four names, no directional buffer, a bright colour). The night curve's colour comes from the
    // main view's draw only; the point light is never indexed, so the route never sees it; the character view's mask is still lifted.
    {
        constexpr const char* kMainSun = R"(
Texture2D<float4> g_SamplerShadowMask : register(t8);
Texture2D<float> g_SamplerDepth : register(t0);
cbuffer g_CameraParameter : register(b1) { float4 cameraA; };
cbuffer g_LightParam : register(b2) { float4 lightA; float4 lightB; };
cbuffer g_DirectionalShadowParameter : register(b5) { float4 dirShadow; };
cbuffer g_FakeSpecularParam : register(b6) { float4 fakeSpecular; };
float4 PS() : SV_Target
{
    return g_SamplerShadowMask.Load(int3(0, 0, 0)) + g_SamplerDepth.Load(int3(0, 0, 0)) + cameraA + lightA + lightB + dirShadow + fakeSpecular;
}
)";
        constexpr const char* kPointLight = R"(
Texture2D<float4> g_SamplerShadowMask : register(t8);
Texture2D<float> g_SamplerDepth : register(t0);
cbuffer g_CameraParameter : register(b1) { float4 cameraA; };
cbuffer g_LightParam : register(b2) { float4 lightA; float4 lightB; };
cbuffer g_LightDrawParam : register(b4) { float4 drawParam; };
float4 PS() : SV_Target
{
    return g_SamplerShadowMask.Load(int3(0, 0, 0)) + g_SamplerDepth.Load(int3(0, 0, 0)) + cameraA + lightA + lightB + drawParam;
}
)";
        const std::string mainCode = Compile(kMainSun);
        const std::string pointCode = Compile(kPointLight);
        CHECK(!mainCode.empty() && !pointCode.empty());
        ComPtr<ID3D11PixelShader> mainPs, pointPs;
        CHECK(SUCCEEDED(g_scene.device->CreatePixelShader(mainCode.data(), mainCode.size(), nullptr, &mainPs)));
        CHECK(SUCCEEDED(g_scene.device->CreatePixelShader(pointCode.data(), pointCode.size(), nullptr, &pointPs)));
        CHECK(!rl::AnyIndexedFakeSpecular());
        rl::OnShaderCreated(mainPs.Get(), mainCode.data(), mainCode.size(), 0x9DC59665u);
        rl::OnShaderCreated(pointPs.Get(), pointCode.data(), pointCode.size(), 0xAB9227D4u);
        CHECK(rl::AnyIndexedFakeSpecular());
        // g_scene.lightPs (kSunLight) is the character view's shape: directional, no fake specular

        constexpr float kBright[3] { 4.0f, 4.0f, 4.0f }; // a point light's colour from the shadow-lift log
        const std::vector<float> mainBytes = g_scene.LightBytes(kNight);
        const std::vector<float> characterBytes = g_scene.LightBytes(kDay);
        const std::vector<float> pointBytes = g_scene.LightBytes(kBright);
        ComPtr<ID3D11Buffer> mainLight = g_scene.MakeBuffer(mainBytes.data(), 512);
        ComPtr<ID3D11Buffer> characterLight = g_scene.MakeBuffer(characterBytes.data(), 512);
        ComPtr<ID3D11Buffer> pointLight = g_scene.MakeBuffer(pointBytes.data(), 512);
        CHECK(mainLight && characterLight && pointLight);

        // (a) the colour: ambient 2.0, no lift; each frame main (night) -> character (day) -> point (bright)
        Configure(0.0f, 2.0f, true);
        uint64_t frame = 9000;
        for (int i = 0; i < 3; ++i, ++frame)
        {
            g_scene.Bind(mainPs.Get(), mainLight.Get());
            CHECK_EQ(DrawAt(frame).ran, 1);
            g_scene.Bind(g_scene.lightPs.Get(), characterLight.Get());
            CHECK_EQ(DrawAt(frame).ran, 1);
            const int withLayout = g_routeCallsWithLayout;
            g_scene.Bind(pointPs.Get(), pointLight.Get());
            const Payload point = DrawAt(frame);
            CHECK_EQ(point.ran, 1);
            CHECK(point.mask == g_scene.mask.Get());        // the point light keeps the game's own mask
            CHECK_EQ(g_routeCallsWithLayout, withLayout);   // and the route never received it as a sun light draw
            g_scene.WaitGpu();
        }
        sh::Status status = sh::CurrentStatus();
        CHECK_EQ(status.colourCopies, 3u); // one copy a frame: the main view's draw only (the character view's and the point light's are not copied)
        eng::ScaleEntry entries[8] {};
        float curve = -1.0f;
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries, 8, &curve), 2u);
        CHECK_EQ(curve, 1.0f); // the main view's night colour, although the character view's day colour was drawn after it every frame

        // (b) the lift: 0.5 with the main view's night reading; the character view's mask is lifted like the main view's
        Configure(0.5f, 1.0f, true);
        for (int i = 0; i < 2; ++i, ++frame)
        {
            g_scene.Bind(mainPs.Get(), mainLight.Get());
            (void) DrawAt(frame);
            g_scene.WaitGpu();
        }
        g_scene.Bind(g_scene.lightPs.Get(), characterLight.Get());
        const Payload character = DrawAt(frame++);
        CHECK_EQ(character.ran, 1);
        CHECK(character.mask != nullptr && character.mask != g_scene.mask.Get()); // the lifted copy was bound for the character view's draw

        // ---- K (2026-11-07): the night curve follows the visible sun. The fog sun (cFogParam[2].y, published by the fog-post capture) is the curve's input
        // while it is fresh; the main view's light colour is the fallback (scenarios B and J above ran through it). Both consumers read it.
        constexpr float kFogNight = -0.1086f; // capture 2: the sun 6.2 degrees below the horizon
        constexpr float kFogDay = 0.1830f;    // capture 1: the sun well above it
        constexpr float kFogHorizon = -0.0119f; // capture 4: the sun 0.7 degrees below the horizon, which the player saw as daylight
        constexpr float kTwilight[3] { 0.3f, 0.6f, 0.9f }; // a dusk-blue light (luminance 0.558, colour curve 0.43): between the night and the day colour
        const auto frameWith = [&](ID3D11Buffer* colour, uint64_t& f, uint64_t step, bool publish, float y) {
            if (publish)
                fogsun::Publish(y);
            g_scene.Bind(mainPs.Get(), colour);
            (void) DrawAt(f);
            g_scene.WaitGpu();
            f += step;
        };
        const std::vector<float> dayBytes = g_scene.LightBytes(kDay);
        ComPtr<ID3D11Buffer> dayMain = g_scene.MakeBuffer(dayBytes.data(), 512);
        const std::vector<float> twilightBytes = g_scene.LightBytes(kTwilight);
        ComPtr<ID3D11Buffer> twilightMain = g_scene.MakeBuffer(twilightBytes.data(), 512);
        CHECK(dayMain && twilightMain);
        eng::ScaleEntry entries2[8] {};
        float curve2 = -1.0f;

        // (a) the fog sun says night while the main view's colour says dusk (curve 0.43): the curve is the fog sun's (0.958), for the ambient table AND the mask lift
        Configure(0.0f, 2.0f, true);
        CHECK(fogsun::Wanted()); // the route is wanted and the curve is ticked: the fog-post capture is asked for
        for (int i = 0; i < 4; ++i)
            frameWith(twilightMain.Get(), frame, 1, true, kFogNight);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries2, 8, &curve2), 2u);
        CHECK(curve2 > 0.95f && curve2 < 0.97f);
        CHECK(std::fabs(sh::CurrentStatus().curve - curve2) < 1.0e-6f);
        CHECK_EQ(CountLogsContaining("reads the fog sun"), size_t(1)); // the source line, once per change of source
        Configure(0.5f, 1.0f, true);
        for (int i = 0; i < 4; ++i)
            frameWith(twilightMain.Get(), frame, 1, true, kFogNight);
        g_scene.Bind(g_scene.lightPs.Get(), characterLight.Get());
        const Payload liftedByFog = DrawAt(frame++);
        CHECK(liftedByFog.mask != nullptr && liftedByFog.mask != g_scene.mask.Get()); // the mask lift ran on the fog sun's night, with a dusk colour

        // (a2) LIVE1 (2026-11-07): a BRIGHT light is day whatever a lagging fog row says (the session logged curve 1.000 next to daylight colours):
        // the fog row still says night here, the colour is the day colour, and nothing is scaled or lifted.
        Configure(0.0f, 2.0f, true);
        for (int i = 0; i < 4; ++i)
            frameWith(dayMain.Get(), frame, 1, true, kFogNight);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries2, 8, &curve2), 0u);
        Configure(0.5f, 1.0f, true);
        for (int i = 0; i < 4; ++i)
            frameWith(dayMain.Get(), frame, 1, true, kFogNight);
        g_scene.Bind(g_scene.lightPs.Get(), characterLight.Get());
        const Payload keptByBright = DrawAt(frame++);
        CHECK(keptByBright.mask == g_scene.mask.Get()); // the game's own mask: the bright light vetoes the fog row's night

        // (b) the fog sun is 0.7 degrees below the horizon (rc6 capture 4, which the player saw as daylight) while the main view's colour says night:
        // nothing is lifted, nothing is scaled
        Configure(0.0f, 2.0f, true);
        for (int i = 0; i < 4; ++i)
            frameWith(mainLight.Get(), frame, 1, true, kFogHorizon);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries2, 8, &curve2), 0u);
        Configure(0.5f, 1.0f, true);
        for (int i = 0; i < 4; ++i)
            frameWith(mainLight.Get(), frame, 1, true, kFogHorizon);
        g_scene.Bind(g_scene.lightPs.Get(), characterLight.Get());
        const Payload keptByFog = DrawAt(frame++);
        CHECK(keptByFog.mask == g_scene.mask.Get()); // the game's own mask: the curve is 0 on a sun at the horizon

        // (b2) LIVE1 (2026-11-07): the fog row says the body is well ABOVE the horizon while the engine's own light is already the moon's: it is the moon,
        // so it is night (the first rc7c session's midnight: curve 0.000 and nothing lifted). The ambient table and the mask lift both follow.
        Configure(0.0f, 2.0f, true);
        for (int i = 0; i < 4; ++i)
            frameWith(mainLight.Get(), frame, 1, true, kFogDay);
        CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries2, 8, &curve2), 2u);
        CHECK_EQ(curve2, 1.0f);
        Configure(0.5f, 1.0f, true);
        for (int i = 0; i < 4; ++i)
            frameWith(mainLight.Get(), frame, 1, true, kFogDay);
        g_scene.Bind(g_scene.lightPs.Get(), characterLight.Get());
        const Payload liftedByMoon = DrawAt(frame++);
        CHECK(liftedByMoon.mask != nullptr && liftedByMoon.mask != g_scene.mask.Get());

        // (c) the fog sun goes stale (no publish for kFogSunMaxAgeFrames rendered frames): the curve falls back to the colour, at 1x and at 4x
        // stepping of the present counter (frame generation), after the same number of RENDERED frames.
        for (const uint64_t step : { uint64_t(1), uint64_t(4) })
        {
            Configure(0.0f, 2.0f, true);
            frameWith(mainLight.Get(), frame, step, true, kFogHorizon); // one publish: the sun is at the horizon, the colour says night
            for (int i = 0; i < 4; ++i)
                frameWith(mainLight.Get(), frame, step, false, 0.0f);
            CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries2, 8, &curve2), 0u); // fresh: the sun's day wins over the night colour
            for (uint32_t i = 0; i < sh::kFogSunMaxAgeFrames - 8; ++i)
                frameWith(mainLight.Get(), frame, step, false, 0.0f);
            CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries2, 8, &curve2), 0u); // still inside the bound
            for (uint32_t i = 0; i < 16; ++i)
                frameWith(mainLight.Get(), frame, step, false, 0.0f);
            CHECK_EQ(sh::Hooks::ambientEntries(g_scene.context.Get(), entries2, 8, &curve2), 2u); // past it: the night colour, curve 1
            CHECK_EQ(curve2, 1.0f);
            CHECK_EQ(CountLogsContaining("no fog sun"), size_t(1)); // the fallback's own line, once
        }

        // (d) the flag: off with the curve unticked, off with shadow mapping off (the shipped default); the capture stays off
        Configure(0.5f, 1.0f, false);
        CHECK(!fogsun::Wanted());
        sh::OnConfigLoaded(false);
        CHECK(!fogsun::Wanted());
    }

    sh::Hooks::lightDrawRoute = g_realRoute;
    return Finish("shadows_route_warp");
}
