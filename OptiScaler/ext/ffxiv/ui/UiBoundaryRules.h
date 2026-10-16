#pragma once
// S10 package A: the boundary rules R1 / R2 / R3 of docs/HUDLESS.md section 10 as pure, testable logic.
//
// The tracker sees one DrawIn per immediate-context draw of a frame (in order) and keeps the candidate boundary ordinals.
// Ordinals start at 1; 0 means "none". Nothing here knows D3D, the clock, files or the log, so tests/ext/ffxiv/ui builds
// it standalone.
//
//   Scene/post stages are 1..5 (tonemap ... fullscreen gamma), kUiStage (6) is a pixel shader of the UI set.
//   R1 (tag based):      the first UI-tagged draw AFTER the last scene/post stage (1..5) draw of the frame.
//   R2 (position based): the first draw AFTER the last stage-5 draw whose render target is the swapchain backbuffer.
//                        `r2any` is the same draw without the render-target condition (the other reading of the sentence
//                        in the design doc), kept so the analysis can compare both readings.
//   R3 (union):          whichever of R1 / R2 comes first, i.e. the smaller ordinal of the two that are valid.
//
// "After the last X" is only known at the end of the frame. The tracker is ONLINE: a candidate is (re)assigned as draws
// arrive and cleared when a later stage draw supersedes it, so a capture taken at a candidate must be repeated if the
// candidate moves. `Retrospective()` computes the same answers from the whole draw list; the two must agree on every input
// (tested), and the number of supersessions is reported per frame.

#include "ext/ffxiv/ui/UiShaderCatalog.h"

#include <cstdint>
#include <span>

namespace ext::ffxiv::ui
{
struct DrawIn
{
    uint8_t stage = kStageNone;  // kStageNone, 1..5, kUiStage, or kStageUnknown (shader created before the hook)
    bool rtBackbuffer = false;   // RTV 0 resource is the swapchain backbuffer; false when unknown or not looked up
};

struct Candidates
{
    uint32_t r1 = 0;
    uint32_t r2 = 0;
    uint32_t r2any = 0;
    uint32_t r3 = 0;

    bool operator==(const Candidates&) const = default;
};

struct FrameSummary
{
    uint32_t draws = 0;
    uint32_t sceneStageDraws = 0;
    uint32_t stageDraws[6] = {}; // index 1..5 used
    uint32_t uiDraws = 0;
    uint32_t unknownDraws = 0;           // shaders created before the hook: stage and UI membership cannot be told
    uint32_t lastSceneStage = 0;         // ordinal of the last stage 1..5 draw
    uint32_t lastStage5 = 0;             // ordinal of the last stage-5 draw
    uint32_t firstUi = 0;                // ordinal of the first UI draw, anywhere in the frame
    uint32_t uiBeforeLastScene = 0;      // UI draws that came before the last scene/post stage draw
    uint32_t r1Supersessions = 0;        // times a valid R1 candidate was dropped by a later scene/post stage draw
    uint32_t r2Supersessions = 0;        // same for R2 (a later stage-5 draw)
    uint32_t candidateMoves = 0;         // times the R3 candidate changed in the frame (capture opportunities)
    bool sawSceneStage = false;          // false on menu-only frames, loading screens, unknown shaders

    bool operator==(const FrameSummary&) const = default;
};

class BoundaryTracker
{
  public:
    void Reset() { *this = BoundaryTracker {}; }

    // The caller looks up the render target of a draw only when this is true or the draw is tagged (cost control): after a
    // stage-5 draw, until R2 is found.
    bool WantsRenderTarget() const { return _sawStage5 && _c.r2 == 0; }

    // Feeds the next draw of the frame. Returns true when the R3 candidate changed with this draw (the draw has NOT run
    // yet in the hook: this is the moment to capture the bound render target).
    bool OnDraw(const DrawIn& d)
    {
        ++_s.draws;
        const uint32_t ordinal = _s.draws;
        const uint32_t beforeR3 = _c.r3;
        const uint32_t beforeR1 = _c.r1;
        const uint32_t beforeR2 = _c.r2;

        const bool scene = d.stage >= 1 && d.stage <= 5;
        const bool ui = d.stage == kUiStage;

        if (d.stage == kStageUnknown)
            ++_s.unknownDraws;

        if (scene)
        {
            ++_s.sceneStageDraws;
            ++_s.stageDraws[d.stage];
            _s.sawSceneStage = true;
            _s.lastSceneStage = ordinal;
            _s.uiBeforeLastScene = _s.uiDraws;
            if (_c.r1 != 0)
                ++_s.r1Supersessions;
            _c.r1 = 0;
        }
        else if (ui)
        {
            ++_s.uiDraws;
            if (_s.firstUi == 0)
                _s.firstUi = ordinal;
            if (_c.r1 == 0)
                _c.r1 = ordinal;
        }

        if (d.stage == 5)
        {
            // The anchor of R2: the candidate restarts after the LAST stage-5 draw.
            _s.lastStage5 = ordinal;
            if (_c.r2 != 0)
                ++_s.r2Supersessions;
            _c.r2 = 0;
            _c.r2any = 0;
            _sawStage5 = true;
        }
        else if (_sawStage5)
        {
            // Any other draw after a stage-5 draw (stages 1..4 included: after its anchor R2 is hash independent).
            if (_c.r2any == 0)
                _c.r2any = ordinal;
            if (_c.r2 == 0 && d.rtBackbuffer)
                _c.r2 = ordinal;
        }

        _c.r3 = Min(_c.r1, _c.r2);
        _change = static_cast<uint8_t>((_c.r1 != beforeR1 ? 1 : 0) | (_c.r2 != beforeR2 ? 2 : 0));
        if (_c.r3 != beforeR3)
        {
            ++_s.candidateMoves;
            return true;
        }
        return false;
    }

    const Candidates& Current() const { return _c; }
    // Which candidates the last OnDraw changed (set, moved or cleared): bit 0 = R1, bit 1 = R2.
    uint8_t LastChange() const { return _change; }
    const FrameSummary& Summary() const { return _s; }

  private:
    static uint32_t Min(uint32_t a, uint32_t b) { return a == 0 ? b : (b == 0 ? a : (a < b ? a : b)); }

    Candidates _c {};
    FrameSummary _s {};
    bool _sawStage5 = false;
    uint8_t _change = 0;
};

// The same candidates computed from the complete list of a frame's draws (the definition the online tracker implements).
inline Candidates Retrospective(std::span<const DrawIn> draws)
{
    Candidates c;
    size_t lastScene = 0, lastStage5 = 0; // 1-based positions, 0 = none
    for (size_t i = 0; i < draws.size(); ++i)
    {
        const uint8_t s = draws[i].stage;
        if (s >= 1 && s <= 5)
            lastScene = i + 1;
        if (s == 5)
            lastStage5 = i + 1;
    }

    for (size_t i = lastScene; i < draws.size(); ++i)
        if (draws[i].stage == kUiStage)
        {
            c.r1 = static_cast<uint32_t>(i + 1);
            break;
        }

    if (lastStage5 != 0)
    {
        if (lastStage5 < draws.size())
            c.r2any = static_cast<uint32_t>(lastStage5 + 1);
        for (size_t i = lastStage5; i < draws.size(); ++i)
            if (draws[i].rtBackbuffer)
            {
                c.r2 = static_cast<uint32_t>(i + 1);
                break;
            }
    }

    c.r3 = c.r1 == 0 ? c.r2 : (c.r2 == 0 ? c.r1 : (c.r1 < c.r2 ? c.r1 : c.r2));
    return c;
}
} // namespace ext::ffxiv::ui
