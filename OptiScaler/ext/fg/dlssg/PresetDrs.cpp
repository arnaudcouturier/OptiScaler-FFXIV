#include "pch.h"

#include "PresetDrs.h"

#include "ext/core/ExtConfig.h"

#include <atomic>

namespace ext::fg::preset
{
RequestState& Requests()
{
    static RequestState state;
    state.InitOnce(ext::Cfg().fg.renderPreset.value_or_default()); // first call wins
    return state;
}

NvAPI_Status OnDrsGetSetting(NvU32 settingId, NVDRS_SETTING* setting, NvAPI_Status status, void* caller)
{
    (void) caller; // the provider module is identified from it (PresetModule.cpp)

    if ((settingId != kRenderPresetSettingId && settingId != kOverrideSettingId) || setting == nullptr)
        return status;

    // Other errors (invalid handle, API not initialised, ...) are not an answer about the setting: leave them alone.
    const bool found = status == NVAPI_OK;
    if (!found && status != NVAPI_SETTING_NOT_FOUND)
        return status;

    const bool isNumber = found && setting->settingType == NVDRS_DWORD_TYPE;
    const auto edit = HandleDriverReply(Requests(), settingId, found, isNumber, found ? setting->u32CurrentValue : 0);
    if (!edit.replace)
        return status;

    // A reply that holds a string or binary value is not ours to rewrite.
    if (found && !isNumber)
        return status;

    setting->settingId = settingId;
    setting->settingType = NVDRS_DWORD_TYPE;
    setting->settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
    setting->isCurrentPredefined = 0;
    setting->u32CurrentValue = edit.value;

    if (settingId == kRenderPresetSettingId)
    {
        static std::atomic<int> reported { -1 };
        const int request = Requests().Active();
        if (reported.exchange(request) != request)
            LOG_INFO("DLSS-G render preset: the runtime asked the driver for its preset, answered {} ({}){}", edit.value,
                     Name(request), found ? "" : " (the driver profile has no such setting)");
    }
    return NVAPI_OK;
}
} // namespace ext::fg::preset
