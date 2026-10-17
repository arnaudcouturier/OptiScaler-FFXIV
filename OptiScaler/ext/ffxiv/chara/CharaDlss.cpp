#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaCamera.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaDlssGate.h"
#include "ext/ffxiv/chara/CharaDlssNgx.h"
#include "ext/ffxiv/chara/CharaDlssRules.h"
#include "ext/ffxiv/chara/CharaGuides.h"
#include "ext/ffxiv/chara/CharaJitter.h"
#include "ext/ffxiv/chara/CharaMotion.h"
#include "ext/ffxiv/chara/CharaSlots.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTable.h"
#include "ext/ffxiv/chara/CharaTags.h"

#include <Config.h>
#include <State.h>
#include <proxies/NVNGX_Proxy.h>

#include <CharaGuides_Alpha_Shader.h>
#include <CharaGuides_Copy_Shader.h>
#include <CharaGuides_Guides_Shader.h>
#include <CharaGuides_Vs_Shader.h>

#include <array>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string_view>

// The isolated DLSS path for character previews: entry points behind CharaDlssApi.h.
//
// Per preview render: the depth prepass and the G-buffer draws run with the viewport shifted by the pass jitter; the
// G-buffer draws are also replayed into a signed-motion target; the final composition draw (the "finisher") produces the
// native image, which is then copied into the colour input, turned into depth and motion guides, upscaled by a private NGX
// feature (render size W x H, output 2W x 2H) and recomposed with the native alpha. UI draws that sample the native image
// get the composed image instead. Every failure keeps the native image.
//
// Reconstructed from the behaviour of Nickle's depth-jitter-20261013 build (no source available), see
// docs/RECONSTRUCTION-NICKLE.md.
namespace ext::ffxiv::chara::dlss
{
using Microsoft::WRL::ComPtr;

// The parameter names and flag values used by CharaDlssNgx.h are the SDK's.
static_assert(std::string_view(NVSDK_NGX_Parameter_Width) == ngx::kWidth);
static_assert(std::string_view(NVSDK_NGX_Parameter_Height) == ngx::kHeight);
static_assert(std::string_view(NVSDK_NGX_Parameter_OutWidth) == ngx::kOutWidth);
static_assert(std::string_view(NVSDK_NGX_Parameter_OutHeight) == ngx::kOutHeight);
static_assert(std::string_view(NVSDK_NGX_Parameter_PerfQualityValue) == ngx::kPerfQuality);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) == ngx::kCreateFlags);
static_assert(std::string_view(NVSDK_NGX_Parameter_Color) == ngx::kColor);
static_assert(std::string_view(NVSDK_NGX_Parameter_Output) == ngx::kOutput);
static_assert(std::string_view(NVSDK_NGX_Parameter_Depth) == ngx::kDepth);
static_assert(std::string_view(NVSDK_NGX_Parameter_MotionVectors) == ngx::kMotionVectors);
static_assert(std::string_view(NVSDK_NGX_Parameter_Jitter_Offset_X) == ngx::kJitterX);
static_assert(std::string_view(NVSDK_NGX_Parameter_Jitter_Offset_Y) == ngx::kJitterY);
static_assert(std::string_view(NVSDK_NGX_Parameter_MV_Scale_X) == ngx::kMvScaleX);
static_assert(std::string_view(NVSDK_NGX_Parameter_MV_Scale_Y) == ngx::kMvScaleY);
static_assert(std::string_view(NVSDK_NGX_Parameter_Sharpness) == ngx::kSharpness);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Pre_Exposure) == ngx::kPreExposure);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Exposure_Scale) == ngx::kExposureScale);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == ngx::kSubrectWidth);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height) == ngx::kSubrectHeight);
static_assert(std::string_view(NVSDK_NGX_Parameter_Reset) == ngx::kReset);
static_assert(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes == ngx::kFlagMotionLowRes);
static_assert(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted == ngx::kFlagDepthInverted);
static_assert(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure == ngx::kFlagAutoExposure);
static_assert(NVSDK_NGX_PerfQuality_Value_MaxPerf == ngx::kPerfQualityMaxPerf);

