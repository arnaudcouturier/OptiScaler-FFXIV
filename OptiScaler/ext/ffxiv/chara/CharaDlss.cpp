#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaCamera.h"
#include "ext/ffxiv/chara/CharaDiag.h"
#include "ext/ffxiv/chara/CharaDispatch.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaDlssGate.h"
#include "ext/ffxiv/chara/CharaDlssNgx.h"
#include "ext/ffxiv/chara/CharaDlssRules.h"
#include "ext/ffxiv/chara/CharaFlood.h"
#include "ext/ffxiv/chara/CharaGuides.h"
#include "ext/ffxiv/chara/CharaIdentify.h"
#include "ext/ffxiv/chara/CharaJitter.h"
#include "ext/ffxiv/chara/CharaMotion.h"
#include "ext/ffxiv/chara/CharaNr.h"
#include "ext/ffxiv/chara/CharaNrPlumbing.h"
#include "ext/ffxiv/chara/CharaNrTint.h"
#include "ext/ffxiv/chara/CharaProbeHooks.h"
#include "ext/ffxiv/chara/CharaSlots.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTable.h"
#include "ext/ffxiv/chara/CharaTags.h"

#include <Config.h>
#include <State.h>
#include <proxies/NVNGX_Proxy.h>

#include <CharaGuides_AlphaTintNr_Shader.h>
#include <CharaGuides_AlphaTint_Shader.h>
#include <CharaGuides_Alpha_Shader.h>
#include <CharaGuides_Copy_Shader.h>
#include <CharaGuides_Guides_Shader.h>
#include <CharaGuides_Vs_Shader.h>

#include <array>
#include <cstdio>
#include <format>
#include <atomic>
#include <mutex>
#include <string>
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
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == ngx::kPresetHintNames[0]);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == ngx::kPresetHintNames[1]);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == ngx::kPresetHintNames[2]);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == ngx::kPresetHintNames[3]);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == ngx::kPresetHintNames[4]);
static_assert(std::string_view(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == ngx::kPresetHintNames[5]);
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
constexpr uint32_t kNrMaxSide = 4096; // larger finisher outputs are not offered to preview NR

// ---- Gate state (any thread) -------------------------------------------------------------------------------------------
// The SR latch lives in the object shared with preview NR (nr::SharedLatches()): an SR failure must not stop NR and the reverse.
bool SrLatched() { return nr::SharedLatches().SrLatched(); }
std::atomic<uint32_t> g_slotsHeld { 0 }; // slots whose GPU objects still exist (a latched path drains them before it stops)
std::atomic<uint8_t> g_availability { static_cast<uint8_t>(Availability::ConfigOff) };
std::atomic<uint64_t> g_nextGateCheck { 0 };
nr::EdgeTracker g_queryHooks; // whether the SR hooks (including the query hooks) were being fed at the last look

std::atomic<uint8_t> g_deviceState { 0 }; // 0 not compared yet, 1 same device, 2 different device

// The device NGX was initialised with: identity values only (never dereferenced after the capture, the device may be released).
std::atomic<uintptr_t> g_ngxRaw { 0 };
std::atomic<uintptr_t> g_ngxNative { 0 };
std::atomic<uint32_t> g_ngxGeneration { 0 };
std::atomic<uintptr_t> g_drawDeviceRaw { 0 };    // the last compared draw context's device, and what a preview resource says
std::atomic<uintptr_t> g_drawDeviceNative { 0 };

std::mutex g_logMutex;
ReasonLog g_reasonLog;

std::atomic<uint32_t> g_activeSlots { 0 };
std::atomic<uint64_t> g_evaluations { 0 };
std::atomic<uint64_t> g_fallbacks { 0 };
std::atomic<uint64_t> g_copyChains { 0 };
FailureWindow g_failures; // draw thread only

// ---- Draw-thread state --------------------------------------------------------------------------------------------------
DeviceCache g_deviceCache;
uint32_t g_deviceLines = 0;

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
    uint32_t ngxGeneration = 0; // the NGX instance (g_ngxGeneration) that created params and handle
    bool failureLogged = false;
    bool composedFromNr = false; // the composed image was made from the production NR output (written by every alpha pass)
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
nr::ConsumerSeen g_consumed; // preview images a UI draw sampled lately: the liveness signal that gates NR (bring-up and production)
constexpr uint64_t kNrLiveWindowMs = 1000;
nr::TintTargets g_nrTint;             // the cyan copies of bring-up NR results (PreviewDebugTint), made once per result
constexpr uint64_t kNrTintUnusedMs = 5000;
bool g_nrProductionThisRender = false; // production NR improved the SR output of the finisher being handled (reset per finisher)
nr::CauseLog g_productionCauses;      // one log line per cause of a production fallback (the module counts every one)
bool g_nrTintFailureLogged = false;
std::array<SlotGpu, kMaxSlots> g_gpu;
PassData g_pass;
int g_lastEvaluatedSlot = kNoSlot; // the slot of the most recent successful evaluation (the preview image dump reads its composed image)
ComPtr<ID3D11Device> g_device;
ID3D11DeviceContext* g_context = nullptr; // the immediate context (valid until Detach)
guides::Passes g_passes;
bool g_passesTried = false;
ComPtr<ID3D11Texture2D> g_depthTexture;
ComPtr<ID3D11ShaderResourceView> g_depthSrv;
DepthMemo g_depthMemo; // what was learnt about g_depthTexture (an unreadable one is looked at and logged once)
bool g_reversedZ = false;
motion::DepthConventionMemo g_depthConvention; // what the depth states of the G-buffer draws said so far
bool g_loggedNoPrepass = false;
BurstLimiter g_lines; // log lines of the path: once per reason per burst of preview activity (reasons: FinishVerdict values, then kLine*)
constexpr size_t kLineSlotAcquired = 10;
constexpr size_t kLinePassLive = 11;
constexpr size_t kLineSwap = 12;
constexpr size_t kLineMotionTarget = 13;
constexpr size_t kLineNrSwap = 14;
constexpr size_t kLineProductionSwap = 15;

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
    f.latched = SrLatched();
    f.ngxModuleLoaded = NVNGXProxy::NVNGXModule() != nullptr;
    f.dlssEnabledInConfig = Config::Instance()->DLSSEnabled.value_or_default();
    f.dx11Initialised = NVNGXProxy::IsDx11Inited();
    f.entryPoints = EntryPointsPresent();
    const uint8_t device = g_deviceState.load(std::memory_order_relaxed);
    f.deviceChecked = device != 0;
    f.deviceMatches = device == 1;
    return f;
}

