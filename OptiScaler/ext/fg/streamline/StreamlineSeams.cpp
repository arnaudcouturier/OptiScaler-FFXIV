#include "pch.h"

#include "ext/Seams.h"

#include <proxies/Ntdll_Proxy.h>

#include "ext/fg/dlssg/AmpereMfgLoader.h"
#include "ext/fg/dlssg/MfgUnlock.h"

// D1 seams on the Streamline hooks and the library loader; the patching itself is ext/fg/dlssg/MfgUnlock.cpp.

namespace ext::streamline
{
void OnLoadStreamline() { AmpereMfgLoader::TrySetup(); }

void MfgUnlockTick() { MfgUnlock::TryApply(); }

bool MfgCeilingStale(const std::optional<int>& cached)
{
    // Provisional until the snippet has been seen: nvngx_dlssg.dll can load after this runs, and the ceiling read before
    // it does is Ada's 1. Avoid caching that provisional answer. Capability discovery settles independently of watching
    // for later providers; a later successful unlock refreshes the cache.
    if (MfgUnlock::Pending())
        return false;

    return !cached.has_value() || static_cast<int>(MfgUnlock::UnlockedMax()) > cached.value();
}

uint32_t RaiseMfgCeiling(uint32_t& numFramesToGenerateMax)
{
    // A wrapper ahead of the snippet can answer a lower ceiling than the patched one.
    if (auto unlockedMax = MfgUnlock::UnlockedMax(); unlockedMax > numFramesToGenerateMax)
        numFramesToGenerateMax = unlockedMax;

    return numFramesToGenerateMax;
}

bool OnDlssgModuleLoad(const std::wstring& normalizedPath, LPCWSTR fullPath, HMODULE& module)
{
    // Optional Ada unlock before NGX caches capabilities.
    if (std::filesystem::path(normalizedPath).filename() != L"nvngx_dlssg.dll" || !MfgUnlock::Watching())
        return false;

    module = NtdllProxy::LoadLibraryExW_Ldr(fullPath, NULL, 0);
    if (module)
        MfgUnlock::TryApply(module);

    return true;
}
} // namespace ext::streamline

namespace ext::fg
{
int AdjustMfgMax(int driverMax) { return std::max(driverMax, static_cast<int>(MfgUnlock::UnlockedMax())); }
} // namespace ext::fg