namespace
{
constexpr uint64_t kGateRefreshMs = 100;
constexpr uint64_t kEvaluationLogEvery = 300;
constexpr uint32_t kMaxSlotFailures = 4; // this many failed slots end the path for the session
constexpr UINT kSrvSlots = 8;

// ---- Gate state (any thread) -------------------------------------------------------------------------------------------
std::atomic<bool> g_latched { false };
std::atomic<uint32_t> g_slotsHeld { 0 }; // slots whose GPU objects still exist (a latched path drains them before it stops)
std::atomic<uint8_t> g_availability { static_cast<uint8_t>(Availability::ConfigOff) };
std::atomic<uint64_t> g_nextGateCheck { 0 };
std::atomic<bool> g_wasActive { false };

std::atomic<uint8_t> g_deviceState { 0 }; // 0 not compared yet, 1 same device, 2 different device
std::atomic<ID3D11Device*> g_checkedNgxDevice { nullptr };

std::mutex g_logMutex;
ReasonLog g_reasonLog;

std::atomic<uint32_t> g_activeSlots { 0 };
std::atomic<uint64_t> g_evaluations { 0 };
std::atomic<uint64_t> g_fallbacks { 0 };
std::atomic<uint64_t> g_copyChains { 0 };
std::atomic<uint32_t> g_slotFailures { 0 };

// ---- Draw-thread state --------------------------------------------------------------------------------------------------
uint64_t g_nextDeviceCheck = 0;

struct Image
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};

struct SlotGpu
{
    ComPtr<ID3D11ShaderResourceView> nativeSrv; // the finisher output (its own SRV; keeps the texture alive)
    Image color, depth, motion, output, composed;
    ComPtr<ID3D11Query> query;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* handle = nullptr;
    bool failureLogged = false;
};

struct PassData
{
    uint32_t counter = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool live = false;        // a live slot exists: the pass is jittered, replayed and finished
    bool prepassSeen = false;
    bool replayed = false;
    bool cameraCaptured = false;
    bool jitterZeroed = false;
    Jitter jitter {};
};

SlotTable g_slots;
std::array<SlotGpu, kMaxSlots> g_gpu;
PassData g_pass;
ComPtr<ID3D11Device> g_device;
ID3D11DeviceContext* g_context = nullptr; // the immediate context (valid until Detach)
ComPtr<IUnknown> g_contextDeviceRef;
guides::Passes g_passes;
bool g_passesTried = false;
ComPtr<ID3D11Texture2D> g_depthTexture;
ComPtr<ID3D11ShaderResourceView> g_depthSrv;
bool g_reversedZ = false;
bool g_loggedNoPrepass = false;
std::array<bool, 4> g_waitLogged {};

// ---- Gate ----------------------------------------------------------------------------------------------------------------
bool EntryPointsPresent()
{
    return NVNGXProxy::D3D11_AllocateParameters() != nullptr && NVNGXProxy::D3D11_DestroyParameters() != nullptr &&
           NVNGXProxy::D3D11_CreateFeature() != nullptr && NVNGXProxy::D3D11_EvaluateFeature() != nullptr &&
           NVNGXProxy::D3D11_ReleaseFeature() != nullptr;
}

GateFacts GatherFacts()
{
    GateFacts f;
    f.configOn = ext::Cfg().ffxiv.chara.previewDlss.value_or_default();
    f.latched = g_latched.load(std::memory_order_relaxed);
    f.ngxModuleLoaded = NVNGXProxy::NVNGXModule() != nullptr;
    f.dlssEnabledInConfig = Config::Instance()->DLSSEnabled.value_or_default();
    f.dx11Initialised = NVNGXProxy::IsDx11Inited();
    f.entryPoints = EntryPointsPresent();
    const uint8_t device = g_deviceState.load(std::memory_order_relaxed);
    f.deviceChecked = device != 0;
    f.deviceMatches = device == 1;
    return f;
}

void ReportAvailability(Availability a)
{
    std::lock_guard lock(g_logMutex);
    if (g_reasonLog.ShouldLog(a))
        LOG_INFO("CharaView DLSS unavailable: {}", ReasonText(a));
}

Availability Refresh(uint64_t now)
{
    const Availability a = Evaluate(GatherFacts());
    g_availability.store(static_cast<uint8_t>(a), std::memory_order_relaxed);
    g_nextGateCheck.store(now + kGateRefreshMs, std::memory_order_relaxed);
    ReportAvailability(a);
    return a;
}

