// Puddles at the tone map: the chained order's own entries, on D3D11 WARP.
//
// The WARP formula cases (puddle_tone_warp) drive TonePass::Run directly, which says what the shader does with a scene and says nothing about who
// hands it one. This test drives the draw path's entries instead, with the state they read:
//   ToneArmedForChain()   - the gate the light shafts ask before their own copy is bound at t0,
//   OnShaftsScene()       - the chained run, on the air-lit scene the shafts hand over, and
//   OnToneStageDraw()     - the seam's own call, which has to leave the tone map alone once the chain has answered it (and to run it when the
//                           chain did not).
// The frame's composite is recorded the way the game records it: a pixel shader stamped with the slots this module reads, bound over the scene it
// writes, with the frame's view positions, G-buffer normal and camera table at those slots. So the chain runs here on a real composite, a real
// device context and a real pass, and its output is compared texel for texel with the same pass run directly on the air-lit scene.
//
// What a wrong ordering or a wrong hand-over breaks, and what is checked:
//   - the gate is the stage's own: off before any composite of the frame, on after one, and off again once the chain has answered it,
//   - the chain runs on the scene the light shafts hand over, not on the composite's one: its output is the pass's output for the air-lit scene to
//     the bit, and differs from the pass's output for the pre-shafts scene, which is what the air light changes,
//   - an answer is stamped: the seam's own call returns false for that tone map, runs the game's draw nowhere and runs no second pass,
//   - a dry frame refuses: the chain returns null, so the shafts keep their own copy, and nothing is stamped or counted,
//   - a frame whose composite carries no shape (a gloss refuse no hold cleared): the chain binds its own last good
//     answer at t0 and does not count it as a run, so a skip in the middle of the ramp shows neither stage's absence,
//   - the fallback still runs a tone map the chain did not take: it draws once, with this module's texture at t0 for that draw and the game's own
//     view back afterwards,
//   - the gate: the pass is handed the engine's material pack AS THE FRAME'S COMPOSITE BOUND IT, so a dry half of that texture gets no pool and an
//     exposed half is today's picture; a composite that binds no material pack (or one that is not an 8-bit BGRA texture, or a key that names another
//     target) leaves the stage nothing to read and draws no pool; and a frame whose composite binds none after a frame whose composite did NEVER
//     reuses the earlier frame's texture (the pairing-fault class of the black frame).
#include "check.h"

#include "ext/ffxiv/engine/GlossPuddlesApply.h"
#include "ext/ffxiv/engine/EnginePassRules.h"
#include "ext/ffxiv/engine/PuddleTone.h"
#include "ext/ffxiv/engine/PuddleTonePass.h"
#include "ext/ffxiv/engine/ShaftsRules.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::gloss;

namespace
{
// ---- the two neighbours the draw path asks, which this harness is instead of --------------------------------------------------
//
// The engine module's own-work flag and the gloss stage's weather are the neighbours' state, not the chain's: the chain only sets and asks them,
// so the harness answers. Everything else the chain touches (the stamp, the pass, the pure rules) is the product's own code.
float g_wetness = 1.0f;
bool g_wetnessKnown = true;
ID3D11ShaderResourceView* g_wetnessView = nullptr;
bool g_pairing = true; // the pairing's stamp: whether the frame's composite carries the gloss stage's shape

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 192;
constexpr uint32_t kSceneFormat = 10; // DXGI_FORMAT_R16G16B16A16_FLOAT, the composite's own
constexpr uint32_t kNormalFormat = 87; // DXGI_FORMAT_B8G8R8A8_UNORM, the G-buffer's own
constexpr float kFloor = 0.25f;
constexpr float kAirLight = 0.1f; // the light shafts' addition, as bright as this fixture's values leave room for

// ---- half floats -------------------------------------------------------------------------------------------------------------

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

// ---- the device and the frame's buffers --------------------------------------------------------------------------------------

struct Fixture
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> sceneTexture, litTexture, toneTexture, positionTexture, normalTexture, glossTexture;
    ComPtr<ID3D11ShaderResourceView> sceneView, litView, positionView, normalView, toneView, glossView;
    ComPtr<ID3D11RenderTargetView> sceneRtv, litRtv, toneRtv;
    ComPtr<ID3D11Buffer> camera, wetness;
    ComPtr<ID3D11ShaderResourceView> wetnessView;
    ComPtr<ID3D11PixelShader> composite;
    ComPtr<ID3D11PixelShader> compositeNoGloss; // the same shader stamped without a material-pack slot: a composite that declares none

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;
        if (device == nullptr || context == nullptr)
            return false;