// The facts the pure plumbing rules (CharaNrPlumbing.h) decide on. The SR gate result is the cached one (Refresh keeps it current).
nr::PlumbingFacts PlumbingNow()
{
    nr::PlumbingFacts f;
    f.srWanted = ext::Cfg().ffxiv.chara.previewDlss.value_or_default();
    f.nrWanted = nr::Wanted();
    f.srLatched = SrLatched();
    f.nrLatched = nr::Latched();
    f.nrAvailable = nr::Available();
    f.srGateOpen = static_cast<Availability>(g_availability.load(std::memory_order_relaxed)) == Availability::Available;
    f.nrHoldsResources = nr::HoldsResources();
    f.srSlotsHeld = g_slotsHeld.load(std::memory_order_relaxed);
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

// The device a resource created through `device` reports (see DeviceIdentity). 0 when no resource could be made.
uintptr_t NativeDeviceOf(ID3D11Device* device)
{
    if (device == nullptr)
        return 0;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> probe;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &probe)) || probe == nullptr)
        return 0;
    ComPtr<ID3D11Device> owner;
    probe->GetDevice(&owner);
    return reinterpret_cast<uintptr_t>(owner.Get());
}

std::string DeviceText(const DeviceIdentity& d)
{
    return std::format("{:#x} (native {:#x}, {})", d.raw, d.native, d.native == 0 ? "native device not readable" : d.IsProxy() ? "a wrapper" : "not wrapped");
}

const char* VerdictText(DeviceVerdict v)
{
    switch (v)
    {
    case DeviceVerdict::Same: return "same device";
    case DeviceVerdict::Different: return "different devices";
    default: return "not compared";
    }
}

// Compares the device NGX was initialised on with the device the preview draws run on, through the devices that resources
// report (a wrapper such as ReShade's proxy has another pointer than the native device it forwards to). Draw thread; the
// verdict is cached and only recomputed when NGX (re)initialises, the draw context changes or a "different" verdict gets old.
void CheckDevice(const DrawView& view)
{
    const uintptr_t ngxRaw = g_ngxRaw.load(std::memory_order_relaxed);
    if (ngxRaw == 0 || view.context == nullptr || view.kind == DrawKind::Ignore)
        return; // NGX has not seen a device yet (the proxy gate reports that), or this draw says nothing about the preview device

    const uint32_t generation = g_ngxGeneration.load(std::memory_order_relaxed);
    const uintptr_t key = reinterpret_cast<uintptr_t>(view.context);
    if (!g_deviceCache.NeedsCheck(generation, key, view.now))
        return;

    ComPtr<ID3D11Device> contextDevice;
    view.context->GetDevice(&contextDevice);
    DeviceIdentity draw { reinterpret_cast<uintptr_t>(contextDevice.Get()), 0 };
    draw.native = NativeDeviceOf(contextDevice.Get());

    // The texture of the draw is the device the preview images really live on: it decides when it can be read.
    uintptr_t textureDevice = 0;
    if (ID3D11Texture2D* texture = view.rtvTexture != nullptr ? view.rtvTexture : view.dsvTexture; texture != nullptr)
    {
        ComPtr<ID3D11Device> owner;
        texture->GetDevice(&owner);
        textureDevice = reinterpret_cast<uintptr_t>(owner.Get());
    }
    if (textureDevice != 0)
        draw.native = textureDevice;

    const DeviceIdentity ngx { ngxRaw, g_ngxNative.load(std::memory_order_relaxed) };
    const DeviceVerdict verdict = CompareDevices(ngx, draw);
    const DeviceVerdict previous = g_deviceCache.verdict;
    g_deviceCache.Store(verdict, generation, key, view.now);
    g_drawDeviceRaw.store(draw.raw, std::memory_order_relaxed);
    g_drawDeviceNative.store(draw.native, std::memory_order_relaxed);
    g_deviceState.store(verdict == DeviceVerdict::Same ? 1 : verdict == DeviceVerdict::Different ? 2 : 0, std::memory_order_relaxed);
    g_nextGateCheck.store(0, std::memory_order_relaxed); // the gate reads the new verdict now

    if (verdict != previous && g_deviceLines < 4)
    {
        ++g_deviceLines;
        LOG_INFO("CharaView DLSS device check: {}. NGX device {}; draw context device {}; device of the preview texture {:#x}; "
                 "draw context {:#x}",
                 VerdictText(verdict), DeviceText(ngx), DeviceText(draw), textureDevice, key);
    }
}
void Latch(const char* why)
{
    if (!nr::SharedLatches().LatchSr())
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

// The query hooks are forwarded only while the SR hooks are needed. Whenever that changes, in either direction, the queries that were
// open are unknown (an End nobody saw would keep every motion replay refused): the set is forgotten and the replay waits for the next
// frame end. Asked from the hook forwarders themselves, which run whether or not the dispatcher's gate is open.
void NoteQueryHooks(bool hooksNeeded)
{
    if (g_queryHooks.Changed(hooksNeeded))
        motion::MarkQueriesBlind();
}

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
    const bool ngxUsable = DecideFeatureUse(g.handle != nullptr || g.params != nullptr, g.ngxGeneration,
                                            g_ngxGeneration.load(std::memory_order_acquire), NVNGXProxy::IsDx11Inited(),
                                            State::Instance().isShuttingDown, true) == FeatureUse::ReleaseThroughDriver;
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

// The pointers a CreateFeature / EvaluateFeature call involves, for the log line that settles whether the driver accepted them.
std::string PointerSet(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11Device> contextDevice;
    if (context != nullptr)
        context->GetDevice(&contextDevice);
    return std::format("context {:#x}, its device {:#x}, NGX device {:#x} (native {:#x}), preview texture device {:#x}",
                       reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(contextDevice.Get()),
                       g_ngxRaw.load(std::memory_order_relaxed), g_ngxNative.load(std::memory_order_relaxed),
                       g_drawDeviceNative.load(std::memory_order_relaxed));
}

// The render-preset hints of the main game's DLSS: the configuration's override when it is on, otherwise the values the game
// requested for its own feature (State keeps them while the override is off).
ngx::PresetHints CurrentPresetHints()
{
    const Config& cfg = *Config::Instance();
    const State& state = State::Instance();
    ngx::PresetSource source;
    source.overrideOn = cfg.RenderPresetOverride.value_or_default();
    source.forAll = static_cast<const std::optional<uint32_t>&>(cfg.RenderPresetForAll);
    const std::optional<uint32_t>* perQuality[ngx::kPresetHintCount] = {
        &cfg.RenderPresetDLAA, &cfg.RenderPresetUltraQuality, &cfg.RenderPresetQuality,
        &cfg.RenderPresetBalanced, &cfg.RenderPresetPerformance, &cfg.RenderPresetUltraPerformance,
    };
    const uint32_t game[ngx::kPresetHintCount] = {
        state.dlssRenderPresetDLAA, state.dlssRenderPresetUltraQuality, state.dlssRenderPresetQuality,
        state.dlssRenderPresetBalanced, state.dlssRenderPresetPerformance, state.dlssRenderPresetUltraPerformance,
    };
    for (size_t i = 0; i < ngx::kPresetHintCount; ++i)
    {
        source.perQuality[i] = *perQuality[i];
        source.game[i] = game[i];
    }
    return ngx::ComposePresetHints(source);
}

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
    // Read once: the values that are applied are the values that are logged (the configuration or the main DLSS can change in between).
    const ngx::PresetHints hints = CurrentPresetHints();
    ngx::ApplyPresetHints(setter, hints);

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result result = create(context, NVSDK_NGX_Feature_SuperSampling, params, &handle);
    if (result != NVSDK_NGX_Result_Success || handle == nullptr)
    {
        if (auto destroy = NVNGXProxy::D3D11_DestroyParameters(); destroy != nullptr)
            destroy(params);
        LOG_WARN("CharaView DLSS: NGX CreateFeature returned {:#x}; {}", static_cast<unsigned>(result), PointerSet(context));
        char text[96];
        std::snprintf(text, sizeof(text), "feature creation failed (0x%X)", static_cast<unsigned>(result));
        Latch(text);
        return false;
    }
    g.params = params;
    g.handle = handle;
    g.ngxGeneration = g_ngxGeneration.load(std::memory_order_acquire);
    g_slots.Slot(index).hasFeature = true;
    LOG_INFO("CreateFeature CharaView DLSS: isolated feature {}x{} -> {}x{}, native alpha retained; NGX result {:#x}; render presets "
             "DLAA {} / UltraQuality {} / Quality {} / Balanced {} / Performance {} / UltraPerformance {}; {}",
             w, h, w * ngx::kOutputScale, h * ngx::kOutputScale, static_cast<unsigned>(result), hints.value[0], hints.value[1],
             hints.value[2], hints.value[3], hints.value[4], hints.value[5], PointerSet(context));
    UpdateCounts();
    return true;
}