// Compares the immediate context's device with the device NGX was initialised on (draw thread, at most once a second).
void CheckDevice(ID3D11DeviceContext* context, uint64_t now)
{
    if (now < g_nextDeviceCheck && g_deviceState.load(std::memory_order_relaxed) != 0)
        return;
    g_nextDeviceCheck = now + 1000;

    ID3D11Device* ngxDevice = State::Instance().currentD3D11Device;
    if (ngxDevice == nullptr || context == nullptr)
        return; // NGX has not seen a device yet: the proxy gate reports that

    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    ComPtr<IUnknown> a, b;
    if (contextDevice == nullptr || FAILED(contextDevice.As(&a)) || FAILED(ngxDevice->QueryInterface(IID_PPV_ARGS(&b))))
        return;

    g_contextDeviceRef = a;
    g_checkedNgxDevice.store(ngxDevice, std::memory_order_relaxed);
    g_deviceState.store(a.Get() == b.Get() ? 1 : 2, std::memory_order_relaxed);
}

void Latch(const char* why)
{
    if (g_latched.exchange(true))
        return;
    LOG_WARN("CharaView DLSS: {}; native previews retained for this session", why);
}

void UpdateCounts()
{
    uint32_t held = 0;
    for (size_t i = 0; i < kMaxSlots; ++i)
        held += g_slots.Slot(static_cast<int>(i)).Used() ? 1 : 0;
    g_slotsHeld.store(held, std::memory_order_relaxed);
    g_activeSlots.store(static_cast<uint32_t>(g_slots.ActiveFeatures()), std::memory_order_relaxed);
}

void CountFallback() { g_fallbacks.fetch_add(1, std::memory_order_relaxed); }

// ---- GPU objects ---------------------------------------------------------------------------------------------------------
bool CreateImage(ID3D11Device* device, DXGI_FORMAT format, uint32_t w, uint32_t h, Image& out)
{
    D3D11_TEXTURE2D_DESC td {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &out.texture)) &&
           SUCCEEDED(device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.srv)) &&
           SUCCEEDED(device->CreateRenderTargetView(out.texture.Get(), nullptr, &out.rtv));
}

void ReleaseSlotGpu(int index)
{
    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    // NGX shutdown can flip the proxy state under us: only call into the driver while it is initialised.
    const bool ngxUsable = !State::Instance().isShuttingDown && NVNGXProxy::IsDx11Inited();
    if (g.handle != nullptr && ngxUsable)
    {
        if (auto release = NVNGXProxy::D3D11_ReleaseFeature(); release != nullptr)
            release(g.handle);
    }
    if (g.params != nullptr && ngxUsable)
    {
        if (auto destroy = NVNGXProxy::D3D11_DestroyParameters(); destroy != nullptr)
            destroy(g.params);
    }
    g = SlotGpu {};
    g_slots.Slot(index).hasFeature = false;
}

bool CreateSlotImages(int index, ID3D11Resource* nativeTexture, uint32_t w, uint32_t h)
{
    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    ID3D11Device* device = g_device.Get();
    D3D11_QUERY_DESC qd {};
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(device->CreateShaderResourceView(nativeTexture, nullptr, &g.nativeSrv)) ||
        !CreateImage(device, DXGI_FORMAT_R8G8B8A8_UNORM, w, h, g.color) || !CreateImage(device, DXGI_FORMAT_R32_FLOAT, w, h, g.depth) ||
        !CreateImage(device, DXGI_FORMAT_R16G16_FLOAT, w, h, g.motion) ||
        !CreateImage(device, DXGI_FORMAT_R16G16B16A16_FLOAT, w * ngx::kOutputScale, h * ngx::kOutputScale, g.output) ||
        !CreateImage(device, DXGI_FORMAT_R8G8B8A8_UNORM, w * ngx::kOutputScale, h * ngx::kOutputScale, g.composed) ||
        FAILED(device->CreateQuery(&qd, &g.query)))
    {
        ReleaseSlotGpu(index);
        return false;
    }
    return true;
}

struct ParamSetter
{
    NVSDK_NGX_Parameter* p;
    void SetUInt(const char* name, uint32_t v) { p->Set(name, static_cast<unsigned int>(v)); }
    void SetInt(const char* name, int v) { p->Set(name, v); }
    void SetFloat(const char* name, float v) { p->Set(name, v); }
    void SetResource(const char* name, ID3D11Resource* r) { p->Set(name, r); }
};