        // The scene the composite wrote (twice: as the engine left it, and with the shafts' air light added), the tone map's own target, and the
        // frame's view positions and normal, all at the display size.
        std::vector<uint16_t> plain(size_t(kWidth) * kHeight * 4, 0);
        std::vector<uint16_t> lit(plain.size(), 0);
        std::vector<uint16_t> positions(plain.size(), 0);
        std::vector<uint8_t> normals(size_t(kWidth) * kHeight * 4, 0);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t index = (size_t(y) * kWidth + x) * 4;
                // The floor at y = -1 in view space with the camera at the origin looking down -z: the world point is the view point, so the
                // noise field reads a real place and the pool is a real shape.
                const float viewX = (float(x) / float(kWidth) - 0.5f) * 2.0f;
                const float viewZ = -1.0f - float(y) / float(kHeight) * 4.0f;
                positions[index + 0] = HalfOf(viewX);
                positions[index + 1] = HalfOf(-1.0f);
                positions[index + 2] = HalfOf(viewZ);
                positions[index + 3] = HalfOf(1.0f);
                plain[index + 0] = HalfOf(kFloor);
                plain[index + 1] = HalfOf(kFloor);
                plain[index + 2] = HalfOf(kFloor);
                plain[index + 3] = HalfOf(1.0f);
                lit[index + 0] = HalfOf(kFloor + kAirLight);
                lit[index + 1] = HalfOf(kFloor + kAirLight);
                lit[index + 2] = HalfOf(kFloor + kAirLight);
                lit[index + 3] = HalfOf(1.0f);
                // The G-buffer normal the way the game writes it: world space, n * 0.5 + 0.5, B8G8R8A8 in memory.
                normals[index + 0] = 128; // blue: nz = 0
                normals[index + 1] = 255; // green: ny = 1, the world's up
                normals[index + 2] = 128; // red: nx = 0
                normals[index + 3] = 255;
            }
        // The engine's material pack, B8G8R8A8 like the game's, exposed ground everywhere (alpha 0.83, the engine's own value at full rain): the gate
        // reads exposure 1 on it, so every case below that does not say otherwise is today's picture.
        std::vector<uint8_t> gloss(size_t(kWidth) * kHeight * 4, 0);
        for (size_t i = 0; i < gloss.size(); i += 4)
        {
            gloss[i + 0] = 90;
            gloss[i + 1] = 120;
            gloss[i + 2] = 255; // red: the selector byte, never a gloss
            gloss[i + 3] = 212; // alpha: 0.83
        }
        if (!CreateTexture(kNormalFormat, gloss, glossTexture, glossView, nullptr))
            return false;
        if (!CreateTexture(kSceneFormat, plain, sceneTexture, sceneView, &sceneRtv) ||
            !CreateTexture(kSceneFormat, lit, litTexture, litView, &litRtv) ||
            !CreateTexture(kSceneFormat, std::vector<uint16_t>(plain.size(), 0), toneTexture, toneView, &toneRtv) ||
            !CreateTexture(kSceneFormat, positions, positionTexture, positionView, nullptr) ||
            !CreateTexture(kNormalFormat, normals, normalTexture, normalView, nullptr))
            return false;

        {
            // The camera table the engine's shaders read: a plain view matrix (the camera at the origin) and its inverse, the projection at
            // register 18. The stage turns a view point into a world point with the inverse view's rows at 3..5.
            std::vector<float> rows(22 * 4, 0.0f);
            for (uint32_t r = 0; r < 3; ++r)
                rows[(r * 4) + r] = 1.0f; // the view matrix's rows 0..2
            for (uint32_t r = 3; r < 6; ++r)
                rows[(r * 4) + (r - 3)] = 1.0f; // the inverse view's rows 3..5
            for (uint32_t r = 18; r < 22; ++r)
                rows[(r * 4) + (r - 18)] = 1.0f; // the projection's rows 18..21
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = uint32_t(rows.size() * 4);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA data { rows.data(), 0, 0 };
            if (FAILED(device->CreateBuffer(&desc, &data, &camera)))
                return false;
        }
        {
            // The gloss pass's own 32-byte wetness buffer, which both stages gate on: .x is the amount the engine's draws read.
            const float values[kWetnessBytes / 4] { 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            D3D11_BUFFER_DESC desc {};
            desc.ByteWidth = kWetnessBytes;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data { values, 0, 0 };
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc {};
            viewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            viewDesc.Buffer.NumElements = kWetnessBytes / 16;
            if (FAILED(device->CreateBuffer(&desc, &data, &wetness)) ||
                FAILED(device->CreateShaderResourceView(wetness.Get(), &viewDesc, &wetnessView)))
                return false;
            g_wetnessView = wetnessView.Get();
        }
        return CreateCompositeShader();
    }

    template <class Element>
    bool CreateTexture(uint32_t format, const std::vector<Element>& pixels, ComPtr<ID3D11Texture2D>& texture,
                       ComPtr<ID3D11ShaderResourceView>& view, ComPtr<ID3D11RenderTargetView>* rtv)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT(format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv != nullptr ? D3D11_BIND_RENDER_TARGET : 0u);
        D3D11_SUBRESOURCE_DATA data { pixels.data(), kWidth * uint32_t(sizeof(Element) * 4), 0 };
        if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) || texture == nullptr)
            return false;
        if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
            return false;
        if (rtv != nullptr && FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, rtv->GetAddressOf())))
            return false;
        return true;
    }

    // A pixel shader the frame's composite could be: it declares the view positions, the normal, the depth and the camera table, and the harness
    // stamps it with the slots this module reads, the way the creation hook stamps the game's own.
    bool CreateCompositeShader()
    {
        static const char* kSource = R"(
Texture2D<float4> g_SamplerGBuffer1 : register(t0);
Texture2D<float4> g_SamplerGBuffer : register(t1);
Texture2D<float4> g_SamplerViewPosition : register(t2);
Texture2D<float4> g_SamplerDepth : register(t3);
cbuffer g_CameraParameter : register(b0) { float4 camera[22]; };
float4 PS(float4 position : SV_Position) : SV_Target
{
    int2 at = int2(position.xy);
    float4 a = g_SamplerGBuffer1.Load(int3(at, 0));
    float4 b = g_SamplerGBuffer.Load(int3(at, 0));
    float4 c = g_SamplerViewPosition.Load(int3(at, 0));
    float4 d = g_SamplerDepth.Load(int3(at, 0));
    return float4(a.rgb + b.rgb + c.rgb + d.rgb + camera[0].rgb, 1.0);
}
)";
        ComPtr<ID3DBlob> code;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(kSource, std::strlen(kSource), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &code, &errors);
        if (FAILED(hr) || code == nullptr)
        {
            if (errors)
                std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
            return false;
        }
        if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &composite)))
            return false;
        Inputs slots;
        slots.depth = 3;      // g_SamplerDepth at t3
        slots.camera = 0;     // g_CameraParameter at b0
        slots.vposition = 2;  // g_SamplerViewPosition at t2
        slots.targets[0] = 0; // g_SamplerGBuffer1 at t0 ...
        slots.targets[3] = 1; // ... and the world-space normal at t1
        if (!detail::StampShader(composite.Get(), PackInputs(slots)))
            return false;
        // The same shader object code stamped without the material pack: a composite that declares no g_SamplerGBuffer1.
        if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &compositeNoGloss)))
            return false;
        Inputs without = slots;
        without.targets[0] = kSlotMissing;
        return detail::StampShader(compositeNoGloss.Get(), PackInputs(without));
    }

    // The material pack's alpha, per texel, as the gate will read it. Rewritten in place, so a view bound earlier keeps naming the same texture.
    void SetGloss(uint8_t (*alphaOf)(uint32_t x, uint32_t y))
    {
        std::vector<uint8_t> gloss(size_t(kWidth) * kHeight * 4, 0);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t i = (size_t(y) * kWidth + x) * 4;
                gloss[i + 0] = 90;
                gloss[i + 1] = 120;
                gloss[i + 2] = 255;
                gloss[i + 3] = alphaOf(x, y);
            }
        context->UpdateSubresource(glossTexture.Get(), 0, nullptr, gloss.data(), kWidth * 4, 0);
    }

    // The frame's composite: its shader is the current pixel shader and the scene it writes is its only render target. `lit` picks the scene the
    // shafts have already added their air light to, which is what the chain reads; both frames bind the same buffers at the same slots.
    // `withGloss` false binds the composite that declares no material pack; `wrongFormat` binds a half-float texture where the material pack
    // would be (a binding the gate must not read as an 8-bit alpha).
    void BindComposite(bool lit, bool withGloss = true, bool wrongFormat = false)
    {
        ID3D11ShaderResourceView* views[4] { wrongFormat ? sceneView.Get() : glossView.Get(), normalView.Get(), positionView.Get(), positionView.Get() };
        ID3D11Buffer* buffers[1] { camera.Get() };
        ID3D11RenderTargetView* rtvs[1] { (lit ? litRtv : sceneRtv).Get() };
        context->PSSetShader((withGloss ? composite : compositeNoGloss).Get(), nullptr, 0);
        context->PSSetShaderResources(0, 4, views);
        context->PSSetConstantBuffers(0, 1, buffers);
        context->OMSetRenderTargets(1, rtvs, nullptr);
        const D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
    }

    // The tone map's own shape: one FP16 target that is not the scene, and t0 the scene the frame's composite wrote.
    void BindToneMap(ID3D11ShaderResourceView* scene)
    {
        ID3D11ShaderResourceView* views[1] { scene };
        ID3D11RenderTargetView* rtvs[1] { toneRtv.Get() };
        context->PSSetShaderResources(0, 1, views);
        context->OMSetRenderTargets(1, rtvs, nullptr);
        const D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
    }

    // The tone map's shape as the light shafts measure it, which the chain classifies with the same rule the fallback uses.
    ext::ffxiv::shafts::ToneMapFacts ShaftFacts()
    {
        ext::ffxiv::shafts::ToneMapFacts facts;
        facts.renderTargets = 1;
        facts.targetWidth = kWidth;
        facts.targetHeight = kHeight;
        facts.targetFormat = kSceneFormat;
        facts.targetIsScene = false;
        facts.sceneFormat = kSceneFormat;
        facts.sceneWidth = kWidth;
        facts.sceneHeight = kHeight;
        facts.sceneSampled = true;
        facts.sceneIsScene = true;
        facts.window = ext::ffxiv::shafts::Window { 0.0f, 0.0f, float(kWidth), float(kHeight) };
        return facts;
    }

    // The pass's inputs, as the draw path builds them from the frame's capture: the frame's own buffers, and the keys.
    TonePassInputs PassInputs(ID3D11ShaderResourceView* scene, ID3D11Texture2D* sceneTexture, const Params& params)
    {
        ext::ffxiv::engine::gloss::ToneFacts facts;
        facts.renderTargets = 1;
        facts.targetWidth = kWidth;
        facts.targetHeight = kHeight;
        facts.targetFormat = kSceneFormat;
        facts.sceneFormat = kSceneFormat;
        facts.sceneWidth = kWidth;
        facts.sceneHeight = kHeight;
        facts.sceneSampled = true;
        facts.sceneIsScene = true;
        facts.viewPositionWidth = kWidth;
        facts.viewPositionHeight = kHeight;
        facts.viewPositionSampled = true;
        facts.normalWidth = kWidth;
        facts.normalHeight = kHeight;
        facts.normalSampled = true;
        facts.cameraBytes = kCameraBytes;
        facts.window[0] = 0.0f;
        facts.window[1] = 0.0f;
        facts.window[2] = float(kWidth);
        facts.window[3] = float(kHeight);
        TonePassInputs inputs;
        inputs.scene = scene;
        inputs.sceneTexture = sceneTexture;
        inputs.viewPosition = positionView.Get();
        inputs.normal = normalView.Get();
        inputs.camera = camera.Get();
        inputs.wetness = wetnessView.Get();
        inputs.gate = glossView.Get(); // the frame's material pack, as its composite bound it
        inputs.facts = facts;
        inputs.params = params;
        inputs.stats = false;
        // The raindrops' clock the stage itself animated from: these passes reproduce the chained picture, and the chain reads a real clock, so a
        // pass left at its default zero would draw the still pool and differ from the chain wherever a ring moved the mirror.
        inputs.timeSeconds = ext::ffxiv::engine::gloss::LastRippleSeconds();
        inputs.rain = ext::ffxiv::engine::gloss::LastRainAmount();
        return inputs;
    }

    // The bits of a texture the stage wrote, as the tone map would read them.
    std::vector<uint16_t> ReadBits(ID3D11Texture2D* texture)
    {
        std::vector<uint16_t> out;
        if (texture == nullptr)
            return out;
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> copy;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy)) || copy == nullptr)
            return out;
        context->CopyResource(copy.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return out;
        out.resize(size_t(desc.Width) * desc.Height * 4);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(&out[size_t(y) * desc.Width * 4], static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        size_t(desc.Width) * 4 * sizeof(uint16_t));
        context->Unmap(copy.Get(), 0);
        return out;
    }

    // The hold's answer is a view, and its texture is the same one the pass returns: the description is read from the
    // view's own resource.
    std::vector<uint16_t> ReadBitsFromView(ID3D11ShaderResourceView* view)
    {
        ComPtr<ID3D11Texture2D> texture;
        if (view != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            if (resource != nullptr)
                resource.As(&texture);
        }
        return ReadBits(texture.Get());
    }

    void Unbind()
    {
        ID3D11ShaderResourceView* none[8] {};
        context->PSSetShaderResources(0, 8, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
};

// The draw lambda the seam hands the game's own call: it counts the runs, so a draw that ran twice or not at all is a failing check. When it is given
// a context it also records what the pixel shader's t0 was bound to at the moment the game's own draw ran, which is the only place the chained
// order's binding can be observed.
struct DrawProbe
{
    uint32_t calls = 0;
    ID3D11DeviceContext* context = nullptr;
    ID3D11ShaderResourceView* bound = nullptr;

    static void Run(void* user) noexcept
    {
        DrawProbe* probe = static_cast<DrawProbe*>(user);
        probe->calls++;
        if (probe->context != nullptr && probe->bound == nullptr)
            probe->context->PSGetShaderResources(0, 1, &probe->bound);
    }
};
} // namespace