void FailSlot(int index, const char* why)
{
    const uint64_t now = GetTickCount64();
    g_slots.MarkFailed(index, now); // released again after kFailedReleaseMs, so the preview can try again
    CountFallback();
    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    if (!g.failureLogged)
    {
        g.failureLogged = true;
        LOG_WARN("CharaView DLSS: {}; native preview retained", why);
    }
    // Failures count inside a sliding window: transient ones decay, four close together end the path for the session.
    if (g_failures.Note(now) >= kMaxSlotFailures)
        Latch("several previews failed");
}

// True when the work submitted for slot `i` finished: no query to wait for, S_OK, or a failed query (a lost device never reports S_OK).
bool SlotQueryDone(int i)
{
    const SlotGpu& g = g_gpu[static_cast<size_t>(i)];
    if (g.query == nullptr || g_context == nullptr)
        return true;
    return QueryResultMeansDone(static_cast<int32_t>(g_context->GetData(g.query.Get(), nullptr, 0, 0)));
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
            g_pass.jitter = PassJitter(g_slots.JitterPhase(g_pass.counter)); // the phase of the preview this render most likely is for
        // The signed-motion target is ensured and cleared at the start of every pass.
        if (!motion::EnsureTarget(g_device.Get(), g_pass.width, g_pass.height))
        {
            g_pass.live = false;
            if (g_lines.ShouldLog(kLineMotionTarget, identify::BurstNumber()))
                LOG_WARN("CharaView DLSS: the signed-motion target ({}x{}) could not be created; this pass is not followed", g_pass.width,
                         g_pass.height);
        }
        else
            motion::ClearTarget(view.context);
    }
    if (g_pass.live)
    {
        ++g_stats.passesLive;
        if (g_lines.ShouldLog(kLinePassLive, identify::BurstNumber()))
            LOG_INFO("CharaView DLSS: pass {} ({}x{}) is followed: a preview slot exists, jitter {:.4f}/{:.4f}", g_pass.counter, g_pass.width,
                     g_pass.height, g_pass.jitter.x, g_pass.jitter.y);
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

    // The last draw that has a say decides; a draw that says nothing (depth test off, ALWAYS) keeps the earlier answer. The convention
    // is a property of the renderer, so it is not overwritten by a masking draw (a flip would rebuild the slot).
    g_depthConvention.Observe(motion::BoundDepthConvention(context));
    g_reversedZ = g_depthConvention.Reversed();

    ID3D11Texture2D* depth = view.dsvTexture;
    if (depth == nullptr)
        return;
    // A texture that cannot be read (or whose view cannot be created after a few tries) is remembered: it is neither examined nor
    // logged again in every pass.
    const uintptr_t key = reinterpret_cast<uintptr_t>(depth);
    if (!g_depthMemo.ShouldExamine(key, g_depthSrv != nullptr))
        return;
    g_depthMemo.Start(key);
    D3D11_TEXTURE2D_DESC desc {};
    depth->GetDesc(&desc);
    if (g_depthTexture.Get() != depth)
    {
        g_depthSrv.Reset();
        g_depthTexture = depth;
    }
    if (DepthReadable(static_cast<uint32_t>(desc.Format), (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0))
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(depth, &sd, &g_depthSrv)))
        {
            g_depthSrv.Reset();
            if (g_depthMemo.ViewFailed())
                LOG_INFO("CharaView DLSS: a shader resource view of the preview depth texture could not be created ({} tries); native "
                         "previews retained",
                         DepthMemo::kMaxViewFailures);
        }
    }
    else
    {
        g_depthMemo.Reject();
        LOG_INFO("CharaView DLSS: the preview depth texture (format {}) cannot be read; native previews retained",
                 static_cast<uint32_t>(desc.Format));
    }
}

