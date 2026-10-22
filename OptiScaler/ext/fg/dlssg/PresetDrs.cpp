#include "pch.h"

#include "PresetDrs.h"

#include "ext/fg/dlssg/PresetLogRelay.h"
#include "ext/fg/dlssg/PresetModule.h"
#include "ext/fg/dlssg/PresetStatus.h"

#include <atomic>

namespace ext::fg::preset
{
NvAPI_Status OnDrsGetSetting(NvU32 settingId, NVDRS_SETTING* setting, NvAPI_Status status, void* caller)
{
    // Only the preset id is looked at. The DLSS override id (kOverrideSettingId) is never answered, so it does not get into the decision
    // code at all and passes through to the driver untouched.
    if (settingId != kRenderPresetSettingId || setting == nullptr)
        return status;

    // Other errors (invalid handle, API not initialised, ...) are not an answer about the setting: leave them alone.
    const bool found = status == NVAPI_OK;
    if (!found && status != NVAPI_SETTING_NOT_FOUND)
        return status;

    const bool isNumber = found && setting->settingType == NVDRS_DWORD_TYPE;
    ReadoutState().NoteDriverQuery(true); // the runtime asked, whatever the answer
    module::ObserveDriverQuery(caller);   // possible provider of the live path

    const auto edit = HandleDriverReply(Requests(), settingId, found, isNumber, found ? setting->u32CurrentValue : 0,
                                       CurrentAvailability() == Unavailable::None);
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

    static std::atomic<int> reported { -1 };
    const int request = Requests().Active();
    if (reported.exchange(request) != request)
        LOG_INFO("DLSS-G render preset: the runtime asked the driver for its preset (id 0x{:X}), answered {} ({}){}; the DLSS "
                 "override id 0x{:X} is passed through to the driver",
                 kRenderPresetSettingId, edit.value, Name(request),
                 found ? "" : " (the driver profile has no such setting)", kOverrideSettingId);
    return NVAPI_OK;
}
} // namespace ext::fg::preset
