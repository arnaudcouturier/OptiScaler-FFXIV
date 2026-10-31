#include "pch.h"

#include "ext/ffxiv/relight/Inject.h"

#include "ext/ffxiv/diag/ShaderBindings.h" // the DXBC RDEF reader the developer draw diagnostic already uses
#include "ext/ffxiv/relight/shaders/CameraRegisters.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>

// Contact shadows: the device side of the injection (ext/ffxiv/relight/Inject.h). Everything here works on the game's own context with the
// draw's own resources: it reads the bound state, decides with the pure rules of ContactShadowsRules.h, runs the pass, binds its copy for
// exactly one draw and puts the game's own binding back, also when the draw throws. The reflection *index* and the config side live in
// Inject.cpp, so a test builds this file and the pass on a software adapter without the game module.
namespace ext::ffxiv::relight
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr uint32_t kCbSlots = 8;       // the constant-buffer slots a draw is inspected at
constexpr uint32_t kTextureSlots = 16; // the texture slots a draw is inspected at
constexpr uint32_t kSunBytes = 0x20;   // g_LightParam +0x00..0x20: position, direction and colour
int64_t Qpc()
{
    LARGE_INTEGER value {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int64_t QpcFrequency()
{
    LARGE_INTEGER value {};
    QueryPerformanceFrequency(&value);
    return value.QuadPart > 0 ? value.QuadPart : 1;
}

double SecondsBetween(int64_t from, int64_t to) { return from <= 0 ? 0.0 : double(to - from) / double(QpcFrequency()); }

// One bound texture of the draw, as the classification needs it.
struct BoundTexture
{
    ComPtr<ID3D11ShaderResourceView> view;
    uint32_t format = 0, width = 0, height = 0, samples = 0;
    bool twoD = false;
};

BoundTexture TextureAt(ID3D11ShaderResourceView* view)
{
    BoundTexture out;
    if (view == nullptr)
        return out;
    out.view = view;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    view->GetDesc(&desc);
    out.format = uint32_t(desc.Format);
    out.samples = 1;
    out.twoD = desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!resource)
        return out;
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        out.twoD = false;
        return out;
    }
    D3D11_TEXTURE2D_DESC texture {};
    static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&texture);
    out.width = texture.Width;
    out.height = texture.Height;
    out.samples = texture.SampleDesc.Count;
    out.twoD = out.twoD && texture.ArraySize == 1;
    return out;
}

// Reads the sun vector and the camera's three matrices once, at the first sun light draw of the session: the classification line the design
// asks for, the unit-length guard, and the one-time check that g_CameraParameter really is the layout the shaders read (a game patch that
// moves a matrix fails here instead of shading with nonsense). After that the direction and the matrices are handled on the GPU only
// (copied into the pass's own buffers, never mapped).
bool ReadLightAndCameraOnce(ID3D11DeviceContext* context, State& state, ID3D11Buffer* light, ID3D11Buffer* camera, float outSun[3],
                            float outCamera[kCameraCheckFloats])
{
    if (light == nullptr || camera == nullptr)
        return false;
    D3D11_BUFFER_DESC lightDesc {};
    light->GetDesc(&lightDesc);
    D3D11_BUFFER_DESC cameraDesc {};
    camera->GetDesc(&cameraDesc);
    if (lightDesc.ByteWidth < kSunBytes || cameraDesc.ByteWidth < CS_CAMERA_CHECK_BYTES)
        return false;
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device)
        return false;
    const auto stagingFor = [context, &device](ComPtr<ID3D11Buffer>& target, const D3D11_BUFFER_DESC& source) {
        if (target)
            return true;
        D3D11_BUFFER_DESC staging {};
        staging.ByteWidth = source.ByteWidth;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> created;
        if (FAILED(device->CreateBuffer(&staging, nullptr, &created)) || !created)
            return false;
        target = created;
        return true;
    };
    if (!stagingFor(state.readback, lightDesc) || !stagingFor(state.cameraReadback, cameraDesc))
        return false;

    D3D11_BOX lightBox {};
    lightBox.right = kSunBytes;
    lightBox.bottom = 1;
    lightBox.back = 1;
    D3D11_BOX cameraBox {};
    cameraBox.right = CS_CAMERA_CHECK_BYTES;
    cameraBox.bottom = 1;
    cameraBox.back = 1;
    context->CopySubresourceRegion(state.readback.Get(), 0, 0, 0, 0, light, 0, &lightBox);
    context->CopySubresourceRegion(state.cameraReadback.Get(), 0, 0, 0, 0, camera, 0, &cameraBox);

    D3D11_MAPPED_SUBRESOURCE mappedLight {};
    if (FAILED(context->Map(state.readback.Get(), 0, D3D11_MAP_READ, 0, &mappedLight)) || mappedLight.pData == nullptr)
        return false;
    std::memcpy(outSun, static_cast<const unsigned char*>(mappedLight.pData) + 0x10, sizeof(float) * 3);
    context->Unmap(state.readback.Get(), 0);

    D3D11_MAPPED_SUBRESOURCE mappedCamera {};
    if (FAILED(context->Map(state.cameraReadback.Get(), 0, D3D11_MAP_READ, 0, &mappedCamera)) || mappedCamera.pData == nullptr)
        return false;
    std::memcpy(outCamera, mappedCamera.pData, sizeof(float) * kCameraCheckFloats);
    context->Unmap(state.cameraReadback.Get(), 0);
    return true;
}