namespace ext::ffxiv::engine
{
// The engine module's own-work flag, which this harness has no module for: the chain sets it around its own pass and nothing here can see it.
void SetOwnWork(bool on) { (void) on; }
} // namespace ext::ffxiv::engine

namespace ext::ffxiv::engine::gloss
{
// The gloss stage's weather, which the chain only asks, and the 32-byte buffer both stages bind.
Wetness CurrentWetness() { return Wetness { g_wetness, g_wetnessKnown }; }
ID3D11ShaderResourceView* WetnessResource() { return g_wetnessView; }
// The wetness latch is the shared pass's own bit (one per session); this harness answers for it with the same flag
// the dry gate flips, so the chain's wet and dry frames stay reachable.
bool g_latched = true; // the pass's own latch: the checks set it the way the frame's wetness copy would
bool g_cameraRejected = false; // the gloss layout check's answer: false keeps the hold answering, true falls back to the caller's copy
bool WetLatched() { return g_latched; }
bool ShapeOnFrame(uint64_t token) { (void) token; return g_pairing; }
bool GlossCameraRejected() { return g_cameraRejected; }
} // namespace ext::ffxiv::engine::gloss

namespace ext::present
{
// The output-frame counter (the capture's frame token, the frame the pairing is asked about): the harness has no
// presentation, so every frame reads one token, which is what one true frame's capture needs.
uint64_t FrameCount() noexcept { return 1; }
} // namespace ext::present

