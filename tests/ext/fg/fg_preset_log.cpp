// DLSS-G preset readout (ext/fg/dlssg/PresetLog.h): the tolerant parser of NVIDIA's [ngxLog] preset lines, on synthetic lines
// shaped like the real Streamline relay lines of builds\session2-correction-log-20261019, and the readout state behind the menu.
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetLog.h"

#include <initializer_list>
#include <string>

using namespace ext::fg::preset;

namespace
{
// The envelope of every relayed NVIDIA line in the real log (only the message after the last ']' differs).
std::string Ngx(const std::string& where, const std::string& message, const char* feature = "FrameGeneration")
{
    return std::string("SL Log: [22-09-37][streamline][info][tid:35512][1s:856ms:658us]commonEntry.cpp:1369[ngxLog] [") + feature +
           "] [2026-10-19 22:09:37] [tid:35512][" + where + "] " + message;
}

void RecognisedLines()
{
    {
        const auto p = ParseLine(Ngx("EndpointCore::ReportOverrideStates:932", "ReportOverrideStates: render preset (requested=1, applied=1, value=1)"));
        CHECK(p.kind == LineKind::Report && p.requested == 1 && p.applied == 1 && p.value == 1);
        CHECK(p.recognised);
    }
    {
        const auto p = ParseLine(Ngx("EndpointCore::ReportOverrideStates:932", "ReportOverrideStates: render preset (requested=1, applied=0, value=3)"));
        CHECK(p.kind == LineKind::Report && p.requested == 1 && p.applied == 0 && p.value == 3);
    }
    {
        const auto p = ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:154", "INFO: Preset ID: 2"));
        CHECK(p.kind == LineKind::PresetId && p.value == 2 && p.recognised);
    }
    {
        const auto p = ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:165", "INFO: Preset A selected, disabling UIR."));
        CHECK(p.kind == LineKind::Selected && p.letter == 'A' && p.uir == Uir::Disabled && p.recognised);
    }
    {
        const auto p = ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:171", "INFO: Preset B selected, enabling UIR."));
        CHECK(p.kind == LineKind::Selected && p.letter == 'B' && p.uir == Uir::Enabled);
    }
    {
        const auto p = ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:188", "INFO: Disabling bUIRecompositionSupported due to preset override."));
        CHECK(p.kind == LineKind::UirNote && p.uir == Uir::Disabled && p.recognised);
        const auto q = ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:188", "INFO: Enabling bUIRecompositionSupported due to preset override."));
        CHECK(q.kind == LineKind::UirNote && q.uir == Uir::Enabled);
    }
    {
        const auto p = ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:147", "INFO: Not parsing presets due to private flag overrides."));
        CHECK(p.kind == LineKind::BlockedByFlags && p.recognised);
    }
}

void Tolerance()
{
    // wording drift: case, spacing, separators, a missing value, hex numbers
    {
        const auto p = ParseLine(Ngx("X:1", "ReportOverrideStates:  RENDER PRESET  (requested = 2 ; applied = 1 )"));
        CHECK(p.kind == LineKind::Report && p.requested == 2 && p.applied == 1 && p.recognised);
    }
    {
        const auto p = ParseLine(Ngx("X:1", "render preset requested=3 applied=0 value=0x3"));
        CHECK(p.kind == LineKind::Report && p.requested == 3 && p.applied == 0 && p.value == 3);
    }
    {
        const auto p = ParseLine(Ngx("X:1", "INFO: preset id : 1"));
        CHECK(p.kind == LineKind::PresetId && p.value == 1);
    }
    {
        const auto p = ParseLine(Ngx("X:1", "INFO: Preset C selected"));
        CHECK(p.kind == LineKind::Selected && p.letter == 'C' && p.uir == Uir::Unknown);
    }
    // a preset line the parser does not know is kept as a candidate, not recognised
    {
        const auto p = ParseLine(Ngx("Y:9", "Using render preset override for frame generation"));
        CHECK(p.kind == LineKind::Candidate && !p.recognised);
    }
    // broken numbers never crash or invent values
    {
        const auto p = ParseLine(Ngx("X:1", "render preset (requested=, applied=zz, value=99999999999999999999)"));
        CHECK(p.kind == LineKind::Report && !p.recognised && p.requested == -1 && p.applied == -1);
    }
    {
        const auto p = ParseLine(Ngx("X:1", "INFO: Preset ID: abc"));
        CHECK(p.kind == LineKind::PresetId && !p.recognised);
    }
}

void Ignored()
{
    // lines of other features, other topics, other origins, and our own log lines
    CHECK(ParseLine(Ngx("Z:1", "Info: (DLAA) Using App hint Preset M", "SuperSampling")).kind == LineKind::None);
    CHECK(ParseLine(Ngx("Z:1", "Info: (Quality) Using Default Overriden Preset Preset_D", "RayReconstruction")).kind == LineKind::None);
    CHECK(ParseLine(Ngx("Z:1", "Loaded library from path (C:/x/nvngx_dlssg.dll)")).kind == LineKind::None);
    CHECK(ParseLine("DLSS-G render preset: the runtime asked the driver for its preset, answered 1 (Preset A)").kind == LineKind::None);
    // the PRESET-BOOT probe wording: which ids were answered and which passed through
    CHECK(ParseLine("DLSS-G render preset: the runtime asked the driver for its preset (id 0x10E41DF1), answered 3 (Preset C); the DLSS "
                    "override id 0x10E41E03 is passed through to the driver")
              .kind == LineKind::None);
    CHECK(ParseLine("DLSS-G preset line: SL Log: [..][ngxLog] [FrameGeneration] render preset (requested=1, applied=1, value=1)").kind ==
          LineKind::None);
    CHECK(ParseLine("").kind == LineKind::None);
    CHECK(ParseLine("preset").kind == LineKind::None); // no relay envelope
    CHECK(ParseLine("[ngxLog] render preset (requested=1, applied=1, value=1)").kind == LineKind::None);
}

void ReadoutState()
{
    Readout r;
    CHECK(!r.Snapshot().queried && !r.Snapshot().confirmed && r.Snapshot().lastLine.empty());

    r.NoteDriverQuery(true);
    CHECK(r.Snapshot().queried);

    bool log = r.Feed(Ngx("X:1", "INFO: Preset ID: 1"), 1000);
    CHECK(!log); // recognised lines are not logged as candidates
    auto s = r.Snapshot();
    CHECK(s.confirmed && s.lastLine.find("Preset ID: 1") != std::string::npos && s.presetId == 1);

    r.Feed(Ngx("X:1", "INFO: Preset A selected, disabling UIR."), 1001);
    s = r.Snapshot();
    CHECK(s.selected == 'A' && s.uir == Uir::Disabled);

    r.Feed(Ngx("X:1", "ReportOverrideStates: render preset (requested=1, applied=1, value=1)"), 1002);
    s = r.Snapshot();
    CHECK(s.hasReport && s.requested == 1 && s.applied == 1 && s.value == 1);

    r.Feed(Ngx("X:1", "INFO: Not parsing presets due to private flag overrides."), 1003);
    CHECK(r.Snapshot().blockedByFlags);

    // a new request clears what the earlier endpoint reported
    r.BeginRequest();
    s = r.Snapshot();
    CHECK(!s.confirmed && !s.hasReport && s.selected == 0 && s.uir == Uir::Unknown && !s.blockedByFlags && s.lastLine.empty());
    CHECK(s.queried);                  // that the runtime asked the driver once is history, not part of a request
    CHECK(!s.readSinceRequest);
    r.NoteCachedRead();
    CHECK(r.Snapshot().readSinceRequest);
    r.BeginRequest();
    CHECK(!r.Snapshot().readSinceRequest);
}

void CandidateLogging()
{
    Readout r;
    const auto unknown = Ngx("Y:9", "Using render preset override for frame generation");
    CHECK(r.Feed(unknown, 10000));   // first candidate: log it
    CHECK(!r.Feed(unknown, 10100));  // same instant burst: rate limited
    CHECK(!r.Feed(unknown, 11999));  // still inside the 2 s window
    CHECK(r.Feed(unknown, 12000));   // window over
    auto s = r.Snapshot();
    CHECK(!s.confirmed);             // a candidate never sets the mark
    CHECK(s.lastLine.find("Using render preset override") != std::string::npos); // but it is the last line shown

    // at most kMaxCandidateLogs candidate log lines per session, whatever the spacing
    Readout many;
    int logged = 0;
    for (int i = 0; i < 200; ++i)
        logged += many.Feed(unknown, 100000u + (uint64_t) i * 5000u) ? 1 : 0;
    CHECK(logged == kMaxCandidateLogs);

    // the stored line is capped and has no trailing newline
    Readout big;
    big.Feed(Ngx("Y:9", std::string("preset ") + std::string(5000, 'x') + "\r\n"), 5);
    const auto b = big.Snapshot();
    CHECK(b.lastLine.size() <= (size_t) kMaxLineChars && b.lastLine.find('\n') == std::string::npos && b.lastLine.find('\r') == std::string::npos);
}
// ---- what the runtime CONFIRMED (fix round 1, M1 and M5) -------------------------------------------------------------------
// ReportOverrideStates logs: requested = 1 when the driver cache holds a preset, applied = 1 when the runtime applied one, value =
// the preset number. The 310.9.1.0 runtime ignores value 3: "requested=1, applied=0, value=3".
Effect Confirm(Readout& r, int request) { return r.Confirm(request).effect; }

void ConfirmedEffect()
{
    {   // nothing heard: unknown, for every request
        Readout r;
        for (int req : { 1, 2, 3 })
            CHECK(Confirm(r, req) == Effect::Unknown);
        CHECK(Confirm(r, 0) == Effect::Applied); // Auto has nothing to confirm
    }
    {   // A applied
        Readout r;
        r.Feed(Ngx("X:1", "ReportOverrideStates: render preset (requested=1, applied=1, value=1)"), 1);
        CHECK(Confirm(r, 1) == Effect::Applied);
        CHECK(Confirm(r, 2) == Effect::Unknown); // a report about another preset says nothing about B
        CHECK(Confirm(r, 3) == Effect::Unknown);
    }
    {   // C ignored by the runtime (the 310.9.1.0 shape)
        Readout r;
        r.Feed(Ngx("X:1", "ReportOverrideStates: render preset (requested=1, applied=0, value=3)"), 1);
        CHECK(Confirm(r, 3) == Effect::NotApplied);
        CHECK(Confirm(r, 1) == Effect::Unknown);
    }
    {   // the driver cache did not hold a preset at all: the report is not about our request
        Readout r;
        r.Feed(Ngx("X:1", "ReportOverrideStates: render preset (requested=0, applied=0, value=0)"), 1);
        CHECK(Confirm(r, 3) == Effect::Unknown);
        Readout q; // the cache held nothing (requested=0) but a number is printed: still no statement about our request
        q.Feed(Ngx("X:1", "ReportOverrideStates: render preset (requested=0, applied=0, value=3)"), 1);
        CHECK(Confirm(q, 3) == Effect::Unknown);
    }
    {   // a "Preset B selected" line confirms B, and only B
        Readout r;
        r.Feed(Ngx("X:1", "INFO: Preset B selected, enabling UIR."), 1);
        CHECK(Confirm(r, 2) == Effect::Applied);
        CHECK(Confirm(r, 1) == Effect::Unknown && Confirm(r, 3) == Effect::Unknown);
        CHECK(r.Confirm(2).uir == Uir::Enabled);
    }
    {   // "Preset ID: 3" only says the runtime read the number
        Readout r;
        r.Feed(Ngx("X:1", "INFO: Preset ID: 3"), 1);
        CHECK(Confirm(r, 3) == Effect::Unknown);
    }
    {   // a new request forgets everything the earlier endpoint said
        Readout r;
        r.Feed(Ngx("X:1", "ReportOverrideStates: render preset (requested=1, applied=1, value=1)"), 1);
        r.BeginRequest();
        CHECK(Confirm(r, 1) == Effect::Unknown);
    }
}

// ---- the 310.9.16 runtime (fix round 2, item 4) ----------------------------------------------------------------------------------
// Strings of the real DLL (Streamline 2.14.16, sha256 8d59d49b...), read with a decompiler:
//   EndpointConfiguration::ApplyRenderPreset (endpoint_config.cpp:337)  "Applying settings for render preset %u."
//   EndpointConfiguration::ReadRegkeys (:136)                           "Warning: Ignoring unsupported render preset ID %u."
//   EndpointCore::ReportOverrideStates (:940)                           "ReportOverrideStates: render preset (requested=%u, effective=%u)"
// Nothing else with "preset" is left in that build: "Preset ID: %d", "Preset A selected, disabling UIR." and the bUIRecomposition
// notes of 310.9.1.0 are gone, and the report no longer has applied= / value=. ReadRegkeys handles value 3 (Preset C) there.
std::string Applying(int n) { return Ngx("EndpointConfiguration::ApplyRenderPreset:337", "Applying settings for render preset " + std::to_string(n) + "."); }
std::string Report31016(int requested, int effective)
{
    return Ngx("EndpointCore::ReportOverrideStates:940",
               "ReportOverrideStates: render preset (requested=" + std::to_string(requested) + ", effective=" + std::to_string(effective) + ")");
}
std::string Unsupported(int id) { return Ngx("EndpointConfiguration::ReadRegkeys:136", "Warning: Ignoring unsupported render preset ID " + std::to_string(id) + "."); }

void Runtime31016Lines()
{
    for (int n : { 1, 2, 3 })
    {
        const auto p = ParseLine(Applying(n));
        CHECK_MSG(p.kind == LineKind::Applying && p.recognised && p.hasValue && p.value == static_cast<uint64_t>(n), "applying %d: kind %d", n, static_cast<int>(p.kind));
        // preset A and C turn the UI recomposition off, B turns it on (ReadRegkeys stores 0x0100 at config+54h for 1 and 3, 0x0101 for 2)
        CHECK(p.uir == (n == 2 ? Uir::Enabled : Uir::Disabled));
    }
    CHECK(!ParseLine(Applying(7)).recognised);
    {
        const auto p = ParseLine(Ngx("EndpointConfiguration::ApplyRenderPreset:337", "Applying settings for render preset x."));
        CHECK(p.kind == LineKind::Applying && !p.recognised);
    }
    {
        const auto p = ParseLine(Unsupported(7));
        CHECK_MSG(p.kind == LineKind::Unsupported && p.recognised && p.hasValue && p.value == 7, "unsupported: kind %d", static_cast<int>(p.kind));
    }
    {   // the report: requested is the preset number the driver cache held, effective the one the endpoint ended up with
        const auto p = ParseLine(Report31016(3, 3));
        CHECK_MSG(p.kind == LineKind::Report && p.newFormat && p.recognised && p.requested == 3 && p.effective == 3 && p.hasValue && p.value == 3,
                  "report: kind %d requested %d effective %d", static_cast<int>(p.kind), p.requested, p.effective);
        CHECK(p.uir == Uir::Disabled);
        CHECK(ParseLine(Report31016(2, 2)).uir == Uir::Enabled);
        CHECK(ParseLine(Report31016(1, 1)).uir == Uir::Disabled);
        const auto q = ParseLine(Report31016(3, 1));
        CHECK(q.recognised && q.requested == 3 && q.effective == 1);
        const auto big = ParseLine(Report31016(16777215, 2)); // the driver sentinel for B
        CHECK(big.recognised && big.requested == 16777215);
        const auto broken = ParseLine(Ngx("X:1", "ReportOverrideStates: render preset (requested=3, effective=)"));
        CHECK(broken.kind == LineKind::Report && !broken.recognised);
    }
    // the other reports of the same function are not about the render preset
    CHECK(ParseLine(Ngx("EndpointCore::ReportOverrideStates:857", "ReportOverrideStates: multi-frame count (requested=1, applied=1, value=3)")).kind == LineKind::None);
    CHECK(ParseLine(Ngx("EndpointCore::ReportOverrideStates:904", "ReportOverrideStates: FG mode (requested=1, applied=0, value=2)")).kind == LineKind::None);
    // the private flag lines of ReadRegkeys: the preset is still applied, a flag overrides part of it (not "presets are ignored")
    for (const char* text : { "Disabling UI recomposition due to private flag override.", "Disabling FG Lite due to private flag override.",
                              "INFO: Disabling bUIRecompositionSupported due to private flag override." }) // the last one is the 310.9.1.0 wording
    {
        const auto p = ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:171", text));
        CHECK_MSG(p.kind == LineKind::FlagOverride && p.recognised, "%s: kind %d", text, static_cast<int>(p.kind));
        // a flag that forces the recomposition off says so; FG Lite says nothing about it
        const bool recomposition = std::string(text).find("FG Lite") == std::string::npos;
        CHECK_MSG(p.uir == (recomposition ? Uir::Disabled : Uir::Unknown), "%s: uir %d", text, static_cast<int>(p.uir));
    }
    CHECK(ParseLine(Ngx("EndpointConfiguration::ReadRegkeys:147", "INFO: Not parsing presets due to private flag overrides.")).kind == LineKind::BlockedByFlags);
}

void Runtime31016Confirmation()
{
    {   // C applied: the ApplyRenderPreset line confirms it, the recomposition is off
        Readout r;
        r.Feed(Applying(3), 1);
        CHECK(Confirm(r, 3) == Effect::Applied && r.Confirm(3).uir == Uir::Disabled);
        CHECK(Confirm(r, 1) == Effect::Unknown && Confirm(r, 2) == Effect::Unknown);
        const auto s = r.Snapshot();
        CHECK(s.confirmed && s.appliedLine == 3);
        r.Feed(Report31016(3, 3), 2);
        CHECK(Confirm(r, 3) == Effect::Applied);
        const auto t = r.Snapshot();
        CHECK(t.hasReport && t.reportNew && t.requested == 3 && t.effective == 3 && t.value == 3);
    }
    {   // the report alone is enough
        Readout r;
        r.Feed(Report31016(1, 1), 1);
        CHECK(Confirm(r, 1) == Effect::Applied && r.Confirm(1).uir == Uir::Disabled);
        Readout b;
        b.Feed(Report31016(2, 2), 1);
        CHECK(Confirm(b, 2) == Effect::Applied && b.Confirm(2).uir == Uir::Enabled);
    }
    {   // the endpoint ended up with another preset: not applied (the final report wins over the earlier "Applying" line)
        Readout r;
        r.Feed(Applying(3), 1);
        r.Feed(Report31016(3, 1), 2);
        CHECK(Confirm(r, 3) == Effect::NotApplied);
        CHECK(Confirm(r, 1) == Effect::Unknown); // requested was 3
    }
    {   // an id the runtime ignores
        Readout r;
        r.Feed(Unsupported(3), 1);
        CHECK(Confirm(r, 3) == Effect::NotApplied);
        CHECK(Confirm(r, 2) == Effect::Unknown);
        const auto s = r.Snapshot();
        CHECK(s.unsupportedId == 3 && s.confirmed);
        CHECK(s.presetId == -1); // not read as "Preset ID: N"
    }
    {   // a private flag line: shown as an override, never as "presets ignored", and it does not unconfirm anything
        Readout r;
        r.Feed(Applying(3), 1);
        r.Feed(Ngx("EndpointConfiguration::ReadRegkeys:171", "Disabling FG Lite due to private flag override."), 2);
        const auto s = r.Snapshot();
        CHECK(s.flagOverride && !s.blockedByFlags && s.confirmed); // confirmed by the Applying line, not by the flag note
        CHECK(Confirm(r, 3) == Effect::Applied);
        CHECK(r.Confirm(3).uir == Uir::Disabled); // from the Applying line; FG Lite does not change it
        Readout q;
        q.Feed(Ngx("EndpointConfiguration::ReadRegkeys:171", "Disabling UI recomposition due to private flag override."), 1);
        CHECK(q.Snapshot().flagOverride && q.Confirm(1).uir == Uir::Disabled); // a private flag forces the recomposition off
        r.BeginRequest();
        CHECK(!r.Snapshot().flagOverride);
    }
    {   // a new request forgets all of it
        Readout r;
        r.Feed(Applying(3), 1);
        r.Feed(Report31016(3, 3), 2);
        r.Feed(Unsupported(5), 3);
        r.BeginRequest();
        const auto s = r.Snapshot();
        CHECK(Confirm(r, 3) == Effect::Unknown && !s.hasReport && !s.reportNew && s.effective == -1 && s.appliedLine == -1 && s.unsupportedId == -1);
    }
    {   // the whole chain for the recomposition rule: C pauses on this runtime, B never, A does
        Readout c;
        c.Feed(Applying(3), 1);
        CHECK(PausesUiRecomposition(kPresetC, c.Confirm(3).effect, c.Confirm(3).uir));
        Readout a;
        a.Feed(Applying(1), 1);
        CHECK(PausesUiRecomposition(kPresetA, a.Confirm(1).effect, a.Confirm(1).uir));
        Readout b;
        b.Feed(Applying(2), 1);
        CHECK(!PausesUiRecomposition(kPresetB, b.Confirm(2).effect, b.Confirm(2).uir));
        // C requested but the runtime reports another preset: nothing is paused
        Readout n;
        n.Feed(Report31016(3, 2), 1);
        CHECK(!PausesUiRecomposition(kPresetC, n.Confirm(3).effect, n.Confirm(3).uir));
        // C ignored as unsupported: nothing is paused
        Readout u;
        u.Feed(Unsupported(3), 1);
        CHECK(!PausesUiRecomposition(kPresetC, u.Confirm(3).effect, u.Confirm(3).uir));
    }
}

// ---- NGX's own override report (the maintainer's in-game log of 2026-10-20, lines 3533 and 12387) ----------------------------------
const char* kOverride3 = "SL Log: [22-31-55][streamline][info][tid:41820][17s:979ms:467us]commonEntry.cpp:1368[ngxLog] [Unknown(32765)] [2026-10-20 22:31:55] [NGXOverrideStatusCallback:2189] Feature [FrameGeneration] Override Reported: ModelPreset, Value: 3, Requested: Yes, Applied: Yes, Is Latest: No";
std::string Override(int value, const char* requested, const char* applied, const char* feature = "FrameGeneration")
{
    return std::string("SL Log: [22-33-40][streamline][info][tid:41820][122s:513ms:888us]commonEntry.cpp:1368[ngxLog] [Unknown(32765)] [2026-10-20 22:33:40] [NGXOverrideStatusCallback:2189] Feature [") + feature +
           "] Override Reported: ModelPreset, Value: " + std::to_string(value) + ", Requested: " + requested + ", Applied: " + applied + ", Is Latest: No";
}

void NgxOverrideReport()
{
    {
        const auto p = ParseLine(kOverride3);
        CHECK_MSG(p.kind == LineKind::OverrideReport && p.recognised && p.hasValue && p.value == 3 && p.requested == 1 && p.applied == 1, "kind %d", static_cast<int>(p.kind));
    }
    {
        const auto p = ParseLine(Override(1, "Yes", "No"));
        CHECK(p.kind == LineKind::OverrideReport && p.recognised && p.value == 1 && p.requested == 1 && p.applied == 0);
        const auto q = ParseLine(Override(2, "No", "No"));
        CHECK(q.kind == LineKind::OverrideReport && q.recognised && q.requested == 0 && q.applied == 0);
        const auto lower = ParseLine(Override(3, "yes", "yes"));
        CHECK(lower.recognised && lower.applied == 1);
        const auto broken = ParseLine(Override(3, "Yes", "Maybe"));
        CHECK(broken.kind == LineKind::OverrideReport && !broken.recognised);
    }
    // other features report their own presets: not this line
    CHECK(ParseLine(Override(1, "Yes", "Yes", "SuperSampling")).kind == LineKind::None);
    CHECK(ParseLine(Override(1, "Yes", "Yes", "RayReconstruction")).kind == LineKind::None);
    CHECK(ParseLine(Override(1, "Yes", "Yes", "DeepDVC")).kind != LineKind::OverrideReport); // only frame generation's own report counts
    {
        Readout w; // reported as applied although nobody asked for it: no confirmation
        w.Feed(Override(3, "No", "Yes"), 1);
        CHECK(Confirm(w, 3) == Effect::Unknown);
    }
    {   // confirming, and "did not apply" when NGX says Applied: No
        Readout r;
        r.Feed(kOverride3, 1);
        CHECK(Confirm(r, 3) == Effect::Applied && Confirm(r, 1) == Effect::Unknown && r.Snapshot().confirmed);
        CHECK(StatusSentence(3, Confirm(r, 3), true).find("In use") == 0);
        CHECK(r.Confirm(3).uir == Uir::Unknown); // the line says nothing about the recomposition: C does not pause on it alone
        CHECK(!PausesUiRecomposition(kPresetC, r.Confirm(3).effect, r.Confirm(3).uir));
        Readout n;
        n.Feed(Override(3, "Yes", "No"), 1);
        CHECK(Confirm(n, 3) == Effect::NotApplied);
        const auto s = StatusSentence(3, Confirm(n, 3), true);
        CHECK(s.find("did not apply") != std::string::npos && s.find("In use") == std::string::npos);
        Readout m; // Applied: No outranks an earlier Applying line
        m.Feed(Applying(3), 1);
        m.Feed(Override(3, "Yes", "No"), 2);
        CHECK(Confirm(m, 3) == Effect::NotApplied);
        Readout q; // a report about another preset says nothing about C
        q.Feed(Override(1, "Yes", "No"), 1);
        CHECK(Confirm(q, 3) == Effect::Unknown);
        Readout b; // the final ReportOverrideStates still outranks a Yes
        b.Feed(Override(3, "Yes", "Yes"), 1);
        b.Feed(Report31016(3, 1), 2);
        CHECK(Confirm(b, 3) == Effect::NotApplied);
        r.BeginRequest();
        CHECK(Confirm(r, 3) == Effect::Unknown && r.Snapshot().overrideValue == -1);
    }
}

// review 3, L6: a report about another preset is a contradiction, not a confirmation; a flag note is not a preset line
void ReadoutWording()
{
    Readout r;
    r.Feed(Applying(3), 1);
    r.Feed(Report31016(1, 1), 2);
    CHECK(Confirm(r, 3) == Effect::Unknown);
    Readout f;
    f.Feed(Ngx("EndpointConfiguration::ReadRegkeys:176", "Disabling FG Lite due to private flag override."), 1);
    CHECK(f.Snapshot().flagOverride && !f.Snapshot().confirmed);
}

void PauseRule()
{
    // A is documented to turn UI recomposition off: paused unless the runtime says otherwise
    CHECK(PausesUiRecomposition(kPresetA, Effect::Unknown, Uir::Unknown));
    CHECK(PausesUiRecomposition(kPresetA, Effect::Applied, Uir::Disabled));
    CHECK(!PausesUiRecomposition(kPresetA, Effect::NotApplied, Uir::Unknown));
    CHECK(!PausesUiRecomposition(kPresetA, Effect::Applied, Uir::Enabled));
    // C only when the runtime confirmed that it acts on C AND turned the recomposition off
    CHECK(!PausesUiRecomposition(kPresetC, Effect::Unknown, Uir::Unknown));
    CHECK(!PausesUiRecomposition(kPresetC, Effect::NotApplied, Uir::Unknown)); // the 310.9.1.0 case: nothing is paused
    CHECK(!PausesUiRecomposition(kPresetC, Effect::Applied, Uir::Unknown));
    CHECK(!PausesUiRecomposition(kPresetC, Effect::Applied, Uir::Enabled));
    CHECK(PausesUiRecomposition(kPresetC, Effect::Applied, Uir::Disabled));
    // B and Auto never pause
    for (auto e : { Effect::Unknown, Effect::Applied, Effect::NotApplied })
        for (auto u : { Uir::Unknown, Uir::Disabled, Uir::Enabled })
            CHECK(!PausesUiRecomposition(kPresetB, e, u) && !PausesUiRecomposition(kAuto, e, u) && !PausesUiRecomposition(7, e, u));
}

void StatusSentences()
{
    auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };
    CHECK(StatusSentence(kAuto, Effect::Applied, true) == "In use: Auto");
    // never "in use" for a preset the runtime did not confirm
    for (int req : { 1, 2, 3 })
    {
        const auto waiting = StatusSentence(req, Effect::Unknown, true);
        CHECK(!has(waiting, "In use") && has(waiting, "waiting for the runtime to confirm"));
        const auto blind = StatusSentence(req, Effect::Unknown, false);
        CHECK(!has(blind, "In use") && has(blind, "cannot be read here"));
        CHECK(has(StatusSentence(req, Effect::Applied, true), "In use:") && has(StatusSentence(req, Effect::Applied, true), "confirmed"));
        const auto no = StatusSentence(req, Effect::NotApplied, true);
        CHECK(!has(no, "In use") && has(no, "did not apply") && !has(no, "applied=0"));
    }
    CHECK(has(StatusSentence(kPresetC, Effect::Unknown, true), "Preset C"));
}