// Creates the NGX feature of a slot. A failure here ends the path for the session (the driver cannot do it).
bool CreateFeature(int index, ID3D11DeviceContext* context, uint32_t w, uint32_t h, bool reversedZ)
{
    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    auto allocate = NVNGXProxy::D3D11_AllocateParameters();
    auto create = NVNGXProxy::D3D11_CreateFeature();
    if (allocate == nullptr || create == nullptr || !NVNGXProxy::IsDx11Inited())
    {
        Latch("D3D11 NGX is no longer available");
        return false;
    }

    NVSDK_NGX_Parameter* params = nullptr;
    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        Latch("NGX parameter allocation failed");
        return false;
    }
    ParamSetter setter { params };
    ngx::ApplyCreate(setter, w, h, reversedZ);

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result result = create(context, NVSDK_NGX_Feature_SuperSampling, params, &handle);
    if (result != NVSDK_NGX_Result_Success || handle == nullptr)
    {
        if (auto destroy = NVNGXProxy::D3D11_DestroyParameters(); destroy != nullptr)
            destroy(params);
        char text[96];
        std::snprintf(text, sizeof(text), "feature creation failed (0x%X)", static_cast<unsigned>(result));
        Latch(text);
        return false;
    }
    g.params = params;
    g.handle = handle;
    g_slots.Slot(index).hasFeature = true;
    LOG_INFO("CreateFeature CharaView DLSS: isolated feature {}x{} -> {}x{}, native alpha retained", w, h, w * ngx::kOutputScale,
             h * ngx::kOutputScale);
    UpdateCounts();
    return true;
}

void FailSlot(int index, const char* why)
{
    g_slots.MarkFailed(index);
    CountFallback();
    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    if (!g.failureLogged)
    {
        g.failureLogged = true;
        LOG_WARN("CharaView DLSS: {}; native preview retained", why);
    }
    if (g_slotFailures.fetch_add(1) + 1 >= kMaxSlotFailures)
        Latch("several previews failed");
}

// ---- Pass bookkeeping ----------------------------------------------------------------------------------------------------
void BeginPass(const DrawView& view)
{
    g_pass = PassData {};
    g_pass.counter = view.pass != nullptr ? view.pass->counter : 0;
    g_pass.width = view.facts.targetWidth;
    g_pass.height = view.facts.targetHeight;
    g_pass.live = g_slots.JitterLive(view.now);
    if (g_pass.live)
    {
        if (g_slots.JitterApplies(view.now))
            g_pass.jitter = PassJitter(g_pass.counter);
        // The signed-motion target is ensured and cleared at the start of every pass.
        if (!motion::EnsureTarget(g_device.Get(), g_pass.width, g_pass.height))
            g_pass.live = false;
        else
            motion::ClearTarget(view.context);
    }
}

// The viewport shift of the jitter: viewport 0 only, restored when the scope ends.
class JitterScope
{
  public:
    JitterScope(ID3D11DeviceContext* context, Jitter jitter) : _context(context)
    {
        _count = 0;
        context->RSGetViewports(&_count, nullptr);
        if (_count == 0 || _count > _viewports.size())
        {
            _count = 0;
            return;
        }
        context->RSGetViewports(&_count, _viewports.data());
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> shifted = _viewports;
        const ext::ffxiv::chara::ViewportShift s = ShiftViewport(shifted[0].TopLeftX, shifted[0].TopLeftY, jitter);
        shifted[0].TopLeftX = s.topLeftX;
        shifted[0].TopLeftY = s.topLeftY;
        context->RSSetViewports(_count, shifted.data());
    }
    ~JitterScope()
    {
        if (_count != 0)
            _context->RSSetViewports(_count, _viewports.data());
    }
    JitterScope(const JitterScope&) = delete;
    JitterScope& operator=(const JitterScope&) = delete;

  private:
    ID3D11DeviceContext* _context;
    UINT _count = 0;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> _viewports {};
};