// ---- Finish --------------------------------------------------------------------------------------------------------------
// The helper passes (copy, guides, alpha, tints). `latchSrOnFailure`: the super-resolution path needs them, so a failure ends it; the tint of
// a bring-up NR result borrows them (PreviewNR without PreviewDLSS) and must not end a path that is not running.
bool EnsureInit(ID3D11DeviceContext* context, bool latchSrOnFailure = true)
{
    if (g_passes.Ready())
        return true;
    if (g_passesTried)
    {
        if (latchSrOnFailure)
            Latch("initialisation failed earlier (private state, shaders or constants)"); // a tint-only attempt failed before the SR path asked
        return false;
    }
    g_passesTried = true;

    context->GetDevice(&g_device);
    g_context = context;
    guides::Shaders shaders;
    shaders.vs = { CharaGuides_Vs_cso, sizeof(CharaGuides_Vs_cso) };
    shaders.copy = { CharaGuides_Copy_cso, sizeof(CharaGuides_Copy_cso) };
    shaders.guides = { CharaGuides_Guides_cso, sizeof(CharaGuides_Guides_cso) };
    shaders.alpha = { CharaGuides_Alpha_cso, sizeof(CharaGuides_Alpha_cso) };
    shaders.alphaTint = { CharaGuides_AlphaTint_cso, sizeof(CharaGuides_AlphaTint_cso) };
    shaders.alphaTintNr = { CharaGuides_AlphaTintNr_cso, sizeof(CharaGuides_AlphaTintNr_cso) };
    if (g_device == nullptr || !g_passes.Init(g_device.Get(), shaders, &DrawFullscreenTriangle))
    {
        if (latchSrOnFailure)
            Latch("initialisation failed (private state, shaders or constants)");
        return false;
    }
    return true;
}

ComPtr<ID3D11ShaderResourceView> RunNrProduction(const DrawView& view, const SlotGpu& g, const SlotState& s, uint32_t w, uint32_t h); // preview NR production entry, defined below

void RunSlot(int index, const DrawView& view)
{
    ReplayScope own; // the upscaler may copy or clear on this context: the hooks must not treat that as the game's work
    ID3D11DeviceContext* context = view.context;
    SlotGpu& g = g_gpu[static_cast<size_t>(index)];
    SlotState& s = g_slots.Slot(index);
    const uint32_t w = g_pass.width, h = g_pass.height;

    // NGX was shut down and initialised again since this slot's feature was created: its objects belong to the old instance.
    if (s.hasFeature && DecideFeatureUse(g.handle != nullptr || g.params != nullptr, g.ngxGeneration,
                                         g_ngxGeneration.load(std::memory_order_acquire), NVNGXProxy::IsDx11Inited(),
                                         State::Instance().isShuttingDown, false) == FeatureUse::Forget)
    {
        g.handle = nullptr;
        g.params = nullptr;
        s.hasFeature = false;
        g_slots.ClearEvaluated(index);
        UpdateCounts();
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            LOG_INFO("CharaView DLSS: NGX was re-initialised; preview features are recreated");
        }
    }

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

    // Preview NR, production entry: the SR evaluate succeeded for this render. NR runs on the SR output with the real guides and, when it
    // produced an image, the alpha pass recomposes the original alpha onto that image instead of the SR output. Any other outcome leaves
    // the SR output as it is (fail open); an NR problem is never a problem of this slot.
    const uintptr_t previewKey = reinterpret_cast<uintptr_t>(view.rtvTexture);
    const bool consumerRecent = view.rtvTexture != nullptr && g_consumed.Recent(previewKey, view.now, kNrLiveWindowMs);
    ComPtr<ID3D11ShaderResourceView> nrOutput;
    if (nr::RunProductionNow(PlumbingNow(), true, consumerRecent))
        nrOutput = RunNrProduction(view, g, s, w, h);
    nr::AlphaInput alphaInput = nr::ChooseAlphaInput(nrOutput != nullptr);

    const bool tinted = ext::Cfg().ffxiv.chara.previewDebugTint.value_or_default();
    const uint32_t outW = w * ngx::kOutputScale, outH = h * ngx::kOutputScale;
    const auto alphaPass = [&](nr::AlphaInput input)
    {
        ID3D11ShaderResourceView* colour = input == nr::AlphaInput::NrOutput ? nrOutput.Get() : g.output.srv.Get();
        const nr::TintKind kind = nr::AlphaTintFor(tinted, input);
        const guides::Tint tint = kind == nr::TintKind::Nr ? guides::Tint::Nr : kind == nr::TintKind::Sr ? guides::Tint::Sr : guides::Tint::None;
        return scope.Alpha(colour, g.nativeSrv.Get(), g.composed.rtv.Get(), outW, outH, tint);
    };
    g.composedFromNr = false; // written by every alpha pass: a render without NR never inherits the flag of an earlier one
    bool alphaOk = alphaPass(alphaInput);
    if (nr::RetryAlphaWithSr(alphaInput, alphaOk))
    {
        // The alpha pass failed with the NR image: try the SR output before this slot counts as failed (an NR problem must not count
        // toward the SR latch).
        LOG_WARN("CharaView NR: the alpha pass failed with the production NR output; the super-resolution output is used");
        alphaInput = nr::AlphaInput::SrOutput;
        alphaOk = alphaPass(alphaInput);
    }
    if (!alphaOk)
    {
        FailSlot(index, "the alpha pass failed");
        return;
    }
    g.composedFromNr = alphaInput == nr::AlphaInput::NrOutput;
    g_nrProductionThisRender = g.composedFromNr;
    if (g.composedFromNr)
        ++g_stats.nrProductionRuns;
    if (tinted)
        ++g_stats.tintApplied; // the composed image: magenta for SR, cyan when its colour came from NR

    {
        ReplayScope own; // the query End goes through the hooked context
        context->End(g.query.Get());
    }
    g_slots.MarkEvaluated(index, view.now, g_pass.counter);
    g_lastEvaluatedSlot = index;
    const uint64_t n = g_evaluations.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % kEvaluationLogEvery == 1 || n <= 3)
        LOG_INFO("CharaView DLSS: evaluations {} (slot {}, {}x{} -> {}x{})", n, index, w, h, w * ngx::kOutputScale,
                 h * ngx::kOutputScale);

    if (diag::CaptureEnabled())
    {
        diag::Request request;
        request.native = view.rtvTexture;
        request.input = g.color.texture.Get();
        request.dlss = g.output.texture.Get();
        request.composed = g.composed.texture.Get();
        request.depth = g.depth.texture.Get();
        request.motion = g.motion.texture.Get();
        request.camera = camera::Buffer();
        request.width = w;
        request.height = h;
        request.jitterX = g_pass.jitter.x;
        request.jitterY = g_pass.jitter.y;
        diag::Capture(context, request, n, view.now);
    }
    if (diag::TraceEnabled())
        diag::Trace(diag::TraceCategory::Guides, view.now,
                    std::format("evaluated slot {} {}x{} jitter {:.4f}/{:.4f} reset {} reversedZ {} pass {}", index, w, h, g_pass.jitter.x,
                                g_pass.jitter.y, in.reset, g_reversedZ, g_pass.counter));
}

