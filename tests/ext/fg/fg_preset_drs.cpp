// The driver-settings seam (ext/fg/dlssg/PresetDrs.cpp) against the REAL NVAPI types: which statuses and settings it touches and
// which it leaves alone. The request state, the readout, the provider tracking and the availability are replaced by recorders.
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetDrs.h"
#include "ext/fg/dlssg/PresetLogRelay.h"
#include "ext/fg/dlssg/PresetModule.h"
#include "ext/fg/dlssg/PresetStatus.h"
#include "ext/fg/dlssg/UiRecompositionRule.h"

#include <cstring>
#include <initializer_list>
#include <new>

using namespace ext::fg::preset;

// ---- stand-ins for the parts that need the product ---------------------------------------------------------------------------
namespace
{
RequestState g_state;
Readout g_readout;
int g_observed = 0;
Unavailable g_available = Unavailable::None;
AvailabilityLatch g_latch; // the real latch the rule reads (PresetSeams.cpp holds the product's one)
int g_remembered = 0;      // RememberAvailability calls
} // namespace

namespace ext::fg::preset
{
RequestState& Requests() { return g_state; }
Readout& ReadoutState() { return g_readout; }
Unavailable CurrentAvailability() { return g_available; }
void RememberAvailability(Unavailable live)
{
    ++g_remembered;
    g_latch.Resolve(live, true);
}
namespace module
{
void ObserveDriverQuery(void*) { ++g_observed; }
} // namespace module
} // namespace ext::fg::preset