// The depth texture of the pass, its shader resource view and the depth convention are taken from the first G-buffer draw.
void CaptureGuideInputs(const DrawView& view)
{
    ID3D11DeviceContext* context = view.context;
    if (camera::Capture(context) == camera::Result::Captured)
        g_pass.cameraCaptured = true;

    g_reversedZ = motion::BoundDepthIsReversed(context);

    ID3D11Texture2D* depth = view.dsvTexture;
    if (depth == nullptr)
        return;
    if (g_depthTexture.Get() != depth || g_depthSrv == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc {};
        depth->GetDesc(&desc);
        g_depthSrv.Reset();
        g_depthTexture = depth;
        if (DepthReadable(static_cast<uint32_t>(desc.Format), (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0))
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
            sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (FAILED(g_device->CreateShaderResourceView(depth, &sd, &g_depthSrv)))
                g_depthSrv.Reset();
        }
        else
            LOG_INFO("CharaView DLSS: the preview depth texture (format {}) cannot be read; native previews retained",
                     static_cast<uint32_t>(desc.Format));
    }
}

// ---- Finish --------------------------------------------------------------------------------------------------------------
bool EnsureInit(ID3D11DeviceContext* context)
{
    if (g_passes.Ready())
        return true;
    if (g_passesTried)
        return false;
    g_passesTried = true;

    context->GetDevice(&g_device);
    g_context = context;
    guides::Shaders shaders;
    shaders.vs = { CharaGuides_Vs_cso, sizeof(CharaGuides_Vs_cso) };
    shaders.copy = { CharaGuides_Copy_cso, sizeof(CharaGuides_Copy_cso) };
    shaders.guides = { CharaGuides_Guides_cso, sizeof(CharaGuides_Guides_cso) };
    shaders.alpha = { CharaGuides_Alpha_cso, sizeof(CharaGuides_Alpha_cso) };
    if (g_device == nullptr || !g_passes.Init(g_device.Get(), shaders, &DrawFullscreenTriangle))
    {
        Latch("initialisation failed (private state, shaders or constants)");
        return false;
    }
    return true;
}

void RunSlot(int index, const DrawView& view)
{
    ReplayScope own; // the upscaler may copy or clear on this context: the hooks must not treat that as the game's work
    ID3D11DeviceContext* context = view.context;
    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    SlotState& s = g_slots.Slot(index);
    const uint32_t w = g_pass.width, h = g_pass.height;

    guides::Settings settings;
    settings.jitterX = g_pass.jitter.x;
    settings.jitterY = g_pass.jitter.y;
    settings.invWidth = 1.0f / static_cast<float>(w);
    settings.invHeight = 1.0f / static_cast<float>(h);

    // The whole step runs inside the private context state: whatever the passes or the upscaler bind is discarded with it.
    guides::Passes::Scope scope(g_passes, context, settings, camera::Buffer(), camera::FirstConstant(), camera::ConstantCount());
    if (!scope.Ok())
    {
        FailSlot(index, "the private context state could not be entered");
        return;
    }
    if (!scope.Copy(g.nativeSrv.Get(), g.color.rtv.Get(), w, h) ||
        !scope.Guides(motion::TargetSrv(), g_depthSrv.Get(), g.depth.rtv.Get(), g.motion.rtv.Get(), w, h))
    {
        FailSlot(index, "a guide pass failed");
        return;
    }

    if (!s.hasFeature && !CreateFeature(index, context, w, h, g_reversedZ))
    {
        FailSlot(index, "feature creation failed");
        return;
    }

    auto evaluate = NVNGXProxy::D3D11_EvaluateFeature(); // null when D3D11 NGX is not initialised any more
    if (evaluate == nullptr)
    {
        FailSlot(index, "D3D11 NGX is no longer available");
        return;
    }
    ngx::EvaluateInputs in;
    in.width = w;
    in.height = h;
    in.jitterX = g_pass.jitter.x;
    in.jitterY = g_pass.jitter.y;
    in.reset = g_slots.ShouldReset(index, view.now);
    ParamSetter setter { g.params };
    ngx::ApplyEvaluate<ParamSetter, ID3D11Resource*>(setter, in, g.color.texture.Get(), g.output.texture.Get(),
                                                     g.depth.texture.Get(), g.motion.texture.Get());
    const NVSDK_NGX_Result result = evaluate(context, g.handle, g.params, nullptr);
    if (result != NVSDK_NGX_Result_Success)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "evaluate %X", static_cast<unsigned>(result));
        FailSlot(index, text);
        return;
    }

    if (!scope.Alpha(g.output.srv.Get(), g.nativeSrv.Get(), g.composed.rtv.Get(), w * ngx::kOutputScale, h * ngx::kOutputScale))
    {
        FailSlot(index, "the alpha pass failed");
        return;
    }

    {
        ReplayScope own; // the query End goes through the hooked context
        context->End(g.query.Get());
    }
    g_slots.MarkEvaluated(index, view.now, g_pass.counter);
    const uint64_t n = g_evaluations.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % kEvaluationLogEvery == 1)
        LOG_INFO("CharaView DLSS: evaluations {} (slot {}, {}x{} -> {}x{})", n, index, w, h, w * ngx::kOutputScale,
                 h * ngx::kOutputScale);
}