// ---- the boot of 2026-10-22 (PRESET-BOOT phase 1, item 1): lines copied from builds\game-logs-20261021-probe\OptiScaler.log ------------
// Boot with [DLSSG] RenderPreset = 3: the runtime logged "Applying settings for render preset 3." (line 2171) and NOTHING else about the
// preset: no "ReportOverrideStates: render preset (...)" and no "Override Reported: ModelPreset" (those two only showed up in the runs in
// which the DLSS override id was answered on, i.e. NGX loaded NVIDIA's model-store module; the live path of bisect3 prints none either). The payloads below are what the logger hands to the relay sink: the file prefix
// "[hh:mm:ss.uuuuuu] [I] " is the log pattern and is not part of the payload.
const char* kRealApplying3 =
    "SL Log: [10-31-35][streamline][info][tid:19444][21s:053ms:194us]commonEntry.cpp:1369[ngxLog] [FrameGeneration] [2026-10-22 10:31:35] "
    "[tid:19444][EndpointConfiguration::ApplyRenderPreset:337] Applying settings for render preset 3.";
const char* kRealInit =
    "SL Log: [10-31-16][streamline][info][tid:19444][2s:220ms:806us]commonEntry.cpp:1368[ngxLog] [FrameGeneration] [2026-10-22 10:31:16] "
    "[tid:19444][NGXInitLog:216] Built with APP_NAME = endpoint";
