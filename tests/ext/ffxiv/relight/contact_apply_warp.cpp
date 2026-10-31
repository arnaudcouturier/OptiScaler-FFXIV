#include "pch.h" // the test stand-in: windows, d3d11 and the counting log macros the injection writes through

// Contact shadows: the injection (ext/ffxiv/relight/ContactShadowsApply.cpp) on the D3D11 WARP software adapter.
//
// The contract this file pins is the one a player would feel if it broke: the game's draw either sees the pass's mask copy and has its own
// binding back afterwards, or it sees exactly what it always saw. The copy is bound for one draw only - also when that draw throws - every
// refusal leaves the game's own shader resource in place with one log line for the reason, sixteen refusals in a row switch the module off
// for the session (a refusal about the draw's shape does not count towards that), a zero strength never runs the pass at all, and the log
// never exceeds its budget.
#include "check.h"

#include "camera_rows.h"

#include "ext/ffxiv/relight/Inject.h"

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::relight;

namespace
{
constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 36;
constexpr float kPi = 3.14159265358979f;

// What one "game draw" observed while it ran, and what it should do.
struct Payload
{
    ID3D11DeviceContext* context = nullptr;
    uint32_t slot = 0;
    bool throws = false;
    bool ran = false;
    bool threw = false;
    ID3D11ShaderResourceView* mask = nullptr; // the mask the draw would have sampled
};

// The game's own draw, as the injection hands it to the test: records the mask binding, optionally throws.
void Draw(void* user)
{
    auto* payload = static_cast<Payload*>(user);
    payload->ran = true;
    payload->mask = nullptr;
    payload->context->PSGetShaderResources(payload->slot, 1, &payload->mask);
    if (payload->mask != nullptr)
        payload->mask->Release();
    if (payload->throws)
    {
        payload->threw = true;
        throw std::runtime_error("the draw failed");
    }
}

struct Scene
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ShaderResourceView> maskView, depthView, atlasView;
    ComPtr<ID3D11Buffer> light, camera;

    bool Create()
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)))
            return false;

        std::vector<uint32_t> maskPixels(size_t(kWidth) * kHeight, 0xFFFFFFFFu);
        std::vector<float> viewPositions(size_t(kWidth) * kHeight * 4, 0.0f);
        std::vector<float> depths(size_t(kWidth) * kHeight, 0.0f);
        const float tanY = std::tan(60.0f * 0.5f * kPi / 180.0f);
        const float tanX = tanY * float(kWidth) / float(kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t index = size_t(y) * kWidth + x;
                const float ndcX = (float(x) + 0.5f) / float(kWidth) * 2.0f - 1.0f;
                const float ndcY = 1.0f - (float(y) + 0.5f) / float(kHeight) * 2.0f;
                viewPositions[index * 4 + 2] = -2.0f;
                viewPositions[index * 4 + 3] = 1.0f;
                viewPositions[index * 4 + 0] = ndcX * tanX * 2.0f;
                viewPositions[index * 4 + 1] = ndcY * tanY * 2.0f;
                depths[index] = 0.1f / 2.0f;
            }

        maskView = MakeView(kWidth, kHeight, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, maskPixels.data(), kWidth * 4);
        depthView = MakeView(kWidth, kHeight, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, depths.data(), kWidth * 4);
        // The screen-space mask's format at the cascade atlas' size (2048x10240 in the game, 2048x1024 here): the size test is what
        // refuses it, and the real atlas would also fail the format test (it is R8_UNORM).
        std::vector<uint32_t> atlasPixels(size_t(2048) * 1024, 0xFFFFFFFFu);
        atlasView = MakeView(2048, 1024, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, atlasPixels.data(), 2048 * 4);

        float lightBytes[8] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f, 0.6f, 0.0f };
        const contact_test::Mat4 projection = contact_test::Projection(0.1f, 1000.0f, 60.0f, float(kWidth) / float(kHeight));
        const contact_test::CameraRows rows = contact_test::MakeCamera(contact_test::Identity(), projection);
        light = MakeBuffer(lightBytes, sizeof lightBytes);
        camera = MakeBuffer(rows.v, sizeof rows.v);
        return maskView && depthView && atlasView && light && camera;
    }

    ComPtr<ID3D11ShaderResourceView> MakeView(uint32_t width, uint32_t height, DXGI_FORMAT format, DXGI_FORMAT viewFormat,
                                              const void* data, uint32_t rowPitch)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
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

    // Binds the draw's state: the three textures and the two constant buffers at the slots the layout names, plus the viewport.
    void Bind(const Layout& layout, ID3D11ShaderResourceView* maskOverride = nullptr)
    {
        ID3D11ShaderResourceView* views[16] {};
        views[layout.maskSlot] = maskOverride != nullptr ? maskOverride : maskView.Get();
        views[layout.depthSlot] = depthView.Get();
        context->PSSetShaderResources(0, 16, views);
        ID3D11Buffer* buffers[8] {};
        buffers[layout.lightParamSlot] = light.Get();
        buffers[layout.cameraParamSlot] = camera.Get();
        context->PSSetConstantBuffers(0, 8, buffers);
        D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
    }

    // The mask view the game bound right now.
    ID3D11ShaderResourceView* BoundMask(uint32_t slot)
    {
        ID3D11ShaderResourceView* view = nullptr;
        context->PSGetShaderResources(slot, 1, &view);
        if (view != nullptr)
            view->Release();
        return view;
    }

    void Clear()
    {
        ID3D11ShaderResourceView* none[16] {};
        context->PSSetShaderResources(0, 16, none);
        ID3D11Buffer* noBuffers[8] {};
        context->PSSetConstantBuffers(0, 8, noBuffers);
    }
};