void Finish(const DrawView& view)
{
    ID3D11Texture2D* texture = view.rtvTexture;
    if (texture == nullptr)
        return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(texture);
    g_slots.NoteRecent(key, view.now, g_pass.counter);

    const int index = g_slots.Find(key);
    FinishFacts f;
    f.width = g_pass.width;
    f.height = g_pass.height;
    f.slotFound = index != kNoSlot;
    f.now = view.now;
    if (f.slotFound)
    {
        f.slotFailed = g_slots.Slot(index).failed;
        f.lastUse = g_slots.Slot(index).lastUse;
        f.needsRebuild = g_slots.NeedsRebuild(index, g_pass.width, g_pass.height, g_reversedZ);
    }
    f.guides.replayed = g_pass.replayed;
    f.guides.depthTexture = g_depthTexture != nullptr;
    f.guides.depthView = g_depthSrv != nullptr;
    f.guides.cameraCaptured = g_pass.cameraCaptured;

    const FinishVerdict verdict = DecideFinish(f);
    switch (verdict)
    {
    case FinishVerdict::NoSlot: return;
    case FinishVerdict::TooLarge:
    case FinishVerdict::Stale:
        // The native image was just written and this render is not evaluated: the UI must not keep an older composed image.
        if (index != kNoSlot)
            g_slots.ClearEvaluated(index);
        return;
    case FinishVerdict::Failed: CountFallback(); return;
    case FinishVerdict::WaitingForMotion:
    case FinishVerdict::WaitingForDepth:
    case FinishVerdict::WaitingForDepthView:
    case FinishVerdict::WaitingForCamera:
    {
        g_slots.ClearEvaluated(index);
        CountFallback();
        bool& logged = g_waitLogged[static_cast<size_t>(verdict) - static_cast<size_t>(FinishVerdict::WaitingForMotion)];
        if (!logged)
        {
            logged = true;
            LOG_INFO("CharaView DLSS: {}", WaitingText(verdict));
        }
        return;
    }
    case FinishVerdict::Rebuild:
        ReleaseSlotGpu(index);
        g_slots.ClearEvaluated(index);
        g_slots.Slot(index).width = g_pass.width;
        g_slots.Slot(index).height = g_pass.height;
        g_slots.Slot(index).reversedZ = g_reversedZ;
        break;
    case FinishVerdict::Proceed: break;
    }

    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    if (g.nativeSrv == nullptr)
    {
        g_slots.Slot(index).reversedZ = g_reversedZ;
        if (!CreateSlotImages(index, texture, g_pass.width, g_pass.height))
        {
            FailSlot(index, "the slot images could not be created");
            return;
        }
        UpdateCounts();
    }
    RunSlot(index, view);
    UpdateCounts();
}

// ---- Consumer ------------------------------------------------------------------------------------------------------------
// A UI draw that samples a preview image: the composed image replaces it for this one draw.
bool Consume(const DrawView& view, DrawCall draw)
{
    ID3D11DeviceContext* context = view.context;
    ID3D11ShaderResourceView* original[kSrvSlots] {};
    context->PSGetShaderResources(0, kSrvSlots, original);

    ID3D11ShaderResourceView* replaced[kSrvSlots] {};
    bool any = false;
    for (UINT i = 0; i < kSrvSlots; ++i)
    {
        replaced[i] = original[i];
        if (original[i] == nullptr)
            continue;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
        original[i]->GetDesc(&sd);
        if (sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || sd.Texture2D.MostDetailedMip != 0)
            continue;

        ComPtr<ID3D11Resource> resource;
        original[i]->GetResource(&resource);
        const uintptr_t key = reinterpret_cast<uintptr_t>(resource.Get());
        int index = g_slots.Find(key);
        if (index == kNoSlot && g_slots.RecentContains(key, view.now))
        {
            ComPtr<ID3D11Texture2D> texture;
            D3D11_TEXTURE2D_DESC td {};
            if (SUCCEEDED(resource.As(&texture)))
            {
                texture->GetDesc(&td);
                index = g_slots.AcquireForConsumer(key, view.now, td.Width, td.Height);
                if (index != kNoSlot)
                    UpdateCounts();
            }
        }
        if (index == kNoSlot)
            continue;

        g_slots.Touch(index, view.now);
        const SlotState& s = g_slots.Slot(index);
        const SlotGpu& g = g_gpu[static_cast<size_t>(index)];
        if (s.everEvaluated && !s.failed && g.composed.srv != nullptr)
        {
            replaced[i] = g.composed.srv.Get();
            any = true;
        }
    }

    bool handled = false;
    if (any)
    {
        context->PSSetShaderResources(0, kSrvSlots, replaced);
        draw();
        context->PSSetShaderResources(0, kSrvSlots, original);
        handled = true;
    }
    for (auto* v : original)
    {
        if (v != nullptr)
            v->Release();
    }
    return handled;
}