const char* kRealMultiFrame =
    "SL Log: [10-31-16][streamline][info][tid:19444][2s:492ms:192us]dlss_gEntry.cpp:1504[slOnPluginStartup] Multi-frame supported, max generated frames 5 "
    "(SL Plugin supports 5, NGX feature supports 5)";
const char* kRealOurQuery =
    "DLSS-G render preset: the runtime asked the driver for its preset (id 0x10E41DF1), answered 3 (Preset C) (the driver profile has no such setting); "
    "the DLSS override id 0x10E41E03 is passed through to the driver";

void BootReadoutStates()
{
    auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };
    // The relay feeds every message; the lines around the preset line must not matter.
    auto feedBoot = [&](Readout& r, bool applying)
    {
        r.NoteDriverQuery(true);
        r.Feed(kRealInit, 1);
        r.Feed(kRealOurQuery, 2); // our own text is never mistaken for an NVIDIA preset line
        r.Feed(kRealMultiFrame, 3);
        if (applying)
            r.Feed(kRealApplying3, 4);
    };

    {   // 1) only "Applying settings for render preset 3." seen (the boot with the preset id answered alone)
        Readout r;
        feedBoot(r, true);
        const auto p = ParseLine(kRealApplying3);
        CHECK_MSG(p.kind == LineKind::Applying && p.recognised && p.value == 3, "real line: kind %d", static_cast<int>(p.kind));
        CHECK(Confirm(r, 3) == Effect::Applied && r.Confirm(3).uir == Uir::Disabled);
        CHECK(Confirm(r, 1) == Effect::Unknown && Confirm(r, 2) == Effect::Unknown);
        const auto s = r.Snapshot();
        CHECK(s.confirmed && s.appliedLine == 3 && s.queried);
        // nothing that the menu draws as a failure, a refusal or "ignored" is set by the absence of the two reports
        CHECK(!s.hasReport && !s.reportNew && s.unsupportedId == -1 && !s.blockedByFlags && !s.flagOverride);
        CHECK(s.overrideValue == -1 && s.overrideRequested == -1 && s.overrideApplied == -1);
        const auto sentence = StatusSentence(3, Confirm(r, 3), true);
        CHECK(sentence == "In use: Preset C (confirmed by the runtime)");
        CHECK(PausesUiRecomposition(kPresetC, r.Confirm(3).effect, r.Confirm(3).uir));
    }
    {   // 2) a run that ALSO prints the reports: that is the override-on / model-store run of 2026-10-21 (bisect2); the good boots and the
        //    live path of bisect3 print only the Applying line. Applying plus the final report and NGX's own override report: same verdict
        Readout r;
        feedBoot(r, true);
        r.Feed(Report31016(3, 3), 5);
        r.Feed(kOverride3, 6);
        CHECK(Confirm(r, 3) == Effect::Applied);
        const auto s = r.Snapshot();
        CHECK(s.hasReport && s.reportNew && s.requested == 3 && s.effective == 3 && s.overrideValue == 3 && s.overrideApplied == 1);
        CHECK(StatusSentence(3, Confirm(r, 3), true) == "In use: Preset C (confirmed by the runtime)");
    }
    {   // 3) nothing seen: unconfirmed, said as such, never "in use", never "did not apply"
        Readout r;
        feedBoot(r, false);
        CHECK(Confirm(r, 3) == Effect::Unknown);
        const auto s = r.Snapshot();
        CHECK(!s.confirmed && s.lastLine.empty() && s.appliedLine == -1 && !s.hasReport && s.unsupportedId == -1);
        const auto sentence = StatusSentence(3, Confirm(r, 3), true);
        CHECK(has(sentence, "waiting for the runtime to confirm") && !has(sentence, "In use") && !has(sentence, "did not apply"));
        CHECK(!PausesUiRecomposition(kPresetC, r.Confirm(3).effect, r.Confirm(3).uir));
    }
    {   // the real line belongs to the request it names only
        Readout r;
        feedBoot(r, true);
        CHECK(!has(StatusSentence(1, Confirm(r, 1), true), "In use"));
        r.BeginRequest(); // a live switch forgets the boot line
        CHECK(Confirm(r, 3) == Effect::Unknown && r.Snapshot().queried);
    }
}

