#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "Util.h"
#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeEvents.h"

#include <atomic>
#include <format>

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

// "6.8.0.2155" from the module's version resource (no version.dll: it may be the proxy we are running as).
std::string ModuleVersion(HMODULE module)
{
    try
    {
        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(1), RT_VERSION);
        if (res == nullptr)
            return {};
        HGLOBAL mem = LoadResource(module, res);
        const DWORD size = SizeofResource(module, res);
        if (mem == nullptr || size < sizeof(VS_FIXEDFILEINFO))
            return {};
        const auto* bytes = static_cast<const uint8_t*>(LockResource(mem));
        for (DWORD i = 0; bytes != nullptr && i + sizeof(VS_FIXEDFILEINFO) <= size && i < 256; i += 4)
        {
            const auto* info = reinterpret_cast<const VS_FIXEDFILEINFO*>(bytes + i);
            if (info->dwSignature == 0xFEEF04BD)
                return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        }
    }
    catch (...)
    {
    }
    return {};
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
    {
        LOG_INFO("ReShade built-in add-on: registered (API {})", kApiVersion);

        // Diagnostics only: subscribe to the runtime events; the report folder is created on its first line.
        if (ProbeEnabled())
        {
            try
            {
                std::wstring modulePath(MAX_PATH, L'\0');
                modulePath.resize(GetModuleFileNameW(result.reshade, modulePath.data(), MAX_PATH));
                const auto base = Util::ExePath().parent_path() / L"OptiScaler" / L"hudless-diag";
                const std::string line =
                    std::format("registration: OK (add-on API {}); ReShade module {:#x} version {} ({}); "
                                "diagnostics ON ([HudLess] ReShadeProbe)",
                                kApiVersion, reinterpret_cast<uintptr_t>(result.reshade),
                                ModuleVersion(result.reshade), wstring_to_string(modulePath));
                if (InstallProbe(base, line))
                    LOG_INFO("ReShade built-in add-on: diagnostics on, subscribed to the effect runtime events");
                else
                    LOG_WARN("ReShade built-in add-on: diagnostics requested but the event subscription failed");
            }
            catch (...)
            {
                LOG_WARN("ReShade built-in add-on: diagnostics requested but setup threw");
            }
        }
    }
    else if (result.status == RegisterStatus::ExportsMissing && result.missingExport != nullptr)
        LOG_INFO("ReShade built-in add-on: inert ({}; missing export {})", Describe(result.status),
                 result.missingExport);
    else
        LOG_INFO("ReShade built-in add-on: inert ({})", Describe(result.status));
}
} // namespace ext::reshade