// ---- Cleanup -------------------------------------------------------------------------------------------------------------
void Cleanup(uint64_t now, bool disabled)
{
    if (g_context == nullptr)
        return;
    g_slots.Cleanup(
        now, disabled,
        [](int i) {
            const SlotGpu& g = g_gpu[static_cast<size_t>(i)];
            return g.query == nullptr || g_context->GetData(g.query.Get(), nullptr, 0, 0) == S_OK;
        },
        [](int i) { ReleaseSlotGpu(i); });
    UpdateCounts();
}
} // namespace

bool Implemented() { return true; }

bool Active()
{
    // A latched path stays active while GPU objects remain, so the maintenance calls keep releasing them.
    if (g_latched.load(std::memory_order_relaxed))
        return g_slotsHeld.load(std::memory_order_relaxed) != 0;

    // NGX moved to another device since the last comparison: compare again on the next draw.
    if (g_deviceState.load(std::memory_order_relaxed) != 0 &&
        State::Instance().currentD3D11Device != g_checkedNgxDevice.load(std::memory_order_relaxed))
        g_deviceState.store(0, std::memory_order_relaxed);

    const uint64_t now = GetTickCount64();
    if (now >= g_nextGateCheck.load(std::memory_order_relaxed))
        Refresh(now);
    const bool active = static_cast<Availability>(g_availability.load(std::memory_order_relaxed)) == Availability::Available;
    if (active && !g_wasActive.exchange(true))
        motion::MarkQueriesBlind(); // queries opened before the hooks saw them are unknown until the next frame end
    else if (!active)
        g_wasActive.store(false, std::memory_order_relaxed);
    return active;
}

bool Around(const DrawView& view, DrawCall draw)
{
    if (g_latched.load(std::memory_order_relaxed))
        return false;
    ID3D11DeviceContext* context = view.context;
    CheckDevice(context, view.now);
    if (view.now >= g_nextGateCheck.load(std::memory_order_relaxed))
        Refresh(view.now);
    if (static_cast<Availability>(g_availability.load(std::memory_order_relaxed)) != Availability::Available)
        return false;
    if (view.kind == DrawKind::Ignore || !EnsureInit(context))
        return false;
    g_context = context;

    if ((view.kind == DrawKind::Prepass || view.kind == DrawKind::GBuffer) &&
        (view.passStarted || g_pass.counter != (view.pass != nullptr ? view.pass->counter : 0)))
        BeginPass(view);

    switch (view.kind)
    {
    case DrawKind::Prepass:
    {
        if (!g_pass.live)
            return false;
        {
            JitterScope shift(context, g_pass.jitter);
            draw();
        }
        g_pass.prepassSeen = true;
        return true;
    }
    case DrawKind::GBuffer:
    {
        if (!g_pass.live)
            return false;
        if (!g_pass.prepassSeen && !g_pass.jitterZeroed)
        {
            g_pass.jitterZeroed = true;
            g_pass.jitter = {};
            if (!g_loggedNoPrepass)
            {
                g_loggedNoPrepass = true;
                LOG_INFO("CharaView DLSS: no verified depth prepass this frame; native raster positions retained");
            }
        }
        const bool opaque = view.facts.rtv0Entry == 8;
        if (opaque && (view.roles & RoleGeometry) != 0 && !g_pass.cameraCaptured)
            CaptureGuideInputs(view);
        JitterScope shift(context, g_pass.jitter);
        if (opaque && motion::Replay(view, draw).outcome == motion::Outcome::Replayed)
            g_pass.replayed = true;
        draw();
        return true;
    }
    case DrawKind::Finisher:
    {
        draw(); // the native image first
        if (!view.finisherAccepted)
        {
            // The native image of this render was written but will not be upscaled: stop showing the older composed one.
            if (view.rtvTexture != nullptr)
            {
                const int rejected = g_slots.Find(reinterpret_cast<uintptr_t>(view.rtvTexture));
                if (rejected != kNoSlot)
                    g_slots.ClearEvaluated(rejected);
            }
            if (g_pass.live)
                CountFallback();
            return true;
        }
        Finish(view);
        return true;
    }
    case DrawKind::Consumer: return Consume(view, draw);
    default: return false;
    }
}

