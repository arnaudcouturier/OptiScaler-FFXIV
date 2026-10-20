#pragma once
// DLSS-G render preset request (WS2, "[DLSSG] RenderPreset": 0 Auto, 1 A, 2 B, 3 C). Pure model, no Windows or NVIDIA
// headers, so tests/ext/fg/fg_preset_request.cpp builds it standalone.
//
// Two places answer for the preset setting 0x10E41DF1, both process-local; no NVIDIA profile is ever written:
//   * the driver-settings query (NvAPI_DRS_GetSetting, nvapi/NvApiHooks.cpp). This is what nvngx_dlssg.dll asks at
//     start-up while it fills its own settings cache, so it carries the first request (the restart path);
//   * the cached-settings reader inside nvngx_dlssg.dll (PresetModule.cpp), which every later endpoint reads. It is
//     detoured at the first live change and answers with the ACTIVE request (the live path).
// Auto means "as if this feature did not exist": the driver query is left alone, and the cached reader gives back
// what the driver answered the first time (a value, or "not found"), never the value of an earlier request.

#include <atomic>
#include <cstdint>

namespace ext::fg::preset
{
inline constexpr uint32_t kRenderPresetSettingId = 0x10E41DF1; // NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID
inline constexpr uint32_t kOverrideSettingId = 0x10E41E03;     // NGX_DLSS_FG_OVERRIDE_ID
inline constexpr uint32_t kOverrideOn = 1;                     // NGX_DLSS_FG_OVERRIDE_ON

inline constexpr int kAuto = 0;
inline constexpr int kPresetA = 1;
inline constexpr int kPresetB = 2;
inline constexpr int kPresetC = 3;

constexpr bool ValidRequest(int request) { return request >= kAuto && request <= kPresetC; }

constexpr const char* Name(int request)
{
    switch (request)
    {
    case kPresetA:
        return "Preset A";
    case kPresetB:
        return "Preset B";
    case kPresetC:
        return "Preset C";
    default:
        return "Auto";
    }
}

// What the driver said the first time the preset was asked for.
struct Baseline
{
    enum class Kind : uint8_t
    {
        Unknown,  // nobody asked yet
        NotFound, // the driver profile has no such setting
        Value
    };
    Kind kind = Kind::Unknown;
    uint32_t value = 0;
};

struct Answer
{
    bool handled = false; // false: leave the original answer alone
    bool found = false;   // handled && !found: answer "setting not found"
    uint32_t value = 0;
};

// Restart path. Only an explicit A/B/C request changes the driver's answer: the preset id gets the number, the
// override id gets "on". Auto, an invalid request and every other id are left alone.
constexpr Answer AnswerDriverQuery(uint32_t settingId, int request)
{
    if (request < kPresetA || request > kPresetC)
        return {};
    if (settingId == kRenderPresetSettingId)
        return { true, true, static_cast<uint32_t>(request) };
    if (settingId == kOverrideSettingId)
        return { true, true, kOverrideOn };
    return {};
}

// Live path: NVIDIA's cached-settings reader is asked for `settingId` and the active request is `request`.
constexpr Answer AnswerCachedRead(uint32_t settingId, int request, const Baseline& baseline)
{
    if (settingId != kRenderPresetSettingId || !ValidRequest(request))
        return {};
    if (request != kAuto)
        return { true, true, static_cast<uint32_t>(request) };
    switch (baseline.kind)
    {
    case Baseline::Kind::Value:
        return { true, true, baseline.value };
    case Baseline::Kind::NotFound:
        return { true, false, 0 };
    default:
        return {}; // the module's own cache is untouched and still holds the original
    }
}

// The reply of the original driver query: `ok` = the call succeeded, `isNumber` = it is a DWORD setting.
constexpr Baseline BaselineFromDriverReply(bool ok, bool isNumber, uint32_t value)
{
    if (!ok)
        return { Baseline::Kind::NotFound, 0 };
    if (!isNumber)
        return {};
    return { Baseline::Kind::Value, value };
}

// Process-wide request state. The active request starts as the configured one; only the dispatch thread commits a
// change (PresetSwitch.h), any thread reads.
class RequestState
{
  public:
    // First call wins; an out-of-range configured value means Auto.
    void InitOnce(int configured)
    {
        bool expected = false;
        if (!initialized_.compare_exchange_strong(expected, true))
            return;
        const int request = ValidRequest(configured) ? configured : kAuto;
        boot_.store(request);
        active_.store(request, std::memory_order_release);
    }

    int Active() const { return active_.load(std::memory_order_acquire); }
    int Boot() const { return boot_.load(std::memory_order_relaxed); }

    // Invalid requests are ignored.
    void Commit(int request)
    {
        if (ValidRequest(request))
            active_.store(request, std::memory_order_release);
    }

    // The driver query, as seen by the hook BEFORE any override. The first reply is kept.
    void NoteDriverQuery(bool ok, bool isNumber, uint32_t value)
    {
        const auto fresh = BaselineFromDriverReply(ok, isNumber, value);
        if (fresh.kind != Baseline::Kind::Unknown)
        {
            uint64_t expected = 0; // Unknown
            baseline_.compare_exchange_strong(expected, (static_cast<uint64_t>(fresh.kind) << 32) | fresh.value);
        }
        querySeen_.store(true, std::memory_order_relaxed);
    }

    // Kind and value travel in one word, so a reader never sees a half-written baseline.
    Baseline Base() const
    {
        const uint64_t word = baseline_.load(std::memory_order_acquire);
        return { static_cast<Baseline::Kind>(word >> 32), static_cast<uint32_t>(word) };
    }
    bool QuerySeen() const { return querySeen_.load(std::memory_order_relaxed); }
    void ClearQuerySeen() { querySeen_.store(false, std::memory_order_relaxed); }

    // What the cached reader answers for `settingId` with the active request.
    Answer ReadCached(uint32_t settingId) const { return AnswerCachedRead(settingId, Active(), Base()); }

    // Going back to Auto live is only possible when the original is known: it is the untouched cache when the process
    // booted with Auto, otherwise the baseline the driver query recorded.
    bool CanReturnToAuto() const { return Boot() == kAuto || Base().kind != Baseline::Kind::Unknown; }

  private:
    std::atomic<bool> initialized_ { false };
    std::atomic<int> boot_ { kAuto };
    std::atomic<int> active_ { kAuto };
    std::atomic<uint64_t> baseline_ { 0 };
    std::atomic<bool> querySeen_ { false };
};
} // namespace ext::fg::preset