// ---- Preview NR hooks -----------------------------------------------------------------------------------------------------
// Bring-up entry: right after the finisher wrote the native image. The module copies it to its shared colour image and queues the
// NR list on the bridge's D3D12 queue (colour only, synthetic guides). Draw thread, immediate context. Failures stay inside the
// module (it latches NR, not the SR path). True: NR produced a result for this render.
bool RunNrBringUpInner(const DrawView& view)
{
    ID3D11Texture2D* texture = view.rtvTexture;
    const uint32_t w = view.facts.targetWidth, h = view.facts.targetHeight;
    if (texture == nullptr || w == 0 || h == 0 || w > kNrMaxSide || h > kNrMaxSide)
        return false;
    ReplayScope own; // the module copies on this context: the copy hooks must not treat that as the game's work
    nr::RunRequest request;
    request.context = view.context;
    request.native = texture;
    request.width = w;
    request.height = h;
    request.now = view.now;
    const nr::RunOutcome outcome = nr::RunBringUp(request);
    if (outcome == nr::RunOutcome::Ran)
        ++g_stats.nrRuns;
    if (diag::TraceEnabled() && outcome != nr::RunOutcome::Ran)
        diag::Trace(diag::TraceCategory::Finish, view.now, std::format("NR bring-up outcome {} for {}x{}", static_cast<int>(outcome), w, h));
    return outcome == nr::RunOutcome::Ran;
}

// The second line of defence behind the module's own boundary (nr::RunBringUp never throws): whatever this function does around the
// call (the trace text, the statistics) cannot leave the game's Draw hook either. An exception here latches preview NR and the render
// keeps the native image.
bool RunNrBringUp(const DrawView& view)
{
    return nr::Guarded([&] { return RunNrBringUpInner(view); },
                       []
                       {
                           if (nr::SharedLatches().LatchNr())
                               LOG_WARN("CharaView NR: an unexpected exception in the dispatcher's bring-up step; switched off for this session, native previews retained");
                           return false;
                       });
}

// Production entry: between the SR evaluate and the alpha pass of RunSlot, with the real guides. Only invoked when the SR evaluate
// succeeded for this render (nr::RunProductionNow). The module copies the SR output and the slot's depth and motion into its shared
// images, queues the NR list on the bridge's D3D12 queue and waits for it on this context (same-frame hand-off), then hands back the NR
// output. Anything else (the model is still being built, no list free, the module is switched off or failed) returns an empty view and the
// alpha pass keeps the SR output: counted by the module, logged here once per cause.
constexpr unsigned kProductionCauseDeclined = 8; // above every RunOutcome value
const char* ProductionCauseText(nr::RunOutcome outcome)
{
    switch (outcome)
    {
    case nr::RunOutcome::Preparing: return "the model of this preview is being built (the first renders after a window opens)";
    case nr::RunOutcome::NotAvailable: return "preview NR is not available (bridge, graphics card, files or a latch)";
    case nr::RunOutcome::Skipped: return "no stream, command list or allocator was free, or the size is changing";
    case nr::RunOutcome::Failed: return "the run failed (preview NR is switched off for this session)";
    default: return "";
    }
}

ComPtr<ID3D11ShaderResourceView> RunNrProductionInner(const DrawView& view, const SlotGpu& g, const SlotState& s, uint32_t w, uint32_t h)
{
    const uint32_t outW = w * ngx::kOutputScale, outH = h * ngx::kOutputScale;
    if (view.rtvTexture == nullptr || outW > kNrMaxSide || outH > kNrMaxSide)
    {
        // Not offered to NR: counted like any other fallback and logged once (a cause outside the RunOutcome range).
        nr::NoteProductionDeclined();
        if (g_productionCauses.First(kProductionCauseDeclined))
            LOG_INFO("CharaView NR: production NR kept the super-resolution output of a render: the preview texture is unknown or its size ({}x{}) is "
                     "larger than NR takes (counted; this cause is logged once)",
                     outW, outH);
        return nullptr;
    }
    nr::ProductionRequest request;
    request.base.context = view.context;
    request.base.native = view.rtvTexture;
    request.base.width = outW;
    request.base.height = outH;
    request.base.now = view.now;
    request.colour = g.output.texture.Get();
    request.depth = g.depth.texture.Get();
    request.motion = g.motion.texture.Get();
    request.guides = nr::MakeProductionGuides(w, h, s.reversedZ); // what the SR feature of this slot was created with
    nr::ProductionResult result = nr::RunProduction(request);
    if (result.outcome == nr::RunOutcome::Ran && result.view != nullptr)
        return std::move(result.view);
    if (g_productionCauses.First(static_cast<unsigned>(result.outcome)))
        LOG_INFO("CharaView NR: production NR kept the super-resolution output of a render: {} (counted; this cause is logged once)",
                 ProductionCauseText(result.outcome));
    if (diag::TraceEnabled())
        diag::Trace(diag::TraceCategory::Finish, view.now, std::format("NR production outcome {} for {}x{}", static_cast<int>(result.outcome), outW, outH));
    return nullptr;
}

// The second line of defence behind the module's own boundary (nr::RunProduction never throws): the request set-up and the log text
// around it cannot leave the game's Draw hook either. An exception here latches preview NR; the SR output is used.
ComPtr<ID3D11ShaderResourceView> RunNrProduction(const DrawView& view, const SlotGpu& g, const SlotState& s, uint32_t w, uint32_t h)
{
    return nr::Guarded([&] { return RunNrProductionInner(view, g, s, w, h); },
                       []() -> ComPtr<ID3D11ShaderResourceView>
                       {
                           if (nr::SharedLatches().LatchNr())
                               LOG_WARN("CharaView NR: an unexpected exception in the dispatcher's production step; switched off for this session, the super-resolution output is used");
                           return nullptr;
                       });
}

// True when `key` is one of the preview renderer's output textures (table entries 0..7): only those can be replaced by an NR result.
bool IsPreviewOutput(const DrawView& view, uintptr_t key)
{
    if (view.table == nullptr)
        return false;
    const int entry = table::IndexOfPointer(*view.table, key);
    return entry >= 0 && entry <= 7;
}

