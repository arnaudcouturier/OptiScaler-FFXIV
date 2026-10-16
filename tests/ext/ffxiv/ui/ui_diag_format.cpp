// S10-A: the D1 file formats round-trip (shaders.csv, frames.txt), run-folder helpers, the byte budget.
#include "check.h"
#include "ext/ffxiv/ui/UiDiagFormat.h"

#include <ctime>

using namespace ext::ffxiv::ui;
using namespace ext::ffxiv::ui::fmt;

namespace
{
ShaderRow Hooked(uint32_t crc, uint32_t bytes, uint8_t stage, bool ui)
{
    ShaderRow r;
    r.id = Hex32(crc);
    r.hasCrc = true;
    r.crc = crc;
    r.bytes = bytes;
    r.stage = stage;
    r.ui = ui;
    r.instances = 2;
    r.draws = 123456789012ull;
    r.firstFrame = 17;
    r.rtSet = { { 3840, 2160, 28 }, { 1920, 1080, 10 }, { 512, 512, 999 } }; // 999 has no name: round-trips as fmt999
    r.rtOverflow = 4;
    r.rtSamples = 40;
    r.bbSamples = 10;
    r.tagAgree = 1;
    return r;
}

void ShadersCsvRoundTrip()
{
    std::vector<ShaderRow> rows;
    rows.push_back(Hooked(0x85E777EFu, 1512, 1, false));
    rows.push_back(Hooked(3865947726u, 2048, kUiStage, true));
    rows.push_back(Hooked(0x12345678u, 100, kStageNone, false));

    ShaderRow pre; // created before the hook
    pre.id = "pre#3";
    pre.hasCrc = false;
    pre.stage = kStageUnknown;
    pre.preHook = true;
    pre.instances = 1;
    pre.draws = 9;
    pre.firstFrame = 1;
    pre.rtSet = { { 1280, 720, 87 } };
    pre.rtSamples = 9;
    pre.bbSamples = 9;
    rows.push_back(pre);

    ShaderRow never; // never drawn, no render target sample at all
    never.id = Hex32(7);
    never.hasCrc = true;
    never.crc = 7;
    never.bytes = 7;
    never.instances = 1;
    rows.push_back(never);

    const std::string text = ShadersCsv(rows);
    const auto back = ParseShadersCsv(text);
    CHECK_EQ(back.size(), rows.size());
    for (size_t i = 0; i < back.size() && i < rows.size(); ++i)
        CHECK(back[i] == rows[i]);

    // Re-serialising the parsed rows gives the identical text (stable format).
    CHECK(ShadersCsv(back) == text);

    // Derived column and header.
    CHECK(text.find("bb_share_pct") != std::string::npos);
    CHECK(ShaderCsvRow(rows[0]).ends_with(",25.0"));
    CHECK(ShaderCsvRow(never).ends_with(",0.0"));
    CHECK(ShaderCsvRow(pre).starts_with("pre#3,?,?,?,0,1,"));

    // Garbage is rejected, not half parsed.
    ShaderRow bad;
    CHECK(!ParseShaderCsvRow("only,three,fields", bad));
    CHECK(!ParseShaderCsvRow("a,0xZZ,1,1,0,0,0,1,1,1,-,0,0,0,0.0", bad));
}

FrameRecord SampleFrame()
{
    FrameRecord r;
    r.frame = 4242;
    r.reason = "first600";
    auto& s = r.summary;
    s.draws = 3100;
    s.sawSceneStage = true;
    s.stageDraws[1] = 1;
    s.stageDraws[2] = 2;
    s.stageDraws[3] = 1;
    s.stageDraws[4] = 1;
    s.stageDraws[5] = 3;
    s.sceneStageDraws = 8;
    s.uiDraws = 211;
    s.unknownDraws = 5;
    s.lastSceneStage = 2890;
    s.lastStage5 = 2889;
    s.firstUi = 2893;
    s.uiBeforeLastScene = 4;
    s.r1Supersessions = 2;
    s.r2Supersessions = 1;
    s.candidateMoves = 3;
    r.candidates = { 2893, 2890, 2890, 2890 };
    r.presentDraws = 14;
    r.omittedDraws = 150;
    r.omittedPresent = 6;
    r.draws.push_back({ 2889, 5, RtKind::Backbuffer, { 3840, 2160, 87 }, false, false, false, false });
    r.draws.push_back({ 2890, kStageNone, RtKind::Backbuffer, { 3840, 2160, 87 }, false, true, true, true });
    r.draws.push_back({ 2893, kUiStage, RtKind::Backbuffer, { 3840, 2160, 87 }, true, false, false, false });
    r.draws.push_back({ 2900, kStageUnknown, RtKind::Other, { 256, 256, 28 }, false, false, false, false });
    r.draws.push_back({ 2901, 3, RtKind::SameAsBackbuffer, { 3840, 2160, 87 }, false, false, false, false });
    r.draws.push_back({ 2902, kUiStage, RtKind::None, {}, false, false, false, false });
    r.present.push_back({ 1, "0xAABBCCDD", kUiStage, RtKind::Backbuffer, { 3840, 2160, 87 } });
    r.present.push_back({ 2, "pre#7", kStageUnknown, RtKind::Other, { 64, 64, 28 } });
    return r;
}

void FramesRoundTrip()
{
    FrameRecord a = SampleFrame();
    FrameRecord b = a;
    b.frame = 4243;
    b.reason.clear(); // "-" in the file
    b.draws.clear();
    b.present.clear();
    b.omittedDraws = b.omittedPresent = 0;

    const std::string text = "# header\n" + FrameRecordText(a) + FrameRecordText(b);
    const auto back = ParseFrames(text);
    CHECK_EQ(back.size(), 2u);
    if (back.size() == 2)
    {
        CHECK(back[0] == a);
        CHECK(back[1] == b);
    }
    // Serialising the parsed records gives the same text.
    std::string again = "# header\n";
    for (const auto& r : back)
        again += FrameRecordText(r);
    CHECK(again == text);

    // The documented head fields exist.
    CHECK(text.find("r1=2893 r2=2890 r2any=2890 r3=2890") != std::string::npos);
    CHECK(text.find("  d 2890 - BB 3840x2160/B8G8R8A8_UNORM R2 R2any R3\n") != std::string::npos);
    CHECK(text.find("  p 2 pre#7 ?? OTHER 64x64/R8G8B8A8_UNORM\n") != std::string::npos);
}

void FrameSelection()
{
    CHECK(LogThisFrame(1));
    CHECK(LogThisFrame(600));
    CHECK(!LogThisFrame(601));
    CHECK(!LogThisFrame(699));
    CHECK(LogThisFrame(700));
    CHECK(LogThisFrame(7300));
    CHECK(!LogThisFrame(7301));
}

void RunFolders()
{
    std::tm t {};
    t.tm_year = 126;
    t.tm_mon = 9;
    t.tm_mday = 16;
    t.tm_hour = 21;
    t.tm_min = 5;
    t.tm_sec = 9;
    CHECK(FormatRunId(t) == "20261016-210509");
    CHECK(IsRunId("20261016-210509"));
    CHECK(!IsRunId("20261016210509"));
    CHECK(!IsRunId("2026101a-210509"));
    CHECK(!IsRunId("snapshots"));

    std::vector<std::string> names { "20260101-000001", "20260103-000001", "notes", "20260102-000001", "20260105-000001",
                                     "20260104-000001", "20260106-000001", "20260107-000001", "keepme.txt" };
    const auto pruned = RunsToPrune(names);
    CHECK_EQ(pruned.size(), 2u); // 7 runs, keep 5: the two oldest
    if (pruned.size() == 2)
    {
        CHECK(pruned[0] == "20260101-000001");
        CHECK(pruned[1] == "20260102-000001");
    }
    CHECK(RunsToPrune({ "20260101-000001", "20260102-000001" }).empty());
    CHECK(RunsToPrune({ "a", "b", "c" }).empty()); // never lists what it did not create
}

void Budget()
{
    ByteBudget b;
    b.limit = 100;
    CHECK(b.Take(60));
    CHECK(!b.truncated);
    CHECK(!b.Take(41)); // would exceed
    CHECK(b.truncated);
    CHECK(!b.Take(1)); // stays refused: no records after the marker
    CHECK_EQ(b.used, 60u);
}

void Rolling_()
{
    Rolling r;
    r.Add(3);
    r.Add(1);
    r.Add(8);
    CHECK_EQ(r.count, 3u);
    CHECK(r.min == 1.0 && r.max == 8.0 && r.Mean() == 4.0);
    CHECK(RollingText("hook_us", r) == "hook_us n=3 mean=4.00 min=1.00 max=8.00");
}

void Names()
{
    CHECK(FormatName(87) == "B8G8R8A8_UNORM");
    CHECK_EQ(FormatFromName("B8G8R8A8_UNORM"), 87u);
    CHECK_EQ(FormatFromName("fmt999"), 999u);
    CHECK(StageName(0) == "-" && StageName(3) == "S3" && StageName(6) == "UI" && StageName(255) == "??");
    for (uint8_t s : { uint8_t(0), uint8_t(1), uint8_t(5), kUiStage, kStageUnknown })
        CHECK_EQ(StageFromName(StageName(s)), s);
    RtShape sh;
    CHECK(ParseShape("1920x1080/R16G16B16A16_FLOAT", sh));
    CHECK(sh == RtShape({ 1920, 1080, 10 }));
    CHECK(!ParseShape("1920x1080", sh));
    CHECK_EQ(sizeof(RawHeader), 32u);
}
} // namespace

int main()
{
    ShadersCsvRoundTrip();
    FramesRoundTrip();
    FrameSelection();
    RunFolders();
    Budget();
    Rolling_();
    Names();
    return Finish("ui_diag_format");
}
