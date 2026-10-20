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
} // namespace

int main()
{
    RecognisedLines();
    Tolerance();
    Ignored();
    ReadoutState();
    CandidateLogging();
    if (g_failures)
    {
        std::printf("fg_preset_log: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_log: recognised lines, tolerance, ignored lines, readout state, candidate rate limit passed");
    return 0;
}