void Finish(const DrawView& view)
{
    ID3D11Texture2D* texture = view.rtvTexture;
    if (texture == nullptr)
        return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(texture);
    g_slots.NoteRecent(key, view.now, g_pass.counter);

    const int index = g_slots.Find(key);
    g_slots.NoteFinishedRender(index); // an accepted finisher: the render counts for this preview's jitter phase
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
    ++g_stats.finishVerdict[static_cast<size_t>(verdict)];
    if (diag::TraceEnabled() && verdict != FinishVerdict::Proceed)
        diag::Trace(diag::TraceCategory::Finish, view.now,
                    std::format("finish verdict {} for pass {}x{} (slot {}, replayed {}, camera {})", static_cast<int>(verdict),
                                g_pass.width, g_pass.height, index, g_pass.replayed, g_pass.cameraCaptured));
    if (verdict != FinishVerdict::Proceed && g_lines.ShouldLog(static_cast<size_t>(verdict), identify::BurstNumber()))
    {
        LOG_INFO("CharaView DLSS: finish step stopped: {} (pass {}, {}x{}, slot {}, motion replay {}, camera copy {}, depth texture {}, "
                 "depth view {})",
                 IsWaiting(verdict) ? WaitingText(verdict) : FinishVerdictName(verdict), g_pass.counter, g_pass.width, g_pass.height,
                 index == kNoSlot ? std::string("none") : std::to_string(index), g_pass.replayed ? "done" : "missing",
                 g_pass.cameraCaptured ? "done" : "missing", g_depthTexture != nullptr ? "known" : "missing",
                 g_depthSrv != nullptr ? "known" : "missing");
    }
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
        return;
    }
    case FinishVerdict::Rebuild:
        // The previous evaluate may still be running: its NGX feature is freed only after the slot's query reports done. Until then this
        // render keeps the native image and the next one asks again.
        if (!g_slots.ReadyForRelease(index, SlotQueryDone))
        {
            g_slots.ClearEvaluated(index);
            CountFallback();
            return;
        }
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
// PreviewDebugTint for a bring-up NR result: the cyan copy of the result (CharaNrTint.h), made once per result. Null: the tint could not be
// made (the helper passes or the cyan shader are missing, a resource failed); the caller gives the untinted result and counts nothing.
ComPtr<ID3D11ShaderResourceView> TintBringUpResult(const DrawView& view, uintptr_t key, ID3D11ShaderResourceView* result, uint64_t serial)
{
    // PreviewNR can run without PreviewDLSS: the helper passes are then created here, and their failure does not touch the SR latch.
    if (!EnsureInit(view.context, false) || g_device == nullptr)
    {
        if (!g_nrTintFailureLogged)
        {
            g_nrTintFailureLogged = true;
            LOG_WARN("CharaView NR: the debug tint of the NR result is not available (the helper passes could not be created)");
        }
        return nullptr;
    }
    ReplayScope own; // the tint pass draws on this context: the draw hooks must not treat it as the game's work
    bool drawn = false;
    ComPtr<ID3D11ShaderResourceView> tinted = g_nrTint.Tinted(g_passes, g_device.Get(), view.context, key, result, serial, view.now, &drawn);
    if (tinted == nullptr)
    {
        if (!g_nrTintFailureLogged)
        {
            g_nrTintFailureLogged = true;
            LOG_WARN("CharaView NR: the debug tint of the NR result could not be made (the cyan shader or the target image); the untinted result is shown");
        }
        return nullptr;
    }
    if (drawn)
        ++g_stats.tintApplied;
    return tinted;
}

