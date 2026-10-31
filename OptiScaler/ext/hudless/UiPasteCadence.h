#pragma once
// Paste cadence: which real frame's UI image each output present received, as a pure counter (no D3D12, no clock, no
// locking; UiPaste.cpp owns the mutex). UiPaste::Paste feeds it one record per output it pasted on. Nothing reads it for
// a decision: it only writes the log line that says whether the pasted UI moves at a steady pace
// (tests/ext/hudless/ui_paste_cadence.cpp).
//
// With frame generation at M outputs per real frame, a steady UI shows every real frame's image on exactly M outputs
// and skips none. The consumer pastes the newest image whose write has completed on the GPU at the moment the output is
// presented (UiPasteRing.h). When the outputs of a real frame are presented while that frame's image is still on the
// GPU, an older image is pasted instead; if that happens on some real frames and not on others, one image lands on 1
// output, the next on 3, and some are never shown. The counters below make that visible.

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace ext::hudless::cadence
{
struct Window
{
    uint64_t pastes = 0;        // outputs that got an image pasted
    uint64_t newestPending = 0; // of those, the newest image built was still on the GPU, so an older one was pasted
    uint64_t steady = 0;        // of those, pasted with steady timing on
    std::array<uint64_t, 4> age {};  // real frames from the newest one the game submitted to the pasted image: 0, 1, 2, 3+
    std::array<uint64_t, 4> runs {}; // outputs one image was pasted on, counted when the next image takes over: 1, 2, 3, 4+
    uint64_t framesSkipped = 0; // real frames between two consecutive pasted images whose image was never pasted
    uint64_t breaks = 0;        // the paste stopped and started again; the run that was open then is not counted
};

class Tracker
{
  public:
    // One output present that pasted the image of real frame `pastedFrame`. `currentFrame` is the newest real frame the
    // game submitted at that moment, `newestPending` says the newest built image had not completed yet.
    void OnPaste(uint64_t currentFrame, uint64_t pastedFrame, bool newestPending, bool steady)
    {
        ++_w.pastes;
        if (newestPending)
            ++_w.newestPending;
        if (steady)
            ++_w.steady;
        const uint64_t age = currentFrame > pastedFrame ? currentFrame - pastedFrame : 0;
        ++_w.age[age < 3 ? age : 3];

        if (_open && pastedFrame == _frame)
        {
            ++_run;
            return;
        }
        if (_open && pastedFrame > _frame)
        {
            ++_w.runs[_run < 4 ? _run - 1 : 3];
            _w.framesSkipped += pastedFrame - _frame - 1;
        }
        _open = true;
        _frame = pastedFrame;
        _run = 1;
    }

    // An output that should have shown the UI and pasted nothing, or a paste gate that closed: what the outputs showed
    // around it is not one image's run, so the open run ends without being counted.
    void Break()
    {
        if (_open)
            ++_w.breaks;
        _open = false;
        _run = 0;
    }

    // The counters since the last Take; the open run carries over into the next window.
    Window Take()
    {
        const Window w = _w;
        _w = {};
        return w;
    }

  private:
    Window _w;
    uint64_t _frame = 0;
    uint32_t _run = 0;
    bool _open = false;
};

// The window of the log line: about 10 s of pasting. A pause in the pasting longer than kGapMs (the gate closed for a fallback, nothing to
// paste) ends the window at its last paste, so a line never spans a fallback and its image count is the count of its own time.
class WindowClock
{
  public:
    static constexpr double kWindowMs = 10000.0;
    static constexpr double kGapMs = 1000.0;

    struct Due
    {
        bool due = false;
        double seconds = 0.0; // the window's length
        uint64_t built = 0;   // paste images built in it
    };

    // Before a paste is counted: a window that a pause ended is due now (the paste opens the next one).
    Due BeforePaste(double nowMs, uint64_t produced)
    {
        Due d;
        if (_open && nowMs - _lastMs > kGapMs)
        {
            d = { true, (_lastMs - _startMs) / 1000.0, Built(_lastProduced) };
            _open = false;
        }
        if (!_open)
        {
            _open = true;
            _startMs = nowMs;
            _baseProduced = produced;
        }
        return d;
    }

    // After the paste was counted: the window is due once it is kWindowMs long (the next one starts here).
    Due AfterPaste(double nowMs, uint64_t produced)
    {
        _lastMs = nowMs;
        _lastProduced = produced;
        if (nowMs - _startMs < kWindowMs)
            return {};
        const Due d { true, (nowMs - _startMs) / 1000.0, Built(produced) };
        _startMs = nowMs;
        _baseProduced = produced;
        return d;
    }

  private:
    uint64_t Built(uint64_t produced) const { return produced >= _baseProduced ? produced - _baseProduced : 0; }

    bool _open = false;
    double _startMs = 0.0;
    double _lastMs = 0.0;
    uint64_t _baseProduced = 0;
    uint64_t _lastProduced = 0;
};

// One log line per window. `built` = paste images built in the same time, `seconds` = the window's length.
inline std::string Line(const Window& w, uint64_t built, double seconds)
{
    const double perImage = built > 0 ? static_cast<double>(w.pastes) / static_cast<double>(built) : 0.0;
    const double pendingShare = w.pastes > 0 ? 100.0 * static_cast<double>(w.newestPending) / static_cast<double>(w.pastes) : 0.0;
    return std::format("HudLess paste cadence ({:.1f} s): {} outputs pasted, {} images built ({:.2f} outputs per image); "
                       "newest image still on the GPU at {} pastes ({:.1f}%); pasted image age 0/1/2/3+ real frames {}/{}/{}/{}; "
                       "outputs per image 1/2/3/4+ {}/{}/{}/{}; real frames never pasted {}; breaks {}; steady timing {} of {}",
                       seconds, w.pastes, built, perImage, w.newestPending, pendingShare, w.age[0], w.age[1], w.age[2], w.age[3],
                       w.runs[0], w.runs[1], w.runs[2], w.runs[3], w.framesSkipped, w.breaks, w.steady, w.pastes);
}
} // namespace ext::hudless::cadence