// Fills what the pure classification needs and the resources the pass reads. Changes none of the context's state.
void Inspect(ID3D11DeviceContext* context, const Layout& layout, Inputs& inputs, DrawFacts& facts)
{
    ID3D11ShaderResourceView* views[kTextureSlots] {};
    context->PSGetShaderResources(0, kTextureSlots, views);
    BoundTexture mask, depth;
    if (layout.maskSlot < kTextureSlots)
        mask = TextureAt(views[layout.maskSlot]);
    if (layout.depthSlot < kTextureSlots)
        depth = TextureAt(views[layout.depthSlot]);
    for (auto*& view : views)
        if (view != nullptr)
        {
            view->Release();
            view = nullptr;
        }

    ID3D11Buffer* buffers[kCbSlots] {};
    context->PSGetConstantBuffers(0, kCbSlots, buffers);
    ComPtr<ID3D11Buffer> light, camera;
    if (layout.lightParamSlot < kCbSlots && buffers[layout.lightParamSlot] != nullptr)
        light.Attach(buffers[layout.lightParamSlot]);
    if (layout.cameraParamSlot < kCbSlots && buffers[layout.cameraParamSlot] != nullptr)
        camera.Attach(buffers[layout.cameraParamSlot]);
    for (size_t i = 0; i < kCbSlots; ++i)
        if (buffers[i] != nullptr && buffers[i] != light.Get() && buffers[i] != camera.Get())
            buffers[i]->Release();

    facts.hasMask = mask.view != nullptr;
    facts.hasDepth = depth.view != nullptr;
    facts.hasLightParam = light != nullptr;
    facts.hasCameraParam = camera != nullptr;
    facts.maskFormat = mask.format;
    facts.maskWidth = mask.width;
    facts.maskHeight = mask.height;
    facts.maskSamples = mask.samples;
    facts.mask2D = mask.twoD;
    facts.depthWidth = depth.width;
    facts.depthHeight = depth.height;
    // The display allocation's resolution is the draw's own full-resolution depth binding: the screen-space mask is that size.
    facts.displayWidth = depth.width;
    facts.displayHeight = depth.height;

    D3D11_VIEWPORT viewport {};
    UINT count = 1;
    context->RSGetViewports(&count, &viewport);
    facts.window = { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height };

    inputs.mask = mask.view.Get();
    inputs.depth = depth.view.Get();
    inputs.lightParam = light.Get();
    inputs.cameraParam = camera.Get();
    inputs.maskFormat = mask.format;
    inputs.maskWidth = mask.width;
    inputs.maskHeight = mask.height;
    inputs.window = facts.window;
}
} // namespace