// A UI draw that samples a preview image: the composed image replaces it for this one draw.
bool Consume(const DrawView& view, DrawCall draw, bool sr, bool nrOn)
{
    ID3D11DeviceContext* context = view.context;
    ID3D11ShaderResourceView* original[kSrvSlots] {};
    context->PSGetShaderResources(0, kSrvSlots, original);

    ID3D11ShaderResourceView* replaced[kSrvSlots] {};
    bool any = false;
    bool flooded = false; // at least one slot got the debug magenta image (PreviewDebugFlood)
    bool anyNr = false;   // at least one slot got the bring-up NR result
    bool anyProduction = false; // at least one slot got a composed image made from the production NR output
    ComPtr<ID3D11ShaderResourceView> nrHold[kSrvSlots]; // the NR results in use (also the cyan copies), alive until after the draw and the restore
    const bool floodOn = flood::On();
    const bool tintOn = ext::Cfg().ffxiv.chara.previewDebugTint.value_or_default();
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
        const bool previewOutput = nrOn && IsPreviewOutput(view, key); // only NR cares which images are preview outputs
        if (previewOutput)
            g_consumed.Record(key, view.now);
        int index = sr ? g_slots.Find(key) : kNoSlot;
        if (sr && index == kNoSlot && g_slots.RecentContains(key, view.now))
        {
            ComPtr<ID3D11Texture2D> texture;
            D3D11_TEXTURE2D_DESC td {};
            if (SUCCEEDED(resource.As(&texture)))
            {
                texture->GetDesc(&td);
                index = g_slots.AcquireForConsumer(key, view.now, td.Width, td.Height);
                if (index != kNoSlot)
                {
                    UpdateCounts();
                    ++g_stats.slotsAcquired;
                    if (g_lines.ShouldLog(kLineSlotAcquired, identify::BurstNumber()))
                        LOG_INFO("CharaView DLSS: preview slot {} acquired by a UI draw that samples the preview texture {}x{} (the next "
                                 "pass of that preview is followed)",
                                 index, td.Width, td.Height);
                }
            }
        }

        // The SR result of this image, if it has a slot that evaluated. An image without a slot has none.
        bool srReady = false;
        const SlotGpu* g = nullptr;
        if (index != kNoSlot)
        {
            g_slots.Touch(index, view.now);
            const SlotState& s = g_slots.Slot(index);
            g = &g_gpu[static_cast<size_t>(index)];
            if (floodOn && flood::CanReplace(sd))
            {
                // The flood test bypasses the "evaluated and not failed" condition: any image that has a slot gets the magenta one.
                if (ID3D11ShaderResourceView* magenta = flood::View(context, sd.Format))
                {
                    replaced[i] = magenta;
                    flooded = true;
                    continue;
                }
            }
            srReady = s.everEvaluated && !s.failed && g->composed.srv != nullptr;
        }

        // One decision for both cases (with and without an SR slot): the preview NR result (native size, original alpha) replaces the image
        // when the stream has a current one and the SR result is not there. The flood test wins (the dispatcher applies it to a draw this
        // function leaves alone). The result is held with its own reference until the draw has been issued.
        nr::ResultView nrResult;
        if (nr::WantNrResult(nrOn, floodOn, srReady, previewOutput))
            nrResult = nr::ResultFor(context, key, view.now);
        // The slot's composed image was made from the production NR output (written by the alpha pass of that render).
        const bool productionReady = srReady && g->composedFromNr;
        const nr::ConsumerSource source = nr::ChooseConsumerSource(floodOn, productionReady, srReady, nrResult.view != nullptr);
        switch (source)
        {
        case nr::ConsumerSource::ProductionResult: // the SR image made from the NR output: the alpha pass already tinted it (cyan)
            replaced[i] = g->composed.srv.Get();
            any = true;
            anyProduction = true;
            break;
        case nr::ConsumerSource::SrResult: // SR wins over the bring-up NR result
            replaced[i] = g->composed.srv.Get();
            any = true;
            break;
        case nr::ConsumerSource::NrResult:
            nrHold[i] = std::move(nrResult.view);
            replaced[i] = nrHold[i].Get();
            // PreviewDebugTint: the bring-up result is a finished image, so its cyan tint is an extra pass (once per result).
            if (tintOn)
                if (ComPtr<ID3D11ShaderResourceView> tinted = TintBringUpResult(view, key, nrHold[i].Get(), nrResult.serial))
                {
                    nrHold[i] = std::move(tinted);
                    replaced[i] = nrHold[i].Get();
                }
            anyNr = true;
            break;
        default: break;
        }
    }

    bool handled = false;
    if (any || anyNr || flooded)
    {
        // A flooded draw is counted as a flood swap only: it did not get the composed image.
        if (flooded)
            ++g_stats.floodSwaps;
        else if (!any)
        {
            ++g_stats.nrSwaps;
            if (g_lines.ShouldLog(kLineNrSwap, identify::BurstNumber()))
                LOG_INFO("CharaView NR: a UI draw is given the preview NR result (NR swaps so far {})", g_stats.nrSwaps.load());
        }
        else
        {
            ++g_stats.consumerSwaps;
            if (anyProduction)
            {
                ++g_stats.nrProductionSwaps;
                if (g_lines.ShouldLog(kLineProductionSwap, identify::BurstNumber()))
                    LOG_INFO("CharaView NR: a UI draw is given the 2x preview image made from the production NR output (production swaps so far {})",
                             g_stats.nrProductionSwaps.load());
            }
            if (g_lines.ShouldLog(kLineSwap, identify::BurstNumber()))
                LOG_INFO("CharaView DLSS: a UI draw is given the 2x preview image (consumer swaps so far {})", g_stats.consumerSwaps.load());
            if (diag::TraceEnabled())
                diag::Trace(diag::TraceCategory::Consumer, view.now, "a UI draw got the composed preview image");
        }

        // The probe traces the first slot that was replaced (the image can be bound in a slot other than t0).
        UINT tracedSlot = 0;
        for (UINT i = 0; i < kSrvSlots; ++i)
        {
            if (replaced[i] != original[i])
            {
                tracedSlot = i;
                break;
            }
        }
        probe::SwapTrace trace(context,
                               flooded          ? probe::SwapKind::Flood
                               : anyProduction  ? probe::SwapKind::NrSr
                               : any            ? probe::SwapKind::Dlss
                                                : probe::SwapKind::Nr,
                               tracedSlot, replaced[tracedSlot]);
        context->PSSetShaderResources(0, kSrvSlots, replaced);
        trace.Applied();
        draw();
        context->PSSetShaderResources(0, kSrvSlots, original);
        trace.Restored();
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
    diag::Pump(g_context, now);
    if (diag::TraceEnabled())
    {
        static uint64_t lastSummary = 0;
        if (now >= lastSummary + 2000)
        {
            lastSummary = now;
            diag::Trace(diag::TraceCategory::Replay, now,
                        std::format("replays {}, skipped: open query {}, UAV {}, stream-out {}, unknown shader {}, no target {}, own draw {}",
                                    motion::ReplayCount(), motion::SkipCount(motion::Skip::OpenQuery),
                                    motion::SkipCount(motion::Skip::UnorderedAccess), motion::SkipCount(motion::Skip::StreamOut),
                                    motion::SkipCount(motion::Skip::UnknownShader), motion::SkipCount(motion::Skip::NoTarget),
                                    motion::SkipCount(motion::Skip::OwnDraw)));
        }
    }
    g_slots.Cleanup(
        now, disabled, [](int i) { return SlotQueryDone(i); }, [](int i) { ReleaseSlotGpu(i); });
    g_nrTint.Release(now, kNrTintUnusedMs); // cyan copies of bring-up results nobody asked for lately
    UpdateCounts();
}
} // namespace

bool Implemented() { return true; }

namespace
{
// The plumbing facts after the SR gate was refreshed when it is due (the gate is only looked at while SR is wanted and not latched).
nr::PlumbingFacts RefreshedFacts()
{
    const uint64_t now = GetTickCount64();
    const bool srWanted = ext::Cfg().ffxiv.chara.previewDlss.value_or_default();
    if (srWanted && !SrLatched() && now >= g_nextGateCheck.load(std::memory_order_relaxed))
        Refresh(now);
    const nr::PlumbingFacts facts = PlumbingNow();
    NoteQueryHooks(nr::SrHooksNeeded(facts));
    return facts;
}
} // namespace

bool Active()
{
    // "Plumbing alive": true while the SR path or preview NR may run, and while any GPU object of either still exists (a latched
    // or switched-off path is drained by the maintenance calls). An SR latch or a closed SR gate does not stop NR and the reverse.
    return nr::PlumbingActive(RefreshedFacts());
}

bool CanRun() { return nr::PathCanRun(RefreshedFacts()); }

bool NrCanRun() { return nr::NrUsable(PlumbingNow()); }

bool SrCanRun() { return nr::SrUsable(PlumbingNow()); }

bool SrAvailable()
{
    // Runs for every copy, clear, query and vertex shader creation of the game: the cheap conditions first (see SrHooksNeededLazy).
    const bool on = nr::SrHooksNeededLazy(
        g_slotsHeld.load(std::memory_order_relaxed), ext::Cfg().ffxiv.chara.previewDlss.value_or_default(), [] { return SrLatched(); },
        [] { return static_cast<Availability>(g_availability.load(std::memory_order_relaxed)) == Availability::Available; });
    NoteQueryHooks(on);
    return on;
}

bool HoldsGpuObjects() { return g_slotsHeld.load(std::memory_order_relaxed) != 0; }

void Probe(const DrawView& view)
{
    if (!SrLatched())
        CheckDevice(view);
}

void OnNgxInitialised(ID3D11Device* device)
{
    g_ngxRaw.store(reinterpret_cast<uintptr_t>(device), std::memory_order_relaxed);
    g_ngxNative.store(NativeDeviceOf(device), std::memory_order_relaxed);
    g_ngxGeneration.fetch_add(1, std::memory_order_release);
    g_nextGateCheck.store(0, std::memory_order_relaxed);
}