void OnCopy(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource* source, bool)
{
    // A copy of a preview output: the UI draws that matter sample the output itself (the dispatcher only treats draws that
    // sample a table entry as consumers), so the copy is only counted.
    if (g_slots.RecentContains(reinterpret_cast<uintptr_t>(source), GetTickCount64()))
    {
        const uint64_t n = g_copyChains.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n % 600 == 0)
            LOG_INFO("CharaView DLSS: copies of a preview output seen: {}", n);
    }
}

void OnClearRenderTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const FLOAT[4])
{
    // The game clears the opaque G-buffer: the signed-motion target is cleared with it (needs the guide capture key).
    if (view == nullptr || !g_pass.live || !ext::Cfg().ffxiv.chara.previewGuideCapture.value_or_default())
        return;
    const table::Snapshot& snap = table::Current(GetTickCount64());
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (table::IndexOfPointer(snap, reinterpret_cast<uintptr_t>(resource.Get())) == 8)
        motion::ClearTarget(context);
}

void OnQueryBegin(ID3D11DeviceContext*, ID3D11Asynchronous* query) { motion::OnQueryBegin(query); }
void OnQueryEnd(ID3D11DeviceContext*, ID3D11Asynchronous* query) { motion::OnQueryEnd(query); }
void OnVertexShaderCreated(ID3D11Device*, ID3D11VertexShader*, const void*, SIZE_T) {}

void OnFrameEnd()
{
    motion::OnFrameEnd();
    const bool disabled = g_latched.load(std::memory_order_relaxed) || !ext::Cfg().ffxiv.chara.previewDlss.value_or_default();
    Cleanup(GetTickCount64(), disabled);
}

void Maintain(uint64_t nowMs)
{
    motion::OnMaintain(nowMs); // lifts the open-query gate when no frame callback exists
    const bool disabled = g_latched.load(std::memory_order_relaxed) || !ext::Cfg().ffxiv.chara.previewDlss.value_or_default();
    Cleanup(nowMs, disabled);
}

void Detach()
{
    // The hooks are going away and the device is still valid: release everything now (the GPU work of the last frames
    // is finished by the time the device is destroyed).
    for (size_t i = 0; i < kMaxSlots; ++i)
        ReleaseSlotGpu(static_cast<int>(i));
    g_slots.Clear();
    g_passes.Release();
    g_passesTried = false;
    motion::Release();
    camera::Release();
    g_depthSrv.Reset();
    g_depthTexture.Reset();
    g_pass = PassData {};
    g_context = nullptr;
    g_device.Reset();
    g_contextDeviceRef.Reset();
    g_deviceState.store(0, std::memory_order_relaxed);
    g_wasActive.store(false, std::memory_order_relaxed);
    UpdateCounts();
}

Counters GetCounters()
{
    Counters c;
    c.activeSlots = g_activeSlots.load(std::memory_order_relaxed);
    c.evaluations = g_evaluations.load(std::memory_order_relaxed);
    c.fallbacks = g_fallbacks.load(std::memory_order_relaxed);
    return c;
}

const char* StatusNote()
{
    // The gate is only refreshed while the dispatcher asks: with the feature off there is nothing to say.
    if (!ext::Cfg().ffxiv.chara.previewDlss.value_or_default())
        return "";
    if (g_latched.load(std::memory_order_relaxed))
        return ReasonText(Availability::Latched);
    const auto a = static_cast<Availability>(g_availability.load(std::memory_order_relaxed));
    return ReasonText(a);
}
} // namespace ext::ffxiv::chara::dlss
