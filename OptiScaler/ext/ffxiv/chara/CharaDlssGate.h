#pragma once
// Availability of the isolated DLSS path: whether a character preview may get its own upscaler feature right now.
// Pure decision over facts the implementation gathers (CharaDlss.cpp), so every reason is tested without a device.
//
// The path calls the driver's real D3D11 NGX entry points through the NGX proxy. They work only after the game (or this
// fork on its behalf) initialised D3D11 NGX on the same device that draws the previews. Anything else keeps the native
// preview image, and the reason is written to the log once.

#include <cstdint>

namespace ext::ffxiv::chara::dlss
{
struct GateFacts
{
    bool configOn = false;           // [FFXIV] PreviewDLSS
    bool latched = false;            // a failure ended the DLSS path for this session
    bool ngxModuleLoaded = false;    // the driver's NGX module is loaded
    bool dlssEnabledInConfig = true; // the fork's own DLSS switch (the proxy reports "not initialised" when it is off)
    bool dx11Initialised = false;    // NVNGXProxy: D3D11 NGX was initialised
    bool entryPoints = false;        // allocate / destroy parameters, create / evaluate / release feature are all present
    bool deviceChecked = false;      // the draw context's device was compared with the NGX device
    bool deviceMatches = false;      // ... and they are the same device
};

enum class Availability : uint8_t
{
    Available,
    ConfigOff,
    Latched,
    NoNgxModule,
    DlssDisabledInConfig,
    NgxNotInitialised,
    EntryPointsMissing,
    DeviceMismatch,
};

constexpr Availability Evaluate(const GateFacts& f)
{
    if (!f.configOn)
        return Availability::ConfigOff;
    if (f.latched)
        return Availability::Latched;
    if (!f.ngxModuleLoaded)
        return Availability::NoNgxModule;
    if (!f.dlssEnabledInConfig)
        return Availability::DlssDisabledInConfig;
    if (!f.dx11Initialised)
        return Availability::NgxNotInitialised;
    if (!f.entryPoints)
        return Availability::EntryPointsMissing;
    if (f.deviceChecked && !f.deviceMatches)
        return Availability::DeviceMismatch;
    return Availability::Available;
}


// ---- Which device is which ----------------------------------------------------------------------------------------------
// A D3D11 device pointer is not an identity: the game's device may be wrapped (ReShade hands the game a proxy that forwards to
// the native device), and the pointer NGX was initialised with, the one `context->GetDevice()` returns and the one a texture
// reports can all differ while naming one device. The native device of a pointer is therefore read through a resource created
// on it: a resource always reports the native device it lives on. Two pointers name the same device when those agree.
struct DeviceIdentity
{
    uintptr_t raw = 0;    // the pointer as handed over (0 = not known)
    uintptr_t native = 0; // the device a resource created through `raw` reports (0 = could not be read)

    constexpr bool Known() const { return raw != 0; }
    // The pointer is a wrapper around another device object.
    constexpr bool IsProxy() const { return native != 0 && native != raw; }
    constexpr uintptr_t Identity() const { return native != 0 ? native : raw; }
};

enum class DeviceVerdict : uint8_t
{
    NotCompared, // one side is not known yet
    Same,
    Different,
};

constexpr DeviceVerdict CompareDevices(const DeviceIdentity& ngx, const DeviceIdentity& draw)
{
    if (!ngx.Known() || !draw.Known())
        return DeviceVerdict::NotCompared;
    return ngx.Identity() == draw.Identity() ? DeviceVerdict::Same : DeviceVerdict::Different;
}

// A verdict is cached for as long as the facts it came from: the NGX identity generation (bumped when NGX initialises or shuts
// down) and the draw context's device pointer. A "different" verdict is also looked at again after `kRecheckMs`, so a stale
// answer cannot outlive a changed setup.
inline constexpr uint64_t kDeviceRecheckMs = 5000;

struct DeviceCache
{
    DeviceVerdict verdict = DeviceVerdict::NotCompared;
    uint32_t generation = 0;
    uintptr_t drawKey = 0; // the draw context pointer the verdict was made for
    uint64_t checkedAt = 0;

    bool NeedsCheck(uint32_t ngxGeneration, uintptr_t currentDrawKey, uint64_t now) const
    {
        if (verdict == DeviceVerdict::NotCompared || generation != ngxGeneration || drawKey != currentDrawKey)
            return true;
        return verdict == DeviceVerdict::Different && now >= checkedAt && now - checkedAt >= kDeviceRecheckMs;
    }

    void Store(DeviceVerdict v, uint32_t ngxGeneration, uintptr_t currentDrawKey, uint64_t now)
    {
        verdict = v;
        generation = ngxGeneration;
        drawKey = currentDrawKey;
        checkedAt = now;
    }

    void Reset() { *this = DeviceCache {}; }
};

// The reason in log text. The same strings serve as menu notes (static storage, safe to hand to another thread).
constexpr const char* ReasonText(Availability a)
{
    switch (a)
    {
    case Availability::Available: return "";
    case Availability::ConfigOff: return "";
    case Availability::Latched: return "switched off for this session after a failure";
    case Availability::NoNgxModule: return "the driver's NGX module is not loaded";
    case Availability::DlssDisabledInConfig: return "DLSS is switched off in the configuration";
    case Availability::NgxNotInitialised: return "waiting for the game to initialise D3D11 NGX";
    case Availability::EntryPointsMissing: return "the D3D11 NGX entry points are missing";
    case Availability::DeviceMismatch: return "the NGX device is not the device that draws the previews";
    }
    return "";
}

// True when `a` is worth one log line (a state that explains a missing effect).
constexpr bool WorthLogging(Availability a) { return a != Availability::Available && a != Availability::ConfigOff; }

// Logs a reason when it changes, at most `kMaxReasonLines` times in total (a flapping NGX state must not flood the log).
inline constexpr uint32_t kMaxReasonLines = 8;

struct ReasonLog
{
    Availability last = Availability::ConfigOff;
    uint32_t lines = 0;

    // Returns true when the caller should write a line for `a` now.
    bool ShouldLog(Availability a)
    {
        if (a == last)
            return false;
        last = a;
        if (!WorthLogging(a) || lines >= kMaxReasonLines)
            return false;
        ++lines;
        return true;
    }
};
} // namespace ext::ffxiv::chara::dlss