void OnNgxShutdown()
{
    g_ngxRaw.store(0, std::memory_order_relaxed);
    g_ngxNative.store(0, std::memory_order_relaxed);
    g_ngxGeneration.fetch_add(1, std::memory_order_release);
    g_deviceState.store(0, std::memory_order_relaxed);
    g_nextGateCheck.store(0, std::memory_order_relaxed);
}

bool Around(const DrawView& view, DrawCall draw)
{
    ID3D11DeviceContext* context = view.context;
    // The SR gate (NGX device comparison, availability) only matters while the SR path is wanted and not latched.
    if (ext::Cfg().ffxiv.chara.previewDlss.value_or_default() && !SrLatched())
    {
        CheckDevice(view);
        if (view.now >= g_nextGateCheck.load(std::memory_order_relaxed))
            Refresh(view.now);
    }
    // The two paths are independent: either may run alone (nr::PlumbingFacts / CharaNrPlumbing.h).
    nr::PlumbingFacts facts = PlumbingNow();
    bool sr = nr::SrUsable(facts);
    const bool nrOn = nr::NrUsable(facts);
    if (view.kind == DrawKind::Ignore || (!sr && !nrOn))
        return false;
    if (sr && !EnsureInit(context))
    {
        sr = false; // EnsureInit latched the SR path; NR carries on
        if (!nrOn)
            return false;
    }
    g_context = context;

    if (sr && (view.kind == DrawKind::Prepass || view.kind == DrawKind::GBuffer) &&
        (view.passStarted || g_pass.counter != (view.pass != nullptr ? view.pass->counter : 0)))
        BeginPass(view);

    switch (view.kind)
    {
    case DrawKind::Prepass:
    {
        // A render of another size than the running pass is neither jittered nor replayed (the guides and the motion target belong to
        // the pass's size).
        if (!sr || !g_pass.live || !GeometryFollowsPass(g_pass.width, g_pass.height, view.facts.targetWidth, view.facts.targetHeight))
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
        if (!sr || !g_pass.live || !GeometryFollowsPass(g_pass.width, g_pass.height, view.facts.targetWidth, view.facts.targetHeight))
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
        g_nrProductionThisRender = false; // set by RunSlot when production NR improved this render's SR output
        // The super-resolution step of this render. Its result wins at the consumer, so preview NR (below) is not run for a render
        // that SR evaluated: its result would be thrown away.
        bool srResultWins = false;
        if (sr)
        {
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
            }
            else
            {
                Finish(view);
                const int slot = view.rtvTexture != nullptr ? g_slots.Find(reinterpret_cast<uintptr_t>(view.rtvTexture)) : kNoSlot;
                srResultWins = slot != kNoSlot && g_slots.EvaluatedInPass(slot, g_pass.counter);
            }
        }
        // Preview NR: bring-up entry. It needs only the native image, not the SR guides, so it does not wait for them and does not
        // care whether the SR path is latched or failed. A live window only: a UI draw must have sampled this preview image lately
        // (the first render after opening is native). It only reads the native image, so running it after the SR step changes nothing.
        const bool live = view.rtvTexture != nullptr &&
                          g_consumed.Recent(reinterpret_cast<uintptr_t>(view.rtvTexture), view.now, kNrLiveWindowMs);
        bool bringUpProduced = false;
        if (nr::RunBringUpNow(facts, view.finisherAccepted, live, srResultWins))
            bringUpProduced = RunNrBringUp(view);
        // Production NR ran inside the SR step above: it counts as a result for this render, or the withdrawal below would mark it stale at once.
        const bool nrProducedResult = nr::NrProducedForRender(bringUpProduced, g_nrProductionThisRender);
        // A render that has no NR result (rejected finisher, nobody shows the window, the SR result won, a skipped or failed bring-up)
        // withdraws the older result of this image: the UI draw must not show an older render for up to a quarter of a second.
        if (nr::InvalidateNow(facts, nrProducedResult) && view.rtvTexture != nullptr)
            nr::Invalidate(reinterpret_cast<uintptr_t>(view.rtvTexture));
        return true;
    }
    case DrawKind::Consumer: return Consume(view, draw, sr, nrOn);
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
    const bool disabled = SrLatched() || !ext::Cfg().ffxiv.chara.previewDlss.value_or_default();
    Cleanup(GetTickCount64(), disabled);
}

void Maintain(uint64_t nowMs)
{
    motion::OnMaintain(nowMs); // lifts the open-query gate when no frame callback exists
    const bool disabled = SrLatched() || !ext::Cfg().ffxiv.chara.previewDlss.value_or_default();
    Cleanup(nowMs, disabled);
}

void Detach()
{
    // The hooks are going away and the device is still valid: release everything now (the GPU work of the last frames
    // is finished by the time the device is destroyed).
    for (size_t i = 0; i < kMaxSlots; ++i)
        ReleaseSlotGpu(static_cast<int>(i));
    g_slots.Clear();
    g_nrTint.Clear();
    g_passes.Release();
    g_passesTried = false;
    motion::Release();
    camera::Release();
    diag::Release();
    g_depthSrv.Reset();
    g_depthTexture.Reset();
    g_depthMemo.Reset();
    g_depthConvention.Reset();
    g_reversedZ = false;
    g_pass = PassData {};
    g_context = nullptr;
    g_device.Reset();
    g_deviceCache.Reset();
    g_deviceState.store(0, std::memory_order_relaxed);
    (void)g_queryHooks.Changed(false);
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

ID3D11Texture2D* ComposedTexture()
{
    const int index = g_lastEvaluatedSlot;
    if (index < 0 || index >= static_cast<int>(kMaxSlots))
        return nullptr;
    const SlotState& s = g_slots.Slot(index);
    if (!s.Used() || !s.everEvaluated || s.failed)
        return nullptr;
    return g_gpu[static_cast<size_t>(index)].composed.texture.Get();
}

bool ComposedFromNr()
{
    const int index = g_lastEvaluatedSlot;
    if (ComposedTexture() == nullptr)
        return false;
    return g_gpu[static_cast<size_t>(index)].composedFromNr;
}

const char* StatusNote()
{
    // The gate is only refreshed while the dispatcher asks: with the feature off there is nothing to say.
    if (!ext::Cfg().ffxiv.chara.previewDlss.value_or_default())
        return "";
    if (SrLatched())
        return ReasonText(Availability::Latched);
    const auto a = static_cast<Availability>(g_availability.load(std::memory_order_relaxed));
    return ReasonText(a);
}
} // namespace ext::ffxiv::chara::dlss
