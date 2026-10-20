#pragma once
// DLSS-G preset readout: a tolerant parser for the preset lines of NVIDIA's runtime and the state the menu shows. Pure (no
// Windows or logging headers); tests/ext/fg/fg_preset_log.cpp builds it standalone.
//
// The runtime's own log lines reach OptiScaler through Streamline's log callback, which writes them as
//   SL Log: [..][streamline][info][tid:N][..]commonEntry.cpp:1369[ngxLog] [FrameGeneration] [date] [tid:N][Function:line] message
// The wording of the preset messages is known from the runtime's strings (310.9.1.0), not yet from a real run, so every
// check is a case-insensitive keyword match and anything with "preset" in it that no rule claims is kept as a candidate:
// shown raw in the menu and logged at a low rate for a parser follow-up.

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
    Report,         // "ReportOverrideStates: render preset (requested=N, applied=N, value=N)"
    PresetId,       // "Preset ID: N"
    Selected,       // "Preset A selected, disabling UIR."
    UirNote,        // "Enabling/Disabling bUIRecompositionSupported due to preset override."
    BlockedByFlags, // "Not parsing presets due to private flag overrides."
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

    if (Has(low, "render preset") && (Has(low, "requested") || Has(low, "applied")))
    {
        out.kind = LineKind::Report;
        int64_t n = 0;
        const bool requested = detail::NumberAfter(low, "requested", n) && detail::ToInt(n, out.requested);
        if (!requested)
            out.requested = -1;
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
    if (Has(low, "not parsing presets") || Has(low, "private flag override"))
    {
        out.kind = LineKind::BlockedByFlags;
        out.recognised = true;
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
                state_.requested = p.requested;
                state_.applied = p.applied;
                state_.value = p.hasValue ? p.value : 0;
            }
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
} // namespace ext::fg::preset