int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    Fixture fixture;
    if (!fixture.Create())
    {
        std::printf("no WARP device: the puddles' chained order cannot be checked here\n");
        return 1;
    }

    ComPtr<ID3D11Texture2D> fallbacksAnswer; // the fallback's own answer, kept for the hold's comparison below
    ext::ffxiv::engine::gloss::Params params;
    params.on = true;
    PublishTone(params);

    // Nothing to chain yet: no composite of this frame, so the light shafts' copy is what the tone map reads.
    CHECK(!ToneArmedForChain());

    // The frame's composite: the draw that binds the view positions is recorded, with the air-lit scene it writes.
    fixture.BindComposite(true);
    DrawProbe probe;
    CHECK(!OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsCompositeBit, &DrawProbe::Run, &probe));
    CHECK_EQ(probe.calls, 0u);
    CHECK_EQ(CurrentToneStatus().captures, 1u);

    // The gate the light shafts ask: a composite of this frame that no tone map has answered.
    CHECK(ToneArmedForChain());

    // The chained run, on the scene the shafts hand over.
    fixture.BindToneMap(fixture.litView.Get());
    const ext::ffxiv::shafts::ToneMapFacts facts = fixture.ShaftFacts();
    ID3D11ShaderResourceView* chained = OnShaftsScene(fixture.context.Get(), facts, fixture.litView.Get());
    CHECK(chained != nullptr);
    CHECK(chained != fixture.litView.Get()); // this module's own texture, not the scene it was handed
    CHECK_EQ(CurrentToneStatus().runs, 1u);
    ComPtr<ID3D11Texture2D> chainedTexture;
    if (chained != nullptr)
    {
        ComPtr<ID3D11Resource> resource;
        chained->GetResource(&resource);
        if (resource != nullptr)
            resource.As(&chainedTexture);
    }
    const std::vector<uint16_t> chainedBits = fixture.ReadBits(chainedTexture.Get());
    CHECK_EQ(chainedBits.size(), size_t(kWidth) * kHeight * 4);

    // The answer is stamped: the gates that ask "is it still to do" now say no, and the seam's own call leaves the draw alone.
    CHECK(!ToneArmedForChain());
    fixture.BindToneMap(fixture.litView.Get());
    probe = DrawProbe {};
    CHECK(!OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsToneMapBit, &DrawProbe::Run, &probe));
    CHECK_EQ(probe.calls, 0u);
    CHECK_EQ(CurrentToneStatus().runs, 1u); // no second pass over the same composite

    // What the chain read: the pass run directly, twice - on the scene the shafts handed over and on the composite's own scene before the air
    // light. The chain's picture has to be the first to the bit, and it has to differ from the second wherever the pool is, which is what says the
    // hand-over reached the shader.
    {
        TonePass litPass;
        CHECK(litPass.Run(fixture.context.Get(), fixture.PassInputs(fixture.litView.Get(), fixture.litTexture.Get(), params)) != nullptr);
        const std::vector<uint16_t> litBits = fixture.ReadBits(litPass.OutputTexture());

        TonePass plainPass;
        CHECK(plainPass.Run(fixture.context.Get(), fixture.PassInputs(fixture.sceneView.Get(), fixture.sceneTexture.Get(), params)) != nullptr);
        const std::vector<uint16_t> plainBits = fixture.ReadBits(plainPass.OutputTexture());

        CHECK_EQ(litBits.size(), chainedBits.size());
        CHECK_EQ(plainBits.size(), chainedBits.size());
        if (litBits.size() == chainedBits.size() && plainBits.size() == chainedBits.size())
        {
            size_t fromLit = 0;
            size_t litDiffersFromPlain = 0;
            for (size_t i = 0; i + 3 < chainedBits.size(); i += 4)
            {
                const bool lit = chainedBits[i] == litBits[i] && chainedBits[i + 1] == litBits[i + 1] && chainedBits[i + 2] == litBits[i + 2];
                const bool sameAsPlain = litBits[i] == plainBits[i] && litBits[i + 1] == plainBits[i + 1] && litBits[i + 2] == plainBits[i + 2];
                fromLit += lit ? 1u : 0u;
                litDiffersFromPlain += sameAsPlain ? 0u : 1u;
            }
            CHECK_EQ(fromLit, size_t(kWidth) * kHeight); // the chain's scene is the shafts' own output, texel for texel
            CHECK(litDiffersFromPlain > 100); // ... and the air light makes that output a different picture, so the match asserts something
        }
    }

    // A dry frame: the chain refuses, so the shafts keep the copy they already have, and nothing is stamped or counted for it.
    {
        PublishTone(params); // a fresh publish, so the frame's capture starts empty and the shafts ask again
        fixture.BindComposite(false);
        probe = DrawProbe {};
        CHECK(!OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsCompositeBit, &DrawProbe::Run, &probe));
        CHECK(ToneArmedForChain());
        fixture.BindToneMap(fixture.sceneView.Get());
        g_wetness = 0.0f;
        g_latched = false; // a dry copy at exact zero unlatches the shared latch, whatever the run was
        CHECK(OnShaftsScene(fixture.context.Get(), facts, fixture.sceneView.Get()) == nullptr);
        CHECK(ToneArmedForChain()); // a dry frame did not answer the composite: nothing was stamped for it
        CHECK_EQ(CurrentToneStatus().runs, 1u); // the chain's run is still the only one: the dry frame ran nothing
        // The wetness gate's own reply holds: the next wet copy re-arms the latch (NoteWetnessFrame's rule), which is
        // what the fallback below - a later frame - reads.
        g_wetness = 1.0f;
        g_latched = true;
    }

    // And the fallback still owns a tone map the chain did not take: it runs the stage, draws once, and leaves t0 as the game had it.
    {
        fixture.BindToneMap(fixture.sceneView.Get());
        ID3D11ShaderResourceView* before = nullptr;
        fixture.context->PSGetShaderResources(0, 1, &before);
        probe = DrawProbe { 0, fixture.context.Get(), nullptr }; // the probe reads t0 as the game's own draw is called
        CHECK(OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsToneMapBit, &DrawProbe::Run, &probe));
        CHECK_EQ(probe.calls, 1u); // the game's own draw ran exactly once
        ID3D11ShaderResourceView* after = nullptr;
        fixture.context->PSGetShaderResources(0, 1, &after);
        CHECK(after == before); // the game's own view is back at t0
        if (before != nullptr)
            before->Release();
        if (after != nullptr)
            after->Release();
        CHECK_EQ(CurrentToneStatus().runs, 2u); // the fallback's own run, after the chain's

        // The fallback's own answer, kept for the hold's comparison below: the tone map read it as bound, so it is
        // the pass's own texture for the scene the fallback was handed.
        CHECK(probe.bound != nullptr);
        if (probe.bound != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            probe.bound->GetResource(&resource);
            if (resource != nullptr)
                resource.As(&fallbacksAnswer);
        }
    }

    // A frame whose composite carries no shape (what a gloss refuse the pass's hold could not clear leaves the
    // pairing saying): the chain answers with its own LAST GOOD OUTPUT instead of the scene it was handed, so a skip
    // in the middle of the ramp binds the pools' previous look rather than a frame the tone stage never shaded - the
    // hold, which is why a ramp skip was never a flip between the staged look and the engine's own picture.
    {
        PublishTone(params); // a fresh publish: the capture starts empty, and the pass's last answer is the fallback's
        g_pairing = false;   // the frame the pairing refuses: the composite's gloss channel carries no shape of this stage's
        fixture.BindComposite(true);
        probe = DrawProbe {};
        CHECK(!OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsCompositeBit, &DrawProbe::Run, &probe));
        CHECK(ToneArmedForChain());
        const uint32_t runsBefore = CurrentToneStatus().runs;
        fixture.BindToneMap(fixture.litView.Get());
        ID3D11ShaderResourceView* held = OnShaftsScene(fixture.context.Get(), facts, fixture.litView.Get());
        CHECK(held != nullptr);
        CHECK(held != fixture.litView.Get()); // the hold is this stage's own last answer, not the scene it was handed
        CHECK_EQ(CurrentToneStatus().runs, runsBefore); // a hold is not a run: the pass never ran for this frame
        CHECK(!ToneArmedForChain()); // the composite was answered, with the hold, so the fallback is left alone

        // The hold's picture is the last pass's answer, to the bit: the fallback's own answer, which this pass still holds.
        const std::vector<uint16_t> holdBits = fixture.ReadBitsFromView(held);
        const std::vector<uint16_t> lastBits = fixture.ReadBits(fallbacksAnswer.Get());
        CHECK_EQ(holdBits.size(), lastBits.size());
        if (holdBits.size() == lastBits.size())
            CHECK(holdBits == lastBits); // the held frame binds exactly what the pass last produced
        g_pairing = true; // the skip-path case below needs the pairing on again
    }

    // A frame the light shafts leave as they were - their sun behind the camera, their light not the frame's sun, no sun draw of the frame - still
    // has pools in it. With their card on this stage's own fallback never sees the tone map, so the shafts' skip path draws through the chained
    // entry below; without it, turning Light Shafts on turned the pools off. The entry draws exactly once, with this stage's picture at t0 when it
    // had anything to do and with the scene it was handed when it did not.
    {
        PublishTone(params);
        fixture.BindComposite(true);
        probe = DrawProbe {};
        CHECK(!OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsCompositeBit, &DrawProbe::Run, &probe));
        CHECK(ToneArmedForChain()); // the composite the shafts recorded, and no tone map of this frame has answered it

        fixture.BindToneMap(fixture.litView.Get());
        ID3D11ShaderResourceView* before = nullptr;
        fixture.context->PSGetShaderResources(0, 1, &before);
        const uint32_t runsBefore = CurrentToneStatus().runs;
        probe = DrawProbe { 0, fixture.context.Get(), nullptr };
        OnShaftsSceneDraw(fixture.context.Get(), facts, fixture.litView.Get(), &DrawProbe::Run, &probe);
        CHECK_EQ(probe.calls, 1u); // the game's own draw ran exactly once on the frame the shafts left alone
        CHECK(probe.bound != nullptr && probe.bound != fixture.litView.Get()); // the tone map read the pool's picture, not the shafts' scene
        CHECK_EQ(CurrentToneStatus().runs, runsBefore + 1); // and the chain ran on it
        CHECK(!ToneArmedForChain()); // the composite is answered, so the seam's own call cannot run a second pass over it
        ID3D11ShaderResourceView* after = nullptr;
        fixture.context->PSGetShaderResources(0, 1, &after);
        CHECK(after == before); // the game's own view is back at t0
        if (before != nullptr)
            before->Release();
        if (after != nullptr)
            after->Release();

        // The picture the tone map read is the pool's: it differs from the scene the shafts handed over in the pool's own texels, so the pools were
        // drawn and not passed through.
        ComPtr<ID3D11Texture2D> boundTexture;
        if (probe.bound != nullptr)
        {
            ComPtr<ID3D11Resource> resource;
            probe.bound->GetResource(&resource);
            if (resource != nullptr)
                resource.As(&boundTexture);
        }
        const std::vector<uint16_t> poolBits = fixture.ReadBits(boundTexture.Get());
        const std::vector<uint16_t> handoverBits = fixture.ReadBits(fixture.litTexture.Get());
        CHECK_EQ(poolBits.size(), handoverBits.size());
        if (poolBits.size() == handoverBits.size())
        {
            size_t moved = 0;
            for (size_t i = 0; i + 3 < poolBits.size(); i += 4)
                moved += poolBits[i] != handoverBits[i] || poolBits[i + 1] != handoverBits[i + 1] || poolBits[i + 2] != handoverBits[i + 2] ? 1u : 0u;
            CHECK(moved > 100); // the pool's colour and a mirror of the scene, texel for texel in the pool's shape
        }
        if (probe.bound != nullptr)
            probe.bound->Release();

        // A dry frame through the same entry: the stage refuses, the game's draw still runs once, and the tone map reads the scene it was handed.
        PublishTone(params);
        fixture.BindComposite(false);
        probe = DrawProbe {};
        CHECK(!OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsCompositeBit, &DrawProbe::Run, &probe));
        // The tone map's own draw: its target is not the scene (a view of a texture the draw has bound as its render target is unbound by the
        // runtime), which is what a real frame's tone map looks like.
        fixture.BindToneMap(fixture.sceneView.Get());
        const uint32_t dryRuns = CurrentToneStatus().runs;
        g_wetness = 0.0f;
        g_latched = false; // the copy's exact zero unlatches the shared latch
        probe = DrawProbe { 0, fixture.context.Get(), nullptr };
        OnShaftsSceneDraw(fixture.context.Get(), facts, fixture.sceneView.Get(), &DrawProbe::Run, &probe);
        CHECK_EQ(probe.calls, 1u);
        CHECK(probe.bound == fixture.sceneView.Get()); // the scene the call was handed, unchanged
        CHECK_EQ(CurrentToneStatus().runs, dryRuns); // nothing was run for it
        if (probe.bound != nullptr)
            probe.bound->Release();
        g_wetness = 1.0f;
        g_latched = true; // the next wet copy re-arms the shared latch, as the product's tick would
    }

    // ---- the gate, through the draw path: the pass is handed the material pack the frame's composite bound, and only this frame's
    {
        // One frame through the shafts' skip-path entry: composite -> tone map. Returns how many texels of the picture the tone map read differ from the
        // scene it was handed (texels the stage shaded), split into the left half and the right half, and whether the stage ran.
        struct FrameOutcome
        {
            bool bound = false;
            uint32_t left = 0, right = 0;
            uint32_t runsAdded = 0;
        };
        const auto runFrame = [&](bool withGloss, bool wrongFormat, bool republish) {
            FrameOutcome outcome;
            if (republish)
                PublishTone(params);
            fixture.BindComposite(true, withGloss, wrongFormat);
            DrawProbe composite;
            OnToneStageDraw(fixture.context.Get(), ext::ffxiv::engine::kShaftsCompositeBit, &DrawProbe::Run, &composite);
            fixture.BindToneMap(fixture.litView.Get());
            const uint32_t runsBefore = CurrentToneStatus().runs;
            DrawProbe tone { 0, fixture.context.Get(), nullptr };
            OnShaftsSceneDraw(fixture.context.Get(), facts, fixture.litView.Get(), &DrawProbe::Run, &tone);
            outcome.runsAdded = CurrentToneStatus().runs - runsBefore;
            ComPtr<ID3D11Texture2D> boundTexture;
            if (tone.bound != nullptr)
            {
                outcome.bound = true;
                ComPtr<ID3D11Resource> resource;
                tone.bound->GetResource(&resource);
                if (resource != nullptr)
                    resource.As(&boundTexture);
                tone.bound->Release();
            }
            const std::vector<uint16_t> picture = fixture.ReadBits(boundTexture.Get());
            const std::vector<uint16_t> handed = fixture.ReadBits(fixture.litTexture.Get());
            if (picture.size() == handed.size())
                for (size_t i = 0; i + 3 < picture.size(); i += 4)
                {
                    const bool moved = picture[i] != handed[i] || picture[i + 1] != handed[i + 1] || picture[i + 2] != handed[i + 2];
                    const uint32_t x = uint32_t((i / 4) % kWidth);
                    (x < kWidth / 2 ? outcome.left : outcome.right) += moved ? 1u : 0u;
                }
            return outcome;
        };

        // (1) exposed everywhere: the pool is drawn on both halves
        g_pairing = true;
        const FrameOutcome open = runFrame(true, false, true);
        CHECK(open.bound);
        CHECK_EQ(open.runsAdded, 1u);
        CHECK(open.left > 50 && open.right > 50);

        // (2) the left half of the engine's material pack is dry: that half of the picture is the scene back to the bit, the right half is unchanged
        fixture.SetGloss([](uint32_t x, uint32_t) -> uint8_t { return x < kWidth / 2 ? uint8_t(0) : uint8_t(212); });
        const FrameOutcome half = runFrame(true, false, true);
        CHECK(half.bound);
        CHECK_EQ(half.runsAdded, 1u);
        CHECK_EQ(half.left, 0u);
        CHECK_EQ(half.right, open.right);
        std::printf("puddle_tone_chain_warp: gate through the draw path, exposed %u|%u shaded texels, left half dry %u|%u\n", open.left, open.right,
                    half.left, half.right);

        // (3) the stale-frame fault: the previous frame's composite bound a material pack (exposed again, below), THIS frame's composite declares none.
        // The stage must not reuse the earlier frame's texture: no pool anywhere. No republish in between, so the capture is the earlier frame's to
        // inherit if the gate were kept.
        fixture.SetGloss([](uint32_t, uint32_t) -> uint8_t { return uint8_t(212); });
        const FrameOutcome before = runFrame(true, false, true);
        CHECK(before.left > 50 && before.right > 50);
        const FrameOutcome stale = runFrame(false, false, false);
        CHECK(stale.bound);
        CHECK_EQ(stale.left, 0u);
        CHECK_EQ(stale.right, 0u);

        // (4) a composite that binds a texture that is not an 8-bit BGRA material pack (a half-float scene in its place) leaves the gate nothing to read
        const FrameOutcome wrong = runFrame(true, true, true);
        CHECK_EQ(wrong.left, 0u);
        CHECK_EQ(wrong.right, 0u);

        // (5) the key names a target that carries no engine gloss (target 2): the gloss stage draws none, and neither does this one
        ext::ffxiv::engine::gloss::Params other = params;
        other.target = 2;
        PublishTone(other);
        const FrameOutcome target2 = runFrame(true, false, false);
        CHECK_EQ(target2.left, 0u);
        CHECK_EQ(target2.right, 0u);
        PublishTone(params);

        // (6) and the next frame, with the key back and a material pack bound, draws the pool again: nothing stuck
        const FrameOutcome back = runFrame(true, false, false);
        CHECK(back.left > 50 && back.right > 50);
    }

    fixture.Unbind();
    g_wetnessView = nullptr;
    return Finish("puddle_tone_chain_warp");
}