Decline ApplyDraw(ID3D11DeviceContext* context, State& state, const Layout* layout, DrawFn draw, void* user)
{
    if (draw == nullptr)
        return Decline::Exception;
    if (context == nullptr || layout == nullptr || !layout->ok || !state.session.Armed())
    {
        draw(user); // not a decline: a draw the index never claimed must not count towards the switch-off
        return Decline::None;
    }

    // One refused light draw: one log line when the reason changes, and the counter that switches the module off for the session. A
    // refusal about the draw's shape (the atlas writer, a mask of another format) is not a build mismatch and does not feed that counter:
    // it is logged, and the draw path skips that shader from then on. The game's draw runs exactly once per call: after it threw, it is
    // not called a second time here (that would run it twice and let the second throw escape).
    bool drew = false;
    const auto decline = [&](Decline reason) {
        state.declined++;
        if (state.log.Take(uint32_t(reason)))
            LOG_INFO("{}", FormatDecline(reason));
        if (!IsShapeDecline(reason) && state.session.NoteDecline() && !state.loggedOff)
        {
            state.loggedOff = true;
            LOG_WARN("{}", FormatSwitchedOff());
        }
        if (!drew)
            draw(user);
        return reason;
    };

    if (!(state.params.strength > 0.0f))
        return decline(Decline::StrengthZero);

    try
    {
        Inputs inputs;
        DrawFacts facts;
        Inspect(context, *layout, inputs, facts);
        const Decline reason = Classify(facts);
        if (reason != Decline::None)
            return decline(reason);

        // The sun vector and the camera layout, once per session (and again after a few seconds if the first light draw still had a loading
        // camera): the unit-length guard, the layout check and the classification line. The GPU normalises every frame anyway, so a later
        // change of the value can only cost a term that does nothing, never a wrong shadow.
        if (!state.haveSun)
        {
            // The pass does not run on a camera buffer that has not been checked: after a refusal the check waits
            // (ContactShadowsRules.h's kCameraCheckSeconds) and until then every light draw is left alone.
            if (state.lastCameraCheckQpc != 0 && SecondsBetween(state.lastCameraCheckQpc, Qpc()) <= kCameraCheckSeconds)
                return decline(Decline::CameraLayout);
            state.lastCameraCheckQpc = Qpc();
            float sun[3] {};
            float cameraRows[kCameraCheckFloats] {};
            D3D11_BUFFER_DESC cameraDesc {};
            inputs.cameraParam->GetDesc(&cameraDesc);
            if (cameraDesc.ByteWidth < CS_CAMERA_CHECK_BYTES)
                return decline(Decline::CameraLayout); // too small to hold the matrices the check reads
            if (!ReadLightAndCameraOnce(context, state, inputs.lightParam, inputs.cameraParam, sun, cameraRows))
                return decline(Decline::NoLightParam);
            if (!IsUnitDirection(sun[0], sun[1], sun[2]))
                return decline(Decline::SunNotUnit);
            if (!CameraLayoutOk(cameraRows, kCameraCheckFloats))
                return decline(Decline::CameraLayout);
            state.haveSun = true;
            state.lastSun[0] = sun[0];
            state.lastSun[1] = sun[1];
            state.lastSun[2] = sun[2];
            state.lastSunQpc = Qpc();
            if (state.log.Take(0x40000000u))
                LOG_INFO("{}", FormatClassification(facts.maskFormat, facts.maskWidth, facts.maskHeight, sun[0], sun[1], sun[2],
                                                    state.params.space));
        }

        inputs.params = state.params;
        // The pass runs in its own device-context state and sets its own shaders there; the game's binding comes back with the state swap,
        // not with another PSSetShader, so the pixel-shader tracker must not overwrite what it learned from the game while this runs.
        struct TrackGuard
        {
            State& state;
            const bool previous;
            TrackGuard(State& state) : state(state), previous(state.inPass.exchange(true, std::memory_order_relaxed)) {}
            ~TrackGuard() { state.inPass.store(previous, std::memory_order_relaxed); } // nested guards restore, never clear
        } trackGuard { state };
        ID3D11ShaderResourceView* copy = state.pass.Run(context, inputs);
        if (copy == nullptr)
            return decline(state.pass.Failure() != nullptr ? Decline::PassFailed : Decline::TextureFailed);

        // Bind the copy for exactly one draw and put the game's own binding back afterwards, also when the draw throws.
        ID3D11ShaderResourceView* original = nullptr;
        context->PSGetShaderResources(layout->maskSlot, 1, &original);
        context->PSSetShaderResources(layout->maskSlot, 1, &copy);
        struct Restore
        {
            ID3D11DeviceContext* context;
            uint32_t slot;
            ID3D11ShaderResourceView* view;
            ~Restore()
            {
                context->PSSetShaderResources(slot, 1, &view);
                if (view != nullptr)
                    view->Release();
            }
        } restore { context, layout->maskSlot, original };

        drew = true;
        draw(user);

        state.runs++;
        state.session.NoteRun();
        // The developer check's one-frame dump: the key holds a delay, and the first light draw that runs after it is written out whole.
        if (DumpDue(state.dumpDelaySeconds, state.dumpWritten, state.dumpStartQpc, Qpc(), QpcFrequency()))
            DumpFrame(context, state, inputs, state.pass);
        if (state.log.Take(0x80000000u | ((facts.maskWidth & 0x7FFFu) << 11) | (facts.maskHeight & 0x7FFu)))
            LOG_INFO("{}", FormatRun(state.params, facts.maskWidth, facts.maskHeight, facts.window.width, facts.window.height,
                                             state.pass.HasGpuMilliseconds(), state.pass.GpuMilliseconds()));
        return Decline::None;
    }
    catch (...)
    {
        return decline(Decline::Exception);
    }
}
} // namespace ext::ffxiv::relight
