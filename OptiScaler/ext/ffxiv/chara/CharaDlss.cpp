#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaDlssGate.h"

#include <Config.h>
#include <State.h>
#include <proxies/NVNGX_Proxy.h>

#include <atomic>
#include <mutex>

// The isolated DLSS path for character previews: entry points behind CharaDlssApi.h.
//
// Reconstructed from the behaviour of Nickle's depth-jitter-20261013 build (no source available), see
// docs/RECONSTRUCTION-NICKLE.md.
namespace ext::ffxiv::chara::dlss
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint64_t kGateRefreshMs = 100;

std::atomic<bool> g_latched { false };
std::atomic<uint32_t> g_slotsHeld { 0 }; // slots whose GPU objects still exist (a latched path drains them before it stops)
std::atomic<uint8_t> g_availability { static_cast<uint8_t>(Availability::ConfigOff) };
std::atomic<uint64_t> g_nextGateCheck { 0 };

// Device identity (draw thread). `g_contextDevice` is the IUnknown identity of the device behind the immediate context.
IUnknown* g_contextDevice = nullptr;
ComPtr<IUnknown> g_contextDeviceRef;
std::atomic<uint8_t> g_deviceState { 0 }; // 0 not compared yet, 1 same device, 2 different device
std::atomic<ID3D11Device*> g_checkedNgxDevice { nullptr }; // the NGX device the comparison was made against
uint64_t g_nextDeviceCheck = 0;

std::mutex g_logMutex;
ReasonLog g_reasonLog;

std::atomic<uint32_t> g_activeSlots { 0 };
std::atomic<uint64_t> g_evaluations { 0 };
std::atomic<uint64_t> g_fallbacks { 0 };

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
    g_contextDevice = a.Get();
    g_checkedNgxDevice.store(ngxDevice, std::memory_order_relaxed);
    g_deviceState.store(a.Get() == b.Get() ? 1 : 2, std::memory_order_relaxed);
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
    return static_cast<Availability>(g_availability.load(std::memory_order_relaxed)) == Availability::Available;
}

bool Around(const DrawView& view, DrawCall)
{
    // Spike: only the availability gate and the device check run; the native draw is never changed.
    CheckDevice(view.context, view.now);
    if (g_latched.load(std::memory_order_relaxed))
        return false;
    if (view.now >= g_nextGateCheck.load(std::memory_order_relaxed))
        Refresh(view.now);
    return false;
}

void OnCopy(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*, bool) {}
void OnClearRenderTarget(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]) {}
void OnQueryBegin(ID3D11DeviceContext*, ID3D11Asynchronous*) {}
void OnQueryEnd(ID3D11DeviceContext*, ID3D11Asynchronous*) {}
void OnVertexShaderCreated(ID3D11Device*, ID3D11VertexShader*, const void*, SIZE_T) {}
void OnFrameEnd() {}
void Maintain(uint64_t) {}

void Detach()
{
    g_contextDeviceRef.Reset();
    g_contextDevice = nullptr;
    g_deviceState.store(0, std::memory_order_relaxed);
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
    if (g_latched.load(std::memory_order_relaxed))
        return ReasonText(Availability::Latched);
    const auto a = static_cast<Availability>(g_availability.load(std::memory_order_relaxed));
    return ReasonText(a);
}
} // namespace ext::ffxiv::chara::dlss
