#pragma once
// DLSS-G preset readout: a tolerant parser for the preset lines of NVIDIA's runtime and the state the menu shows. Pure (no
// Windows or logging headers); tests/ext/fg/fg_preset_log.cpp builds it standalone.
//
// The runtime's own log lines reach OptiScaler through Streamline's log callback, which writes them as
//   SL Log: [..][streamline][info][tid:N][..]commonEntry.cpp:1369[ngxLog] [FrameGeneration] [date] [tid:N][Function:line] message
// The wording of the preset messages is known from the runtime's strings of two builds, not yet from a real run:
//   310.9.1.0   "Preset ID: N", "Preset A selected, disabling UIR.", "Enabling/Disabling bUIRecompositionSupported due to preset
//               override.", "ReportOverrideStates: render preset (requested=N, applied=N, value=N)"
//   310.9.16    "Applying settings for render preset N.", "Warning: Ignoring unsupported render preset ID N.",
//               "ReportOverrideStates: render preset (requested=N, effective=N)", and two private-flag notes of ReadRegkeys
//               ("Disabling UI recomposition / FG Lite due to private flag override."); "Preset ID", "Preset A selected" and the
//               bUIRecomposition notes of 310.9.1.0 are gone
// so every check is a case-insensitive keyword match and anything with "preset" in it that no rule claims is kept as a candidate:
// shown raw in the menu and logged at a low rate for a parser follow-up.

#include "ext/fg/dlssg/PresetRequest.h"

