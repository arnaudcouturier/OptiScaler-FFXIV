#pragma once
// Seams of the frame-generation group (SEAMS-catalog groups `fg` and `streamline`), included by ext/Seams.h.
// Per-object state: ext::fg::Dx12Ext (IFGFeature_Dx12::_ext), ext/fg/Dx12Ext.h. Implementations: ext/fg/*.cpp.

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

class IFGFeature_Dx12;

// ---------------------------------------------------------------------------------------------------------------
// Streamline (D1, D4). Implementation: ext/fg/streamline/*.cpp. Hosts: proxies/Streamline_Proxy.h (LoadStreamline),
// hooks/Streamline_Hooks.cpp (hkslDLSSGSetOptions, hkslDLSSGGetState), hooks/LibraryLoad_Hooks.cpp (LoadLibraryCheckW).
namespace ext::streamline
{
// D4: first statement of StreamlineProxy::LoadStreamline: Ampere MFG companion setup (guarded inside, runs on every call;
// must precede the Streamline module load and the `_dll != nullptr` early-out).
void OnLoadStreamline();
// D1: apply the nvngx_dlssg.dll patches when the provider is loaded (cheap flag check when the unlock is off).
void MfgUnlockTick();
// D1: the populate-once condition of the cached Streamline frame-count ceiling (State::dlssgMfgMax): true while it is
// unset or an unlock raised the ceiling after it was cached, and the answer is not provisional any more.
bool MfgCeilingStale(const std::optional<int>& cached);
// D1: raises `numFramesToGenerateMax` to the unlocked ceiling when a Streamline wrapper reports a lower one. Returns the
// (possibly raised) value. Must stay inside the struct-version-2+ branch of hkslDLSSGGetState.
uint32_t RaiseMfgCeiling(uint32_t& numFramesToGenerateMax);
// D1: LoadLibraryCheckW, nvngx_dlssg.dll branch. True = handled: `module` is what LoadLibraryCheckW returns (nullptr when
// the load failed). The provider is loaded through the loader-level entry and patched before NGX caches capabilities.
bool OnDlssgModuleLoad(const std::wstring& normalizedPath, LPCWSTR fullPath, HMODULE& module);
} // namespace ext::streamline

namespace ext::fg
{
// --- C6 (Dx12Sc.cpp)
// DLSSG_Dx12, XeFG_Dx12, FSRFG_Dx12 `Present`: close and submit the SC list in order (monotonic completion fence).
// False = the submission failed (further SC rendering is off): `return false` from Present.
bool SubmitSC(IFGFeature_Dx12* fg, UINT index);
// IFGFeature_Dx12::GetSCCommandList, after the stale lists were discarded: leaves the list at `index` open and ready
// (failed-submission and null guards, allocator fence wait, resets). False -> return nullptr.
bool PrepareSC(IFGFeature_Dx12* fg, UINT index);

// D1: DLSSG_Common::QueryState, the usable maximum: max(driver ceiling, unlocked ceiling).
int AdjustMfgMax(int driverMax);
} // namespace ext::fg
