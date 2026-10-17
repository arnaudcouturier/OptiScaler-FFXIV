#pragma once
// Decision logic of [DlssNr] DisableOnFocusLoss: should the NR pass run this frame, given whether the game window is
// the foreground window and whether it is minimised?
//
// Pure: no Win32, no config, no logging, so it is unit-tested without the game (tests/ext/nr/nr_focus_gate.cpp). The
// caller samples the window state and owns the side effects (ext/nr/NrFocus.cpp, DlssNr_Dx12.cpp).
//
// Timing, chosen so a brief flicker never flaps the pass but a real alt-tab saves the GPU time quickly:
//   * pause after the window has been unfocused for 1000 ms without a break. Menu popups, IME candidate windows,
//     screenshot tools and notification toasts take the foreground for a few hundred milliseconds at most.
//   * minimising pauses at once. A minimised window cannot flicker, and nothing is on screen to spoil.
//   * resume after 250 ms of continuous focus: long enough to ride out a focus bounce on the way back (the shell
//     hands the foreground over in steps), short enough that the pass is back before the player notices.
// The feature switch is part of the input: with it off the pass is always allowed and every timer is cleared, so
// turning it on while the window is already in the background starts the 1 s count at that moment.

#include <cstdint>

namespace ext::nr
{
class FocusGate
{
  public:
    static constexpr uint64_t kPauseAfterMs = 1000;
    static constexpr uint64_t kResumeAfterMs = 250;

    // `enabled`: the DisableOnFocusLoss switch (already combined with "NR is on" by the caller).
    // `nowMs`: any monotonic millisecond clock. Returns true when the NR pass may run.
    bool Update(bool enabled, bool focused, bool minimised, uint64_t nowMs)
    {
        if (!enabled)
        {
            _allowed = true;
            _awaySince = _focusedSince = 0;
            _away = _focus = false;
            return true;
        }

        const bool away = !focused || minimised;

        if (_allowed)
        {
            _focus = false;
            if (minimised)
            {
                Pause();
            }
            else if (!away)
            {
                _away = false;
            }
            else
            {
                if (!_away)
                {
                    _away = true;
                    _awaySince = nowMs;
                }
                if (Elapsed(_awaySince, nowMs) >= kPauseAfterMs)
                    Pause();
            }
        }
        else
        {
            _away = false;
            if (away)
            {
                _focus = false;
            }
            else
            {
                if (!_focus)
                {
                    _focus = true;
                    _focusedSince = nowMs;
                }
                if (Elapsed(_focusedSince, nowMs) >= kResumeAfterMs)
                {
                    _allowed = true;
                    _focus = false;
                }
            }
        }

        return _allowed;
    }

    bool Allowed() const { return _allowed; }

  private:
    void Pause()
    {
        _allowed = false;
        _away = false;
        _focus = false;
    }

    // A clock that steps backwards counts as no time having passed.
    static uint64_t Elapsed(uint64_t since, uint64_t now) { return now >= since ? now - since : 0; }

    bool _allowed = true;
    bool _away = false;  // unfocused sample seen, _awaySince valid
    bool _focus = false; // focused sample seen while paused, _focusedSince valid
    uint64_t _awaySince = 0;
    uint64_t _focusedSince = 0;
};
} // namespace ext::nr