// ---- PRESET-BOOT fix round, item 5 (readout precedence): which statement wins when the runtime says two things about one request --------
// Lines of builds\game-logs-20261021-bisect2\OptiScaler.log (the override-on run, the only one that prints the reports), payloads as the relay
// sink gets them; the order in that log is Applying (2170), ReportOverrideStates (3700), Override Reported (3703), all within 70 ms.
const char* kRealReport33 =
    "SL Log: [10-39-59][streamline][info][tid:26056][18s:192ms:907us]commonEntry.cpp:1369[ngxLog] [FrameGeneration] [2026-10-21 10:39:59] "
    "[tid:26056][EndpointCore::ReportOverrideStates:940] ReportOverrideStates: render preset (requested=3, effective=3)";
const char* kRealOverrideYes =
    "SL Log: [10-39-59][streamline][info][tid:26056][18s:192ms:929us]commonEntry.cpp:1368[ngxLog] [Unknown(32765)] [2026-10-21 10:39:59] "
    "[NGXOverrideStatusCallback:2189] Feature [FrameGeneration] Override Reported: ModelPreset, Value: 3, Requested: Yes, Applied: Yes, Is Latest: No";
std::string RealOverrideNo() // the same real line with the answer flipped: no log has an `Applied: No` yet, its meaning is unverified
{
    std::string s = kRealOverrideYes;
    const auto at = s.find("Applied: Yes");
    s.replace(at, 12, "Applied: No");
    return s;
}