#include <cctype>
#include <climits>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace ext::fg::preset
{
inline constexpr int kMaxLineChars = 400;    // longest raw line kept for the menu
inline constexpr int kMaxCandidateLogs = 20; // unrecognised preset lines logged per session
inline constexpr uint64_t kCandidateGapMs = 2000;

enum class LineKind : uint8_t
{
    None,           // not a preset line (other feature, other topic, not a relayed NVIDIA line, our own text)
    Report,         // "ReportOverrideStates: render preset (requested=N, applied=N, value=N)" or (requested=N, effective=N)
    PresetId,       // "Preset ID: N"
    Selected,       // "Preset A selected, disabling UIR."
    UirNote,        // "Enabling/Disabling bUIRecompositionSupported due to preset override."
    BlockedByFlags, // "Not parsing presets due to private flag overrides."
    FlagOverride,   // 310.9.16: "Disabling UI recomposition / FG Lite due to private flag override." (the preset is still applied)
    Applying,       // 310.9.16: "Applying settings for render preset N."
    Unsupported,    // 310.9.16: "Warning: Ignoring unsupported render preset ID N."
    Candidate       // mentions a preset, no rule claims it
};

enum class Uir : uint8_t
{
    Unknown,
    Disabled,
    Enabled
};

struct ParsedLine
{
    LineKind kind = LineKind::None;
    bool recognised = false; // the fields the kind promises were read
    int requested = -1;      // Report
    int applied = -1;        // Report
    uint64_t value = 0;      // Report value, PresetId number
    bool hasValue = false;
    char letter = 0; // Selected: 'A'..'Z'
    Uir uir = Uir::Unknown;
    int effective = -1;     // Report in the 310.9.16 wording: the preset the endpoint ended up with
    bool newFormat = false; // Report in the 310.9.16 wording (requested = the preset number, effective = the preset in force)
};

// ---- what the runtime CONFIRMED about a request ------------------------------------------------------------------------------
// The request is what we asked for; the runtime decides what it does with it. 310.9.1.0 acts on 1 (A) and 2 (B) and ignores 3 (C);
// 310.9.16 acts on all three (C = UI recomposition off, FG Lite on). The menu and the UI recomposition rule go by what the runtime
// reported, not by the request.
enum class Effect : uint8_t
{
    Unknown,    // nothing heard that is about this request (log not readable, wording unknown, not yet logged)
    Applied,    // the runtime reported it applied this preset
    NotApplied  // the runtime read the preset and did not apply it (applied=0, a different effective preset, or "Ignoring unsupported render preset")
};

struct Confirmation
{
    Effect effect = Effect::Unknown;
    Uir uir = Uir::Unknown; // what the runtime said about its UI recomposition for the current request
};


namespace detail
{
inline std::string Lower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool Has(const std::string& lowered, std::string_view word) { return lowered.find(word) != std::string::npos; }

// The integer after `key` (spaces, then one optional '=' or ':', spaces): decimal with an optional minus, or 0x hex. False when
// there is none or it does not fit; never a guessed value.
inline bool NumberAfter(const std::string& lowered, std::string_view key, int64_t& out)
{
    const size_t at = lowered.find(key);
    if (at == std::string::npos)
        return false;
    size_t i = at + key.size();
    auto skip = [&]
    {
        while (i < lowered.size() && (lowered[i] == ' ' || lowered[i] == '\t'))
            ++i;
    };
    skip();
    if (i < lowered.size() && (lowered[i] == '=' || lowered[i] == ':'))
        ++i;
    skip();
    bool negative = false;
    if (i < lowered.size() && lowered[i] == '-')
    {
        negative = true;
        ++i;
    }
    uint64_t base = 10;
    if (i + 1 < lowered.size() && lowered[i] == '0' && lowered[i + 1] == 'x')
    {
        base = 16;
        i += 2;
    }
    uint64_t value = 0;
    size_t digits = 0;
    for (; i < lowered.size(); ++i)
    {
        const char c = lowered[i];
        uint64_t d = 0;
        if (c >= '0' && c <= '9')
            d = static_cast<uint64_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = 10u + static_cast<uint64_t>(c - 'a');
        else
            break;
        if (value > (static_cast<uint64_t>(INT64_MAX) - d) / base)
            return false; // does not fit
        value = value * base + d;
        ++digits;
    }
    if (digits == 0)
        return false;
    out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
    return true;
}

// "preset", spaces, one letter, spaces or a comma, "selected": the letter in upper case, 0 when the pattern is absent.
inline char SelectedLetter(const std::string& lowered)
{
    for (size_t at = lowered.find("preset"); at != std::string::npos; at = lowered.find("preset", at + 1))
    {
        size_t i = at + 6;
        if (i >= lowered.size() || lowered[i] != ' ')
            continue;
        while (i < lowered.size() && lowered[i] == ' ')
            ++i;
        if (i >= lowered.size() || !std::isalpha(static_cast<unsigned char>(lowered[i])))
            continue;
        const char letter = lowered[i];
        ++i;
        if (i >= lowered.size() || !(lowered[i] == ' ' || lowered[i] == ','))
            continue;
        while (i < lowered.size() && (lowered[i] == ' ' || lowered[i] == ','))
            ++i;
        if (lowered.compare(i, 8, "selected") == 0)
            return static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    }
    return 0;
}

inline Uir UirFrom(const std::string& lowered)
{
    if (Has(lowered, "disabling"))
        return Uir::Disabled;
    if (Has(lowered, "enabling"))
        return Uir::Enabled;
    return Uir::Unknown;
}

inline bool ToInt(int64_t v, int& out)
{
    if (v < 0 || v > INT_MAX)
        return false;
    out = static_cast<int>(v);
    return true;
}
} // namespace detail

// Classifies one log message as handed to the logger. Only relayed NVIDIA runtime lines are looked at: they start with the
// Streamline relay prefix and carry the [ngxLog] marker, and lines of the DLSS super resolution and ray reconstruction features
// (which talk about their own presets) are skipped. OptiScaler's own lines never start with the relay prefix.
inline ParsedLine ParseLine(std::string_view line)
{
    ParsedLine out;
    constexpr std::string_view kRelay = "SL Log: [";
    if (line.substr(0, kRelay.size()) != kRelay || line.find("ngxLog]") == std::string_view::npos)
        return out;
    if (line.find("[SuperSampling]") != std::string_view::npos || line.find("[RayReconstruction]") != std::string_view::npos)
        return out;
    if (line.size() > 8192)
        line = line.substr(0, 8192);

    const std::string low = detail::Lower(line);
    using detail::Has;
    if (!Has(low, "preset") && !Has(low, "private flag override"))
        return out;

    // 310.9.16: "Applying settings for render preset N." (written by ApplyRenderPreset when ReadRegkeys acts on 1, 2 or 3). The
    // recomposition follows the preset the way ReadRegkeys stores it: 1 (A) and 3 (C) turn it off, 2 (B) turns it on.
    if (Has(low, "applying settings for render preset"))
    {
        out.kind = LineKind::Applying;
        int64_t n = 0;
        if (detail::NumberAfter(low, "render preset", n) && n >= 1 && n <= 3)
        {
            out.value = static_cast<uint64_t>(n);
            out.hasValue = true;
            out.uir = (n == 2) ? Uir::Enabled : Uir::Disabled;
            out.recognised = true;
        }
        return out;
    }
    // 310.9.16: "Warning: Ignoring unsupported render preset ID N." - the runtime read the id and does not act on it.
    if (Has(low, "unsupported render preset"))
    {
        out.kind = LineKind::Unsupported;
        int64_t n = 0;
        if (detail::NumberAfter(low, "preset id", n) && n >= 0)
        {
            out.value = static_cast<uint64_t>(n);
            out.hasValue = true;
            out.recognised = true;
        }
        return out;
    }

    if (Has(low, "render preset") && (Has(low, "requested") || Has(low, "applied") || Has(low, "effective")))
    {
        out.kind = LineKind::Report;
        int64_t n = 0;
        const bool requested = detail::NumberAfter(low, "requested", n) && detail::ToInt(n, out.requested);
        if (!requested)
            out.requested = -1;
        if (Has(low, "effective"))
        {
            // 310.9.16: requested = the preset number the driver cache held, effective = the preset the endpoint ended up with
            // (EndpointCore::ReportOverrideStates derives it from the recomposition and FG Lite settings: 2 when the recomposition
            // is on, else 3 when FG Lite is on, else 1).
            out.newFormat = true;
            const bool effective = detail::NumberAfter(low, "effective", n) && detail::ToInt(n, out.effective);
            if (!effective)
                out.effective = -1;
            if (requested)
            {
                out.value = static_cast<uint64_t>(out.requested);
                out.hasValue = true;
            }
            if (effective)
                out.uir = (out.effective == 2) ? Uir::Enabled : ((out.effective == 1 || out.effective == 3) ? Uir::Disabled : Uir::Unknown);
            out.recognised = requested && effective;
            return out;
        }
        const bool applied = detail::NumberAfter(low, "applied", n) && detail::ToInt(n, out.applied);
        if (!applied)
            out.applied = -1;
        if (detail::NumberAfter(low, "value", n) && n >= 0)
        {
            out.value = static_cast<uint64_t>(n);
            out.hasValue = true;
        }
        out.recognised = requested && applied;
        return out;
    }
    if (Has(low, "not parsing presets"))
    {
        out.kind = LineKind::BlockedByFlags;
        out.recognised = true;
        return out;
    }
    if (Has(low, "private flag override")) // the flag changes the recomposition or FG Lite setting, the preset is still applied
    {
        out.kind = LineKind::FlagOverride;
        out.recognised = true;
        if (Has(low, "disabling") && Has(low, "recomposition")) // also inside "bUIRecompositionSupported"
            out.uir = Uir::Disabled; // 310.9.16 "Disabling UI recomposition ...", 310.9.1.0 "Disabling bUIRecompositionSupported ..."
        return out;
    }
    if (Has(low, "buirecompositionsupported"))
    {
        out.kind = LineKind::UirNote;
        out.uir = detail::UirFrom(low);
        out.recognised = out.uir != Uir::Unknown;
        return out;
    }
    if (const char letter = detail::SelectedLetter(low); letter != 0)
    {
        out.kind = LineKind::Selected;
        out.letter = letter;
        out.uir = detail::UirFrom(low);
        out.recognised = true;
        return out;
    }
    if (Has(low, "preset id"))
    {
        out.kind = LineKind::PresetId;
        int64_t n = 0;
        if (detail::NumberAfter(low, "preset id", n) && n >= 0)
        {
            out.value = static_cast<uint64_t>(n);
            out.hasValue = true;
            out.recognised = true;
        }
        return out;
    }
    out.kind = LineKind::Candidate;
    return out;
}

// What the menu shows. Written from the log relay (any thread) and the preset glue, read from the UI thread.
class Readout
{
  public:
    struct View
    {
        bool queried = false;          // the DRS hook saw the runtime ask the driver for the preset (since process start)
        bool readSinceRequest = false; // the runtime read the request through its cached settings since the last switch
        bool confirmed = false;        // a recognised NVIDIA preset line arrived since the last request
        std::string lastLine;          // the last preset line of the runtime, verbatim (capped)
        int presetId = -1;             // "Preset ID: N"
        char selected = 0;             // "Preset X selected": X
        Uir uir = Uir::Unknown;
        bool hasReport = false; // ReportOverrideStates seen
        int requested = -1;
        int applied = -1;
        uint64_t value = 0;
        bool blockedByFlags = false; // "Not parsing presets due to private flag overrides"
        bool flagOverride = false;   // 310.9.16: a private flag overrides the recomposition or FG Lite setting; the preset is still applied
        bool reportNew = false;      // the report is in the 310.9.16 wording: `requested` is the preset number, `effective` the one in force
        int effective = -1;
        int appliedLine = -1;        // "Applying settings for render preset N."
        int unsupportedId = -1;      // "Ignoring unsupported render preset ID N."
    };

    void NoteDriverQuery(bool queried)
    {
        std::lock_guard lock(mutex_);
        state_.queried = state_.queried || queried;
    }

    void NoteCachedRead()
    {
        std::lock_guard lock(mutex_);
        state_.readSinceRequest = true;
    }

    // A new request is about to take effect: what the earlier endpoint reported no longer describes the runtime.
    void BeginRequest()
    {
        std::lock_guard lock(mutex_);
        const bool queried = state_.queried;
        state_ = {};
        state_.queried = queried;
    }

    View Snapshot() const
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

    // What the runtime has confirmed about `request` since the last BeginRequest (no copy of the raw line: cheap enough per frame).
    // ReportOverrideStates logs requested = 1 when the driver cache holds a preset, applied = 1 when the runtime applied one and
    // value = the preset number; a "Preset X selected" line confirms X. "Preset ID: N" only says the number was read.
    Confirmation Confirm(int request) const
    {
        Confirmation out;
        if (request == kAuto)
        {
            out.effect = Effect::Applied; // nothing to confirm
            return out;
        }
        std::lock_guard lock(mutex_);
        out.uir = state_.uir;
        if (state_.unsupportedId == request)
            out.effect = Effect::NotApplied; // 310.9.16: "Ignoring unsupported render preset ID N."
        else if (state_.hasReport && state_.reportNew && state_.requested == request)
            out.effect = (state_.effective == request) ? Effect::Applied : Effect::NotApplied; // 310.9.16 report: the final word
        else if (state_.hasReport && !state_.reportNew && state_.value == static_cast<uint64_t>(request))
        {
            if (state_.applied > 0)
                out.effect = Effect::Applied;
            else if (state_.requested > 0)
                out.effect = Effect::NotApplied;
        }
        else if (state_.appliedLine == request)
            out.effect = Effect::Applied; // 310.9.16: "Applying settings for render preset N."
        else if (state_.selected != 0 && state_.selected - 'A' + 1 == request)
            out.effect = Effect::Applied;
        return out;
    }

    // One log message. Returns true when the caller should log this line as an unrecognised preset line (rate limited).
    bool Feed(std::string_view line, uint64_t nowMs)
    {
        const ParsedLine p = ParseLine(line);
        if (p.kind == LineKind::None)
            return false;

        std::lock_guard lock(mutex_);
        state_.lastLine = Clean(line);
        switch (p.kind)
        {
        case LineKind::Report:
            if (p.recognised)
            {
                state_.hasReport = true;
                state_.reportNew = p.newFormat;
                state_.requested = p.requested;
                state_.applied = p.applied;
                state_.effective = p.effective;
                state_.value = p.hasValue ? p.value : 0;
                if (p.newFormat && p.uir != Uir::Unknown)
                    state_.uir = p.uir;
            }
            break;
        case LineKind::Applying:
            if (p.recognised)
            {
                state_.appliedLine = static_cast<int>(p.value);
                if (p.uir != Uir::Unknown)
                    state_.uir = p.uir;
            }
            break;
        case LineKind::Unsupported:
            if (p.recognised)
                state_.unsupportedId = static_cast<int>(p.value > INT_MAX ? INT_MAX : p.value);
            break;
        case LineKind::PresetId:
            if (p.recognised)
                state_.presetId = static_cast<int>(p.value > INT_MAX ? INT_MAX : p.value);
            break;
        case LineKind::Selected:
            state_.selected = p.letter;
            if (p.uir != Uir::Unknown)
                state_.uir = p.uir;
            break;
        case LineKind::UirNote:
            if (p.recognised)
                state_.uir = p.uir;
            break;
        case LineKind::BlockedByFlags:
            state_.blockedByFlags = true;
            break;
        case LineKind::FlagOverride:
            state_.flagOverride = true;
            if (p.uir != Uir::Unknown)
                state_.uir = p.uir;
            break;
        default:
            break;
        }

        if (p.kind == LineKind::Candidate)
        {
            if (candidateLogs_ >= kMaxCandidateLogs || (candidateLogs_ > 0 && nowMs - lastCandidateLog_ < kCandidateGapMs))
                return false;
            ++candidateLogs_;
            lastCandidateLog_ = nowMs;
            return true;
        }
        if (p.recognised)
            state_.confirmed = true;
        return false;
    }

  private:
    static std::string Clean(std::string_view line)
    {
        std::string out(line.substr(0, kMaxLineChars));
        for (auto& c : out)
            if (c == '\r' || c == '\n')
                c = ' ';
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    mutable std::mutex mutex_;
    View state_;
    int candidateLogs_ = 0;
    uint64_t lastCandidateLog_ = 0;
};
// Does this request pause the UI recomposition of NVIDIA's runtime (OptiScaler then does not ask for it)? A is documented to turn
// it off ("Preset A selected, disabling UIR"), so A pauses unless the runtime says it did not apply A or kept the recomposition
// on. C: 310.9.1.0 ignores it (nothing to pause) and 310.9.16 applies it with the recomposition off, so it pauses only when the
// runtime confirmed C AND turned the recomposition off.
inline bool PausesUiRecomposition(int request, Effect effect, Uir uir)
{
    if (request == kPresetA)
        return effect != Effect::NotApplied && uir != Uir::Enabled;
    if (request == kPresetC)
        return effect == Effect::Applied && uir == Uir::Disabled;
    return false;
}

// The status line of the menu. A preset is "in use" only when the runtime confirmed it. `replyReadable`: the runtime's reply could
// reach us (the log relay is attached and the log level lets info lines through).
inline std::string StatusSentence(int request, Effect effect, bool replyReadable)
{
    if (!ValidRequest(request) || request == kAuto)
        return "In use: Auto";
    const std::string name = Name(request);
    switch (effect)
    {
    case Effect::Applied:
        return "In use: " + name + " (confirmed by the runtime)";
    case Effect::NotApplied:
        return name + " was requested, but the runtime did not apply it: its own default stays in use.";
    default:
        return name + " requested, waiting for the runtime to confirm it." +
               (replyReadable ? "" : " The runtime's reply cannot be read here (see below), so this stays unconfirmed.");
    }
}

} // namespace ext::fg::preset