Layout GoodLayout()
{
    Layout layout;
    layout.ok = true;
    layout.maskSlot = 6;
    layout.depthSlot = 7;
    layout.lightParamSlot = 2;
    layout.cameraParamSlot = 3;
    return layout;
}

Params GoodParams() { return BuildParams(0.5f, 0.5f, 12, SunSpace::World, SunSign::Toward); }
} // namespace

int main()
{
    Scene scene;
    CHECK(scene.Create());
    if (!scene.device)
        return Finish("contact_shadows_apply_warp");
    const Layout layout = GoodLayout();

    // ---- a draw the pass can run: the game sees the pass's copy, and its own binding is back afterwards
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout);
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        const int linesBefore = g_testLogLines;
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        CHECK(reason == Decline::None);
        CHECK(payload.ran);
        CHECK(payload.mask != nullptr);
        CHECK(payload.mask != scene.maskView.Get());      // the draw ran on the pass's copy
        CHECK(scene.BoundMask(layout.maskSlot) == scene.maskView.Get()); // and the game's own is back
        CHECK_EQ(state.runs, 1u);
        CHECK(g_testLogLines > linesBefore); // the classification line and the run line
        scene.Clear();
    }

    // ---- a draw that throws: the game's binding is back afterwards and the throw is reported, not propagated
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout);
        Payload payload { scene.context.Get(), layout.maskSlot, true };
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        CHECK(reason == Decline::Exception);
        CHECK(payload.threw);
        CHECK(scene.BoundMask(layout.maskSlot) == scene.maskView.Get());
        CHECK_EQ(state.runs, 0u);
        scene.Clear();
    }

    // ---- a draw the rules refuse (the cascade atlas at the mask slot): the game's own binding never moved
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout, scene.atlasView.Get());
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        CHECK(reason == Decline::MaskSize);
        CHECK(payload.ran);
        CHECK(payload.mask == scene.atlasView.Get()); // exactly what the game bound
        CHECK(scene.BoundMask(layout.maskSlot) == scene.atlasView.Get());
        CHECK_EQ(state.runs, 0u);
        CHECK_EQ(state.declined, 1u);
        scene.Clear();
    }

    // ---- a strength of zero runs nothing at all
    {
        State state;
        state.params = BuildParams(0.5f, 0.0f, 12, SunSpace::World, SunSign::Toward);
        scene.Bind(layout);
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        CHECK(reason == Decline::StrengthZero);
        CHECK(payload.mask == scene.maskView.Get());
        CHECK_EQ(state.runs, 0u);
        scene.Clear();
    }

    // ---- sixteen refusals in a row switch the module off for the session, with one line, and then every draw is passed through
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout);
        // A window that leaves the mask is the draw's own fault (a resolution the design has not seen), so it counts towards the switch-off;
        // a shape refusal (the next block) does not.
        D3D11_VIEWPORT outside { -100.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
        scene.context->RSSetViewports(1, &outside);
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        for (uint32_t i = 0; i < kDeclineLimit; ++i)
            CHECK(ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload) == Decline::WindowNotInMask);
        CHECK(state.session.off);
        CHECK(state.loggedOff);
        CHECK(!state.session.Armed());
        CHECK_EQ(state.declined, kDeclineLimit);
        const int afterSwitchOff = g_testLogLines;
        // a draw after the switch-off is the game's own, and says nothing more
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        CHECK(reason == Decline::None);
        CHECK(payload.mask != nullptr);
        CHECK_EQ(g_testLogLines, afterSwitchOff);
        scene.Clear();
    }

    // ---- a camera buffer the shader cannot read: the pass refuses before it draws, the game's own binding never moves and the draw runs
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout);
        const float empty[4] {};
        ComPtr<ID3D11Buffer> shortBuffer = scene.MakeBuffer(empty, sizeof empty);
        CHECK(shortBuffer != nullptr);
        ID3D11Buffer* buffers[8] {};
        buffers[layout.lightParamSlot] = scene.light.Get();
        buffers[layout.cameraParamSlot] = shortBuffer.Get();
        scene.context->PSSetConstantBuffers(0, 8, buffers);
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        std::printf("    a camera buffer of 16 bytes: %s, the draw saw its own mask: %s%c", DeclineName(reason),
                    payload.mask == scene.maskView.Get() ? "yes" : "no", 10);
        CHECK(reason == Decline::CameraLayout);
        CHECK(payload.ran);
        CHECK(payload.mask == scene.maskView.Get());
        CHECK(scene.BoundMask(layout.maskSlot) == scene.maskView.Get());
        CHECK_EQ(state.runs, 0u);
        scene.Clear();
    }

    // ---- a camera buffer that is not the layout the shaders read: refused once, the game's binding untouched, and it does not count
    // towards the switch-off (a loading camera must not end the feature for the session)
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout);
        float wrong[22 * 4] {};
        for (int i = 0; i < 22; ++i)
            wrong[i * 4 + i % 4] = 2.0f; // every matrix a scaled identity: the view basis is not orthonormal and no product is I
        ComPtr<ID3D11Buffer> bogus = scene.MakeBuffer(wrong, sizeof wrong);
        CHECK(bogus != nullptr);
        ID3D11Buffer* buffers[8] {};
        buffers[layout.lightParamSlot] = scene.light.Get();
        buffers[layout.cameraParamSlot] = bogus.Get();
        scene.context->PSSetConstantBuffers(0, 8, buffers);
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        std::printf("    a camera buffer of another layout: %s%c", DeclineName(reason), 10);
        CHECK(reason == Decline::CameraLayout);
        {
            CHECK(payload.mask == scene.maskView.Get());
            for (uint32_t i = 0; i < kDeclineLimit * 2; ++i)
                ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
            CHECK(!state.session.off); // a shape refusal never switches the module off
        }
        scene.Clear();
    }

    // ---- a sun vector that is not unit length: the member the engine reports moved, so the draw is left alone (the ray would be scaled)
    // and the game's own binding never moves
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout);
        float notUnit[8] { 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f };
        ComPtr<ID3D11Buffer> scaled = scene.MakeBuffer(notUnit, sizeof notUnit);
        CHECK(scaled != nullptr);
        ID3D11Buffer* buffers[8] {};
        buffers[layout.lightParamSlot] = scaled.Get();
        buffers[layout.cameraParamSlot] = scene.camera.Get();
        scene.context->PSSetConstantBuffers(0, 8, buffers);
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        const Decline reason = ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload);
        std::printf("    a sun vector of length 2: %s%c", DeclineName(reason), 10);
        CHECK(reason == Decline::SunNotUnit);
        CHECK(payload.ran);
        CHECK(scene.BoundMask(layout.maskSlot) == scene.maskView.Get());
        CHECK_EQ(state.runs, 0u);
        scene.Clear();
    }

    // ---- the pass cannot run on this device at all (a feature level without the shaders): the game's own binding still comes back
    // exactly as it was, so a device the pass does not understand fails open like every other refusal
    {
        ComPtr<ID3D11Device> lowDevice;
        ComPtr<ID3D11DeviceContext> lowContext;
        const D3D_FEATURE_LEVEL levels[] { D3D_FEATURE_LEVEL_9_3 };
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &lowDevice, nullptr,
                                             &lowContext);
        CHECK(SUCCEEDED(hr) && lowDevice != nullptr && lowContext != nullptr);
        if (lowDevice)
        {
            std::vector<uint32_t> maskPixels(size_t(kWidth) * kHeight, 0xFFFFFFFFu);
            std::vector<float> depthPixels(size_t(kWidth) * kHeight, 0.5f);
            const auto makeView = [&lowDevice](DXGI_FORMAT format, const void* data, uint32_t rowPitch) {
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
                if (FAILED(lowDevice->CreateTexture2D(&desc, &initial, &texture)))
                    return ComPtr<ID3D11ShaderResourceView> {};
                D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
                srv.Format = format;
                srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = 1;
                ComPtr<ID3D11ShaderResourceView> view;
                if (FAILED(lowDevice->CreateShaderResourceView(texture.Get(), &srv, &view)))
                    return ComPtr<ID3D11ShaderResourceView> {};
                return view;
            };
            const auto makeBuffer = [&lowDevice](const void* data, uint32_t bytes) {
                D3D11_BUFFER_DESC desc {};
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.ByteWidth = bytes;
                D3D11_SUBRESOURCE_DATA initial { data, 0, 0 };
                ComPtr<ID3D11Buffer> buffer;
                if (FAILED(lowDevice->CreateBuffer(&desc, &initial, &buffer)))
                    return ComPtr<ID3D11Buffer> {};
                return buffer;
            };
            const ComPtr<ID3D11ShaderResourceView> lowMask = makeView(DXGI_FORMAT_B8G8R8A8_UNORM, maskPixels.data(), kWidth * 4);
            const ComPtr<ID3D11ShaderResourceView> lowDepth = makeView(DXGI_FORMAT_B8G8R8A8_UNORM, maskPixels.data(), kWidth * 4);
            float lightBytes[8] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f, 0.6f, 0.0f };
            const contact_test::Mat4 projection = contact_test::Projection(0.1f, 1000.0f, 60.0f, float(kWidth) / float(kHeight));
            const contact_test::CameraRows rows = contact_test::MakeCamera(contact_test::Identity(), projection);
            const ComPtr<ID3D11Buffer> lowLight = makeBuffer(lightBytes, sizeof lightBytes);
            const ComPtr<ID3D11Buffer> lowCamera = makeBuffer(rows.v, sizeof rows.v);
            CHECK(lowMask && lowDepth && lowLight && lowCamera);
            if (lowMask && lowDepth && lowLight && lowCamera)
            {
                ID3D11ShaderResourceView* views[16] {};
                views[layout.maskSlot] = lowMask.Get();
                views[layout.depthSlot] = lowDepth.Get();
                lowContext->PSSetShaderResources(0, 16, views);
                ID3D11Buffer* buffers[8] {};
                buffers[layout.lightParamSlot] = lowLight.Get();
                buffers[layout.cameraParamSlot] = lowCamera.Get();
                lowContext->PSSetConstantBuffers(0, 8, buffers);
                D3D11_VIEWPORT viewport { 0.0f, 0.0f, float(kWidth), float(kHeight), 0.0f, 1.0f };
                lowContext->RSSetViewports(1, &viewport);

                State state;
                state.params = GoodParams();
                state.haveSun = true; // the session already read the sun: this case is about the pass failing, not the camera read
                Payload payload { lowContext.Get(), layout.maskSlot, false };
                const Decline reason = ApplyDraw(lowContext.Get(), state, &layout, &Draw, &payload);
                std::printf("    a device without the pass's shaders: %s, the draw saw its own mask: %s%c", DeclineName(reason),
                            payload.mask == lowMask.Get() ? "yes" : "no", 10);
                CHECK(reason == Decline::PassFailed);
                CHECK(payload.ran);
                CHECK(payload.mask == lowMask.Get());
                CHECK_EQ(state.runs, 0u);
            }
        }
    }

    // ---- a draw of another shape (the cascade atlas at the mask slot) is refused, logged once and never switches the module off: the atlas
    // writer declares the same four names as the sun light draw, so this happens on every shadow pass of a session and the sun light draw has
    // to keep running after it
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout, scene.atlasView.Get());
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        const int linesBefore = g_testLogLines;
        for (uint32_t i = 0; i < 50; ++i)
            CHECK(ApplyDraw(scene.context.Get(), state, &layout, &Draw, &payload) == Decline::MaskSize);
        CHECK(!state.session.off);
        CHECK(state.session.Armed());
        CHECK_EQ(state.declined, 50u);
        CHECK_EQ(g_testLogLines - linesBefore, 1); // one line for the reason, no switch-off line
        // the sun light draw still runs, and the game sees the pass's copy for it
        scene.Bind(layout);
        Payload sun { scene.context.Get(), layout.maskSlot, false };
        CHECK(ApplyDraw(scene.context.Get(), state, &layout, &Draw, &sun) == Decline::None);
        CHECK_EQ(state.runs, 1u);
        CHECK(sun.mask != nullptr && sun.mask != scene.maskView.Get()); // the draw saw the pass's copy
        CHECK(scene.BoundMask(layout.maskSlot) == scene.maskView.Get()); // and the game's own binding is back afterwards
        scene.Clear();
    }

    // ---- the log budget: reasons that keep changing still cannot write more than the budget
    {
        State state;
        state.params = GoodParams();
        const int before = g_testLogLines;
        // Two layouts that fail differently (no depth, and the atlas at the mask), alternated so every refusal is a change of state.
        Layout noDepth = layout;
        noDepth.depthSlot = 12; // nothing is bound there
        for (unsigned i = 0; i < 100; ++i)
        {
            const Layout& current = (i % 2) == 0 ? noDepth : layout;
            scene.Bind(layout, current.depthSlot == layout.depthSlot ? scene.atlasView.Get() : nullptr);
            Payload payload { scene.context.Get(), layout.maskSlot, false };
            ApplyDraw(scene.context.Get(), state, &current, &Draw, &payload);
        }
        CHECK(g_testLogLines - before <= int(kMaxLogLines));
        scene.Clear();
    }

    // ---- a null layout (a shader the index never claimed) is a pass-through and not a decline: a frame of ordinary draws must not count
    // towards the switch-off, so more such calls than the limit leave the module armed
    {
        State state;
        state.params = GoodParams();
        scene.Bind(layout);
        Payload payload { scene.context.Get(), layout.maskSlot, false };
        for (uint32_t i = 0; i < kDeclineLimit * 4; ++i)
            CHECK(ApplyDraw(scene.context.Get(), state, nullptr, &Draw, &payload) == Decline::None);
        CHECK_EQ(state.declined, 0u);
        CHECK(!state.session.off);
        CHECK(state.session.Armed());
        CHECK(payload.mask == scene.maskView.Get());
        scene.Clear();
    }

    return Finish("contact_shadows_apply_warp");
}