// ---- PRESET-BOOT fix round, item 2: NVIDIA's model-store copy of the runtime, from NGX's init line ---------------------------------------
// Real payloads. nvapp_override: builds\game-logs-20261021-bisect2 line 1141 (the run in which the DLSS override id was answered on, NGX
// loaded the model-store module, max generated frames 1). endpoint: the probe run (local OptiScaler/nvngx_dlssg.dll, max 5), kRealInit above.
// The other two init lines of the same boots belong to the super resolution feature (APP_NAME = default / app_transformer_dlssd).
const char* kRealInitNvapp =
    "SL Log: [10-39-43][streamline][info][tid:26056][1s:966ms:897us]commonEntry.cpp:1368[ngxLog] [FrameGeneration] [2026-10-21 10:39:43] "
    "[tid:26056][NGXInitLog:216] Built with APP_NAME = nvapp_override";
const char* kRealInitSrDefault =
    "SL Log: [10-39-43][streamline][info][tid:26056][1s:675ms:129us]commonEntry.cpp:1368[ngxLog] [SuperSampling] [2026-10-21 10:39:43] "
    "[tid:26056][NGXInitLog:216] Built with APP_NAME = default";
const char* kRealInitSrTransformer =
    "SL Log: [10-39-43][streamline][info][tid:26056][2s:091ms:867us]commonEntry.cpp:1368[ngxLog] [SuperSampling] [2026-10-21 10:39:43] "
    "[tid:26056][NGXInitLog:216] Built with APP_NAME = app_transformer_dlssd";
