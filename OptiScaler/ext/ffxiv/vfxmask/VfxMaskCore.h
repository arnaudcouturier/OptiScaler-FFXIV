#pragma once
// The VFX mask gate probe ([FFXIV] VfxMaskProbe = off | left | right, ini only, off by default): does the upscaler read the
// mask input at all? Pure decision logic and text only: no pch, no Windows, D3D or NGX call, so every rule here is covered by
// tests/ext/ffxiv/vfxmask on the host.
//
// Why the probe exists: the game hands the upscaler no mask of any kind (its evaluate packet sets the bias, transparency and
// particle keys to NULL), so the fork would be the first producer of one. NVIDIA's DLSS programming guide still documents the
// bias current colour mask but says it was not introduced for the latest models and should not be used, and names only the
// older preset F as supporting it; the FSR and XeSS backends read the same slot as their reactive and responsive masks. Before
// any mask generation is built, this probe answers the cheap question: does a constant mask change the picture at all?
//
// What it does: a constant R8_UNORM mask the size of the colour input, one half at 1.0 (masked) and one half at 0.0, is handed
// to the mask input of the evaluate in progress. Two frames of a still scene never match bit for bit (the jitter phase keeps
// moving), so the unmasked half is the noise floor: the on-versus-off difference of the masked half has to be clearly larger
// than that of the unmasked half for the mask to be read. The half is chosen per run (left or right) so a pair of screenshots
// shows the masked and the unmasked half in the same picture.
//
// Fail open: a mode, a size or a texture the probe cannot use means no write at all, and the game's own value for the key
// (NULL) goes through exactly as without this feature.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ext::ffxiv::vfxmask
{
enum class ProbeMode : std::uint8_t
{
    Off = 0,
    Left = 1,  // the left half of the mask is 1.0, the right half 0.0
    Right = 2, // the right half of the mask is 1.0, the left half 0.0
};

constexpr const char* ModeName(ProbeMode mode) noexcept
{
    switch (mode)
    {
    case ProbeMode::Left: return "left";
    case ProbeMode::Right: return "right";
    case ProbeMode::Off: break;
    }
    return "off";
}

// "off", "left" or "right", in any case, with surrounding spaces and tabs ignored. Anything else reads as off: a value this
// build does not know must leave the game's own behaviour alone.
constexpr ProbeMode ParseMode(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);

    auto lower = [](char c) constexpr noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };

    const char* const left = "left";
    const char* const right = "right";
    if (text.size() == 4)
    {
        bool same = true;
        for (std::size_t i = 0; i < 4 && same; ++i)
            same = lower(text[i]) == left[i];
        if (same)
            return ProbeMode::Left;
    }
    if (text.size() == 5)
    {
        bool same = true;
        for (std::size_t i = 0; i < 5 && same; ++i)
            same = lower(text[i]) == right[i];
        if (same)
            return ProbeMode::Right;
    }
    return ProbeMode::Off;
}

// Full intensity for a masked pixel, nothing for an unmasked one (the guide asks for exactly that: 1.0 inside, 0.0 outside).
inline constexpr std::uint8_t kMaskValue = 255;
inline constexpr std::uint8_t kClearValue = 0;

// One column cannot be split in two, and 16384 is the largest texture edge every D3D11 feature level this fork runs on takes.
inline constexpr unsigned kMinMaskEdge = 2;
inline constexpr unsigned kMaxMaskEdge = 16384;

// Masked column of a row `width` wide: the left half is x < width / 2, the right half x >= width / 2. The two modes are exact
// complements, also for an odd width (where the right half is the wider one by a column).
constexpr bool MaskedAt(unsigned x, unsigned width, ProbeMode mode) noexcept
{
    if (mode == ProbeMode::Off || width < kMinMaskEdge || x >= width)
        return false;
    return mode == ProbeMode::Left ? x < width / 2 : x >= width / 2;
}

constexpr std::uint8_t MaskValueAt(unsigned x, unsigned width, ProbeMode mode) noexcept
{
    return MaskedAt(x, width, mode) ? kMaskValue : kClearValue;
}

// The size of the mask an evaluate needs: the colour input's own size, never a guess. `ok` is false when the probe must not
// write (mode off, a size no texture can have).
struct MaskPlan
{
    bool ok = false;
    unsigned width = 0;
    unsigned height = 0;
};

constexpr MaskPlan PlanMask(unsigned width, unsigned height, ProbeMode mode) noexcept
{
    const bool sizeable =
        width >= kMinMaskEdge && width <= kMaxMaskEdge && height >= 1 && height <= kMaxMaskEdge;
    return { mode != ProbeMode::Off && sizeable, width, height };
}

// One row of the mask, as it is uploaded: the masked half at kMaskValue, the rest at kClearValue.
inline void FillRow(std::uint8_t* row, unsigned width, ProbeMode mode) noexcept
{
    if (row == nullptr)
        return;
    for (unsigned x = 0; x < width; ++x)
        row[x] = MaskValueAt(x, width, mode);
}

// What one evaluate did, as far as the log is concerned.
enum class Outcome : std::uint8_t
{
    Given = 0,       // the parameter block kept the mask this evaluate
    Unconfirmed = 1, // the key was written, but reading it back did not return the mask
    Refused = 2,     // no mask could be given (no colour input, no texture, a size the plan rejects)
};

struct LogKey
{
    ProbeMode mode = ProbeMode::Off;
    unsigned width = 0;
    unsigned height = 0;
    Outcome outcome = Outcome::Given;
    // Counts the times the key was switched on, so switching it off and on again says what happened again (a still image
    // does not produce a per-frame line, and a changed configuration is one line, not one per frame).
    unsigned arms = 0;

    constexpr bool operator==(const LogKey&) const = default;
};

// One log line per change of LogKey, at most kMaxLines per session: a game that cannot use the mask must not flood the log, and
// a mask that is refused on every frame (a colour input that never arrives) is one line, not one per frame. The arm counter in
// the key makes switching the key off and on again say what happened again.
class LogGate
{
  public:
    static constexpr unsigned kMaxLines = 32;

    bool Take(const LogKey& key) noexcept
    {
        if (_lines >= kMaxLines || (_have && key == _last))
            return false;
        _have = true;
        _last = key;
        ++_lines;
        return true;
    }

  private:
    LogKey _last {};
    bool _have = false;
    unsigned _lines = 0;
};

// The one line a change of LogKey writes. `backend` names the upscaler this evaluate runs on (DLSS, FSR, XeSS); `reason`
// explains a refusal and must be null for the other outcomes.
inline std::string FormatLine(const LogKey& key, const char* backend, const char* reason)
{
    if (key.outcome == Outcome::Refused)
        return std::format("VFX mask probe: nothing written this evaluate ({}), colour input {}x{}",
                           reason != nullptr ? reason : "unknown reason", key.width, key.height);

    const std::string text = std::format(
        "VFX mask probe: {} half masked in a {}x{} R8_UNORM mask, given to the {} bias-mask input", ModeName(key.mode),
        key.width, key.height, backend != nullptr ? backend : "unknown");

    if (key.outcome == Outcome::Given)
        return text + "; the parameter block reports the mask back";
    return text + "; the parameter block does not report the mask back (the key was written, the block did not keep it)";
}
} // namespace ext::ffxiv::vfxmask