namespace
{
void Reset(int request)
{
    g_state.~RequestState();
    new (&g_state) RequestState();
    g_state.InitOnce(request);
    g_readout.BeginRequest();
    g_observed = 0;
    g_available = Unavailable::None;
    g_remembered = 0;
    g_latch.~AvailabilityLatch();
    new (&g_latch) AvailabilityLatch();
}

NVDRS_SETTING Fresh()
{
    NVDRS_SETTING s {};
    s.version = NVDRS_SETTING_VER;
    s.settingId = 0xDEAD;
    s.settingType = NVDRS_STRING_TYPE; // garbage the call must not trust
    s.u32CurrentValue = 0x7777;
    return s;
}

NVDRS_SETTING Dword(uint32_t value)
{
    NVDRS_SETTING s = Fresh();
    s.settingType = NVDRS_DWORD_TYPE;
    s.u32CurrentValue = value;
    return s;
}

void OtherIdsAndNull()
{
    Reset(kPresetA);
    auto s = Dword(5);
    CHECK(OnDrsGetSetting(0x10308298u, &s, NVAPI_OK, nullptr) == NVAPI_OK && s.u32CurrentValue == 5u); // DLSSG mode id
    CHECK(OnDrsGetSetting(0x10E41DF3u, &s, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_SETTING_NOT_FOUND); // the SR preset id
    CHECK(OnDrsGetSetting(kRenderPresetSettingId, nullptr, NVAPI_OK, nullptr) == NVAPI_OK);
    CHECK(OnDrsGetSetting(kRenderPresetSettingId, nullptr, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_SETTING_NOT_FOUND);
    CHECK(g_observed == 0 && !g_state.QuerySeen());
}

void AutoLeavesEverythingAlone()
{
    Reset(kAuto);
    auto found = Dword(2);
    CHECK(OnDrsGetSetting(kRenderPresetSettingId, &found, NVAPI_OK, nullptr) == NVAPI_OK && found.u32CurrentValue == 2u);
    CHECK(g_state.Base().kind == Baseline::Kind::Value && g_state.Base().value == 2u); // but the reply is recorded
    CHECK(g_observed == 1 && g_readout.Snapshot().queried);

    Reset(kAuto);
    auto missing = Fresh();
    CHECK(OnDrsGetSetting(kRenderPresetSettingId, &missing, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_SETTING_NOT_FOUND);
    CHECK(missing.settingType == NVDRS_STRING_TYPE && missing.u32CurrentValue == 0x7777u); // untouched
    CHECK(g_state.Base().kind == Baseline::Kind::NotFound);
}

void ExplicitRequestBecomesTheAnswer()
{
    for (int request : { 1, 2, 3 })
    {
        Reset(request);
        auto found = Dword(2);
        CHECK(OnDrsGetSetting(kRenderPresetSettingId, &found, NVAPI_OK, nullptr) == NVAPI_OK);
        CHECK(found.settingType == NVDRS_DWORD_TYPE && found.u32CurrentValue == (uint32_t) request);
        CHECK(g_state.Base().kind == Baseline::Kind::Value && g_state.Base().value == 2u); // the original, not our answer

        Reset(request);
        auto missing = Fresh();
        CHECK(OnDrsGetSetting(kRenderPresetSettingId, &missing, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_OK); // NOT_FOUND becomes OK
        CHECK(missing.settingType == NVDRS_DWORD_TYPE && missing.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION);
        CHECK(missing.isCurrentPredefined == 0 && missing.u32CurrentValue == (uint32_t) request && missing.settingId == kRenderPresetSettingId);
        CHECK(missing.version == NVDRS_SETTING_VER); // the caller's version field is kept
        CHECK(g_state.Base().kind == Baseline::Kind::NotFound);
    }
}

void OtherErrorsAreNotAnAnswer()
{
    Reset(kPresetB);
    for (NvAPI_Status error : { NVAPI_INVALID_HANDLE, NVAPI_API_NOT_INITIALIZED, NVAPI_ERROR, NVAPI_INVALID_ARGUMENT })
    {
        auto s = Fresh();
        CHECK(OnDrsGetSetting(kRenderPresetSettingId, &s, error, nullptr) == error);
        CHECK(s.settingType == NVDRS_STRING_TYPE && s.u32CurrentValue == 0x7777u);
        CHECK(OnDrsGetSetting(kOverrideSettingId, &s, error, nullptr) == error);
    }
    CHECK(g_state.Base().kind == Baseline::Kind::Unknown && g_observed == 0); // nothing was learnt from them
}

void NonDwordRepliesAreLeftAlone()
{
    Reset(kPresetA);
    auto s = Fresh(); // OK, but a string setting
    s.settingType = NVDRS_WSTRING_TYPE;
    CHECK(OnDrsGetSetting(kRenderPresetSettingId, &s, NVAPI_OK, nullptr) == NVAPI_OK);
    CHECK(s.settingType == NVDRS_WSTRING_TYPE && s.u32CurrentValue == 0x7777u);
    CHECK(g_state.Base().kind == Baseline::Kind::Unknown);
}

void OverrideOnId()
{
    // PRESET-BOOT probe: the DLSS override id is never answered, whatever the request; the driver's reply goes back untouched
    for (int request : { 1, 2, 3 })
    {
        Reset(request);
        auto missing = Fresh();
        CHECK(OnDrsGetSetting(kOverrideSettingId, &missing, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_SETTING_NOT_FOUND);
        CHECK(missing.settingId == 0xDEADu && missing.settingType == NVDRS_STRING_TYPE && missing.u32CurrentValue == 0x7777u);
        auto off = Dword(0);
        CHECK(OnDrsGetSetting(kOverrideSettingId, &off, NVAPI_OK, nullptr) == NVAPI_OK && off.u32CurrentValue == 0u);
        auto on = Dword(1);
        CHECK(OnDrsGetSetting(kOverrideSettingId, &on, NVAPI_OK, nullptr) == NVAPI_OK && on.u32CurrentValue == 1u);
        CHECK(g_observed == 0 && g_state.Base().kind == Baseline::Kind::Unknown); // not the preset query
        // the preset id of the same request is still answered
        auto preset = Fresh();
        CHECK(OnDrsGetSetting(kRenderPresetSettingId, &preset, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_OK);
        CHECK(preset.u32CurrentValue == (uint32_t) request);
    }

    Reset(kAuto);
    auto autoMissing = Fresh();
    CHECK(OnDrsGetSetting(kOverrideSettingId, &autoMissing, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_SETTING_NOT_FOUND);
}

void UnavailableControlDoesNotAnswer()
{
    // L5: with the companion runtime, a replacement runtime or another output the control is off, so the request is not forced on
    // a runtime that is not NVIDIA's; the driver reply is still recorded
    for (auto why : { Unavailable::AmpereRuntime, Unavailable::ReplacementRuntime, Unavailable::OtherOutput })
    {
        Reset(kPresetC);
        g_available = why;
        auto found = Dword(2);
        CHECK(OnDrsGetSetting(kRenderPresetSettingId, &found, NVAPI_OK, nullptr) == NVAPI_OK && found.u32CurrentValue == 2u);
        auto missing = Fresh();
        CHECK(OnDrsGetSetting(kRenderPresetSettingId, &missing, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_SETTING_NOT_FOUND);
        auto on = Fresh();
        CHECK(OnDrsGetSetting(kOverrideSettingId, &on, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_SETTING_NOT_FOUND);
        CHECK(g_state.Base().kind == Baseline::Kind::Value && g_observed == 2);
    }
}

void FirstDriverQueryFixesTheAvailabilityForTheRule()
{
    // Boot with Preset C and the control available: the runtime asks the driver and is told C (it forces the recomposition off). The user then
    // ticks "Load RTX 20/30 runtime" before the first SetOptions (the checkbox writes at once): the live availability is now AmpereRuntime,
    // but the rule must keep describing the runtime that was told C, so it still drops the recomposition instead of asking for it.
    Reset(kPresetC);
    auto missing = Fresh();
    CHECK(OnDrsGetSetting(kRenderPresetSettingId, &missing, NVAPI_SETTING_NOT_FOUND, nullptr) == NVAPI_OK && missing.u32CurrentValue == 3u);
    CHECK(g_remembered == 1);
    g_available = Unavailable::AmpereRuntime;
    const auto atFirstSetOptions = g_latch.Resolve(g_available, true); // what AdjustUiRecomposition does
    CHECK(atFirstSetOptions == Unavailable::None);
    const auto decision = DecideUiRecomposition(true, atFirstSetOptions, g_state.Active());
    CHECK(!decision.requested && decision.droppedByPreset && decision.preset == kPresetC);

    // The other way round: the control was off at the first look (the runtime was never told C), then it becomes available: the rule keeps
    // reading Auto, which is what the runtime runs
    Reset(kPresetC);
    g_available = Unavailable::OtherOutput;
    auto found = Dword(2);
    CHECK(OnDrsGetSetting(kRenderPresetSettingId, &found, NVAPI_OK, nullptr) == NVAPI_OK && found.u32CurrentValue == 2u);
    g_available = Unavailable::None;
    CHECK(g_latch.Resolve(g_available, true) == Unavailable::OtherOutput);
    CHECK(DecideUiRecomposition(true, g_latch.Resolve(g_available, true), g_state.Active()).requested);

    // Only a real question about the preset id fixes it: other ids, errors and a null setting leave the latch empty (a later look still
    // reads the live value)
    Reset(kPresetC);
    auto s = Dword(0);
    OnDrsGetSetting(kOverrideSettingId, &s, NVAPI_OK, nullptr);
    OnDrsGetSetting(0x10308298u, &s, NVAPI_OK, nullptr);
    OnDrsGetSetting(kRenderPresetSettingId, nullptr, NVAPI_OK, nullptr);
    OnDrsGetSetting(kRenderPresetSettingId, &s, NVAPI_ERROR, nullptr);
    CHECK(g_remembered == 0);
    g_available = Unavailable::ReplacementRuntime;
    CHECK(g_latch.Resolve(g_available, false) == Unavailable::ReplacementRuntime);
}

void ProviderIsObservedForThePresetIdOnly()
{
    Reset(kAuto);
    auto s = Dword(0);
    OnDrsGetSetting(kRenderPresetSettingId, &s, NVAPI_OK, nullptr);
    OnDrsGetSetting(kOverrideSettingId, &s, NVAPI_OK, nullptr);
    OnDrsGetSetting(0x10308298u, &s, NVAPI_OK, nullptr);
    CHECK(g_observed == 1);
}
} // namespace

int main()
{
    OtherIdsAndNull();
    AutoLeavesEverythingAlone();
    ExplicitRequestBecomesTheAnswer();
    OtherErrorsAreNotAnAnswer();
    NonDwordRepliesAreLeftAlone();
    OverrideOnId();
    UnavailableControlDoesNotAnswer();
    FirstDriverQueryFixesTheAvailabilityForTheRule();
    ProviderIsObservedForThePresetIdOnly();
    if (g_failures)
    {
        std::printf("fg_preset_drs: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_drs: OnDrsGetSetting statuses, NOT_FOUND to OK, other errors untouched, Auto untouched, unavailable control passed");
    return 0;
}
