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