const char* kRealMultiFrameOne =
    "SL Log: [10-39-43][streamline][info][tid:26056][2s:219ms:072us]dlss_gEntry.cpp:1504[slOnPluginStartup] Multi-frame not supported, max generated "
    "frames 1 (SL Plugin supports 5, NGX feature supports 1)";

void ModelStoreCopy()
{
    // only the frame generation feature's init line with the model-store app name is the signal
    {
        const auto p = ParseLine(kRealInitNvapp);
        CHECK_MSG(p.kind == LineKind::NvidiaCopy && p.recognised, "kind %d", static_cast<int>(p.kind));
    }
    CHECK(ParseLine(kRealInit).kind == LineKind::None);            // FrameGeneration, APP_NAME = endpoint (the local runtime)
    CHECK(ParseLine(kRealInitSrDefault).kind == LineKind::None);   // other features, other app names
    CHECK(ParseLine(kRealInitSrTransformer).kind == LineKind::None);
    CHECK(ParseLine(kRealMultiFrameOne).kind == LineKind::None);
    // the same app name printed by another feature (super resolution) is not about frame generation
    std::string sr = kRealInitNvapp;
    sr.replace(sr.find("[FrameGeneration]"), 17, "[SuperSampling]");
    CHECK(ParseLine(sr).kind == LineKind::None);
    CHECK(ParseLine("DLSS-G note: Built with APP_NAME = nvapp_override").kind == LineKind::None); // not a relayed NVIDIA line

    // the readout
    for (const char* other : { kRealInit, kRealInitSrDefault, kRealInitSrTransformer, kRealMultiFrameOne })
    {
        Readout r;
        r.Feed(other, 1);
        CHECK(!r.Snapshot().nvidiaCopy && !r.TakeNvidiaCopyWarning() && !r.NvidiaCopyLoaded());
    }
    Readout r;
    r.Feed(kRealInit, 1);
    r.Feed(kRealApplying3, 2);
    CHECK(!r.NvidiaCopyLoaded());
    CHECK(!r.Feed(kRealInitNvapp, 3)); // never a "candidate line"
    CHECK(r.NvidiaCopyLoaded() && r.Snapshot().nvidiaCopy);
    // it is not an NVIDIA preset line: the menu's last line and the "confirmed" mark are those of the preset lines
    const auto s = r.Snapshot();
    CHECK(s.lastLine.find("Applying settings for render preset 3.") != std::string::npos && s.lastLine.find("APP_NAME") == std::string::npos);
    Readout alone;
    alone.Feed(kRealInitNvapp, 1);
    const auto a = alone.Snapshot();
    CHECK(a.nvidiaCopy && a.lastLine.empty() && !a.confirmed && a.appliedLine == -1 && !a.hasReport);
    CHECK(alone.Confirm(3).effect == Effect::Unknown); // the line says nothing about a preset
    // the module is loaded once per process: the next request must not forget it
    r.BeginRequest();
    CHECK(r.NvidiaCopyLoaded() && r.Snapshot().nvidiaCopy && r.Snapshot().lastLine.empty());
    // one warning, however often the line comes
    CHECK(r.TakeNvidiaCopyWarning());
    CHECK(!r.TakeNvidiaCopyWarning());
    r.Feed(kRealInitNvapp, 4);
    CHECK(!r.TakeNvidiaCopyWarning() && r.NvidiaCopyLoaded());
}

