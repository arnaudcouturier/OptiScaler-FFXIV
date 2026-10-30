#pragma once
// Menu logging of the HUD-less card's sliders: one log line per change that has settled, with the final value, so a session log
// shows what the user set and when (a play session cannot be judged without it: the ini is only written by Save Settings, and a
// mis-set Coverage guard left no line at all). ImGui cannot run offline, so the settle rule and the line texts live here and the
// CPU test drives them.
//
// A slider reports a change on every mouse move while it is dragged. Logging each would flood the log, so a change only arms a
// timer and the line is written once the value has not changed for kSettleMs; a further change restarts the timer and the line
// then carries the value the slider came to rest on. The card flushes Due() on every frame it draws, before the Advanced fold's
// early return, so a change that settled while the fold was closed is written on the next frame of the card. "Reset advanced"
// writes one line with the values it restored and drops the pending ones.

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ext::hudless
{
// One slider's settle timer.
class SliderSettleLog
{
  public:
    static constexpr uint64_t kSettleMs = 500;

    // A new widget value. Nothing is written from here: the line comes from Due().
    void Observe(float value, uint64_t nowMs)
    {
        if (!_armed || value != _pending)
        {
            _armed = true;
            _pending = value;
            _changedAt = nowMs;
        }
    }

    // True once the value has been stable for kSettleMs. The difference is signed, so a clock that jumped backwards (a suspend, a
    // system time change) holds the line until the next change instead of writing it early.
    bool Due(uint64_t nowMs) const
    {
        return _armed && static_cast<int64_t>(nowMs) - static_cast<int64_t>(_changedAt) >= static_cast<int64_t>(kSettleMs);
    }
    // The value to log, and the slider is no longer armed.
    float Take()
    {
        _armed = false;
        return _pending;
    }
    bool Armed() const { return _armed; }
    float Pending() const { return _pending; }

  private:
    float _pending = 0.f;
    uint64_t _changedAt = 0;
    bool _armed = false;
};

// The three Advanced sliders of the card, each with its own timer.
struct HudlessSliderLogs
{
    SliderSettleLog maskThreshold;
    SliderSettleLog maskDilation;
    SliderSettleLog coverageGuard;

    // Called once per menu frame after the widgets. Returns the lines to write now, one per slider that settled; each line names
    // the ini key, so it can be matched with a saved ini, and the value the slider came to rest on.
    std::vector<std::string> Due(uint64_t nowMs)
    {
        std::vector<std::string> lines;
        if (maskThreshold.Due(nowMs))
            lines.push_back(std::format("HudLess: MaskThreshold set to {:.3f} from the menu", maskThreshold.Take()));
        if (maskDilation.Due(nowMs))
            lines.push_back(std::format("HudLess: MaskDilation set to {} px from the menu", static_cast<int>(maskDilation.Take())));
        if (coverageGuard.Due(nowMs))
            lines.push_back(GuardLine(coverageGuard.Take()));
        return lines;
    }

    // The guard value as both forms the user sees: the ini number and the share it means.
    static std::string GuardLine(float limit)
    {
        if (limit <= 0.f)
            return "HudLess: CoverageGuard set to off from the menu";
        return std::format("HudLess: CoverageGuard set to {:.2f} ({:.2g}% of the screen) from the menu", limit,
                           100.0 * static_cast<double>(limit));
    }

    // "Reset advanced": the three values it restored, in one line, and every pending drag is dropped with them. A Reset pressed
    // within the settle time of a drag used to log the dragged value afterwards ("set to 0.012" while it was already 0.008); the
    // log now shows the Reset and nothing the Reset replaced.
    std::vector<std::string> Reset(float threshold, int dilation, float guard)
    {
        maskThreshold.Take();
        maskDilation.Take();
        coverageGuard.Take();
        const std::string guardText = guard <= 0.f ? std::string("off") : std::format("{:.2f}", guard);
        return { std::format("HudLess: advanced settings reset from the menu: MaskThreshold {:.3f}, MaskDilation {} px, "
                             "CoverageGuard {}",
                             threshold, dilation, guardText) };
    }
};
} // namespace ext::hudless
