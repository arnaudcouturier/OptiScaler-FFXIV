#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/reshade/ReShadeAddon.h"

#include <atomic>

namespace ext::reshade
{
namespace
{
// Heap-allocated and never destroyed: nothing runs at DLL_PROCESS_DETACH (loader lock), and callbacks that ReShade may
// still deliver while the process shuts down only ever read these atomics.
struct Global
{
    std::atomic<bool> attempted { false };
    std::atomic<bool> registered { false };
    std::atomic<uint8_t> reason { static_cast<uint8_t>(RegisterStatus::NoModule) };
};

Global& G()
{
    static Global* g = new Global;
    return *g;
}
} // namespace

bool ProbeEnabled() { return ext::Cfg().reshade.probe.value_or_default(); }

AddonStatus Status()
{
    auto& g = G();
    AddonStatus s;
    s.attempted = g.attempted.load(std::memory_order_acquire);
    s.registered = g.registered.load(std::memory_order_acquire);
    s.reason = static_cast<RegisterStatus>(g.reason.load(std::memory_order_acquire));
    s.probeEnabled = ProbeEnabled();
    return s;
}

void OnReshadeLoaded(HMODULE reshadeModule)
{
    auto& g = G();
    if (g.attempted.exchange(true))
        return;

    RegisterResult result;
    try
    {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&OnReshadeLoaded),
                               &self))
        {
            result = RegisterAddon(self, reshadeModule);

            // ReShade may FreeLibrary a module it registered when it shuts down (unverified); our callbacks must stay
            // mapped for the whole process lifetime. The proxy DLL lives that long anyway.
            if (result.status == RegisterStatus::Ok)
            {
                HMODULE pinned = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(&OnReshadeLoaded), &pinned);
            }
        }
        else
        {
            result.status = RegisterStatus::NoSelf;
        }
    }
    catch (...)
    {
        result.status = RegisterStatus::Exception;
    }

    g.reason.store(static_cast<uint8_t>(result.status), std::memory_order_release);
    g.registered.store(result.status == RegisterStatus::Ok, std::memory_order_release);

    // The one log line (docs/HUDLESS.md section 16 / BRIEF-s10-d0): nothing else happens with diagnostics off.
    if (result.status == RegisterStatus::Ok)
        LOG_INFO("ReShade built-in add-on: registered (API {})", kApiVersion);
    else if (result.status == RegisterStatus::ExportsMissing && result.missingExport != nullptr)
        LOG_INFO("ReShade built-in add-on: inert ({}; missing export {})", Describe(result.status),
                 result.missingExport);
    else
        LOG_INFO("ReShade built-in add-on: inert ({})", Describe(result.status));
}
} // namespace ext::reshade