void ConfirmationPrecedence()
{
    CHECK(ParseLine(kRealReport33).kind == LineKind::Report && ParseLine(kRealReport33).newFormat && ParseLine(kRealReport33).recognised);
    CHECK(ParseLine(RealOverrideNo()).kind == LineKind::OverrideReport && ParseLine(RealOverrideNo()).applied == 0);
    {   // the order of the real burst, Yes at the end
        Readout r;
        r.Feed(kRealApplying3, 1);
        r.Feed(kRealReport33, 2);
        r.Feed(kRealOverrideYes, 3);
        CHECK(Confirm(r, 3) == Effect::Applied);
    }
    {   // NGX's Applied: No alone: not applied (as before)
        Readout r;
        r.Feed(RealOverrideNo(), 1);
        CHECK(Confirm(r, 3) == Effect::NotApplied && r.Snapshot().confirmed);
    }
    {   // a LATER final report that says the requested preset is in force beats an earlier Applied: No
        Readout r;
        r.Feed(RealOverrideNo(), 1);
        r.Feed(kRealReport33, 2);
        CHECK_MSG(Confirm(r, 3) == Effect::Applied, "effect %d", static_cast<int>(Confirm(r, 3)));
        const auto s = r.Snapshot();
        CHECK(s.confirmed && s.overrideApplied == 0 && s.hasReport && s.effective == 3);
        CHECK(StatusSentence(3, Confirm(r, 3), true) == "In use: Preset C (confirmed by the runtime)");
    }
    {   // the older wording of the final report counts the same way
        Readout r;
        r.Feed(RealOverrideNo(), 1);
        r.Feed(Ngx("EndpointCore::ReportOverrideStates:932", "ReportOverrideStates: render preset (requested=1, applied=1, value=3)"), 2);
        CHECK(Confirm(r, 3) == Effect::Applied);
    }
    {   // a later Applying line counts too
        Readout r;
        r.Feed(RealOverrideNo(), 1);
        r.Feed(kRealApplying3, 2);
        CHECK(Confirm(r, 3) == Effect::Applied);
    }
    {   // Applied: No wins when nothing later says otherwise: it came last, or the later report names another preset in force
        Readout last;
        last.Feed(kRealApplying3, 1);
        last.Feed(kRealReport33, 2);
        last.Feed(RealOverrideNo(), 3);
        CHECK(Confirm(last, 3) == Effect::NotApplied);
        Readout other;
        other.Feed(RealOverrideNo(), 1);
        other.Feed(Report31016(3, 1), 2);
        CHECK(Confirm(other, 3) == Effect::NotApplied);
        Readout aboutAnother; // a later report about another preset is no evidence for this one
        aboutAnother.Feed(RealOverrideNo(), 1);
        aboutAnother.Feed(Report31016(1, 1), 2);
        CHECK(Confirm(aboutAnother, 3) == Effect::NotApplied);
    }
    {   // a new request forgets the order together with the lines
        Readout r;
        r.Feed(kRealReport33, 1);
        r.BeginRequest();
        r.Feed(RealOverrideNo(), 2);
        CHECK(Confirm(r, 3) == Effect::NotApplied);
    }
}

} // namespace

int main()
{
    RecognisedLines();
    Tolerance();
    Ignored();
    ReadoutState();
    CandidateLogging();
    ConfirmedEffect();
    Runtime31016Lines();
    Runtime31016Confirmation();
    NgxOverrideReport();
    ConfirmationPrecedence();
    ModelStoreCopy();
    ReadoutWording();
    PauseRule();
    StatusSentences();
    BootReadoutStates();
    if (g_failures)
    {
        std::printf("fg_preset_log: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_log: recognised lines, tolerance, ignored lines, readout state, candidate rate limit passed");
    return 0;
}
