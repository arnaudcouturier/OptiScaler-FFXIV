#pragma once
// DLSS-G render preset: the process-wide request state and the driver-settings seam (nvapi/NvApiHooks.cpp). The pure model is
// PresetRequest.h; this header is the part that needs the NVIDIA API types.

#include "ext/fg/dlssg/PresetRequest.h"

#include <intrin.h> // _ReturnAddress for the seam in NvApiHooks.cpp
#include <nvapi.h>

namespace ext::fg::preset
{

// NvApiHooks.cpp hkNvAPI_DRS_GetSetting, right after the original call. `status` is the original result; returns the status
// to hand back to the caller. Only the DLSS-G preset ids are looked at. An explicit request (A/B/C) turns a found or a "setting
// not found" reply into "found, DWORD, <request>"; Auto changes nothing. Always records what the driver said (the baseline Auto
// restores later) and that the runtime asked. `caller` = _ReturnAddress() of the hook, used to identify the runtime module.
NvAPI_Status OnDrsGetSetting(NvU32 settingId, NVDRS_SETTING* setting, NvAPI_Status status, void* caller);
} // namespace ext::fg::preset
