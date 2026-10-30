// Pure text side of the developer probes (ext/hudless/PlateProbeFormat.h): csv escaping, the outputs.csv row, the summary line, rule
// R0 one condition at a time, the draws.csv row, the frame sampling and rewrite schedules, the byte caps, the per-shader table and the
// run folder naming. Exact strings are pinned on purpose: the analysis reads these files.
#include "ext/hudless/PlateProbeFormat.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ext::hudless::plate;

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)
#define EXPECT_EQ(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        const std::string va_ = (a), vb_ = (b);                                                                        \
        if (va_ != vb_)                                                                                                \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s\n      got      [%s]\n      expected [%s]\n", __LINE__, #a, va_.c_str(), vb_.c_str());\
        }                                                                                                              \
    } while (0)

size_t Columns(const std::string& header) { return 1 + static_cast<size_t>(std::count(header.begin(), header.end(), ',')); }

void Escaping()
{
    EXPECT_EQ(CsvField("0xDEADBEEF"), "0xDEADBEEF");
    EXPECT_EQ(CsvField(""), "");
    EXPECT_EQ(CsvField("a,b"), "\"a,b\"");
    EXPECT_EQ(CsvField("say \"hi\""), "\"say \"\"hi\"\"\"");
    EXPECT_EQ(CsvField("two\nlines"), "\"two\nlines\"");
    EXPECT_EQ(CsvField("cr\rhere"), "\"cr\rhere\"");
    EXPECT_EQ(CsvField("|-separated"), "|-separated");
}

void OutputsCsv()
{
    EXPECT(Columns(kOutputsHeader) == 12, "header columns");
    OutputRecord r;
    r.qpc = 2500000;
    r.thread = 77;
    r.backBuffer = 2;
    r.window = 5;
    r.batch = 4;
    r.indexInBatch = 2;
    r.multiplier = 3;
    r.lagPosts = 1;
    r.lagTicks = 17000;
    r.sinceLastTicks = 8000;
    r.ticksSincePost = 6500;
    EXPECT_EQ(OutputsRow(r, 1000000, 1000.0), "2500000,1500.000,77,2,5,4,2,3,1,17.000,8.000,6500\n");
    EXPECT(Columns(OutputsRow(r, 1000000, 1000.0)) == 12, "row columns");
    EXPECT_EQ(kOutputsHeader, "qpc,t_ms,thread,back_buffer,window,batch,index,multiplier,lag_posts,lag_ms,since_prev_ms,ticks_since_post");

    OutputRecord none;
    none.qpc = 900;
    EXPECT_EQ(OutputsRow(none, 1000, 1000.0), "900,0.000,0,-1,0,0,0,0,-1,-1,0.000,-1\n"); // before the run's zero: 0, no batch: -1
    EXPECT(OutputsHeaderText().find(kOutputsHeader) != std::string::npos, "header text carries the header");
    EXPECT(OutputsHeaderText().find("format 3") != std::string::npos && kFormatVersion == 3, "the draws.csv column removal bumped the format");
}

void Association()
{
    // every logged row is exactly one outcome, and the counters add up
    AssocStats a;
    EXPECT(!a.Any(), "an empty set says nothing");
    a.NoteRow(RowOutcome::NotIndexed, BoxReason::Ok);
    a.NoteRow(RowOutcome::Associated, BoxReason::Ok);
    a.NoteRow(RowOutcome::Associated, BoxReason::Ok);
    a.NoteRow(RowOutcome::Associated, BoxReason::Untagged);
    a.NoteRow(RowOutcome::NoPacket, BoxReason::Strides); // the box reason only counts for an associated row
    a.NoteRow(RowOutcome::Stale, BoxReason::NoRun);
    a.NoteRow(RowOutcome::Changed, BoxReason::NonFinite);
    a.NoteRow(RowOutcome::Mismatch, BoxReason::Ok);
    a.NoteRow(RowOutcome::Mismatch, BoxReason::Ok);
    EXPECT(a.rows == 9 && a.notIndexed == 1 && a.associated == 3 && a.noPacket == 1, "rows %llu", (unsigned long long) a.rows);
    EXPECT(a.stale == 1 && a.changed == 1 && a.mismatch == 2 && a.Rejected() == 4, "rejected rows");
    EXPECT(a.boxes == 2 && a.noBox == 1, "boxes among associated rows only");
    EXPECT(a.noBoxUntagged == 1 && a.noBoxStrides == 0 && a.noBoxNoRun == 0 && a.noBoxNonFinite == 0 && a.NoBoxReasons() == a.noBox,
           "the reasons of the other rows were not counted");
    EXPECT(a.rows == a.notIndexed + a.associated + a.noPacket + a.Rejected(), "every row is one outcome");
    EXPECT(a.Any(), "rows make it count");

    // each no-box reason has its own counter, and together they are the no-box rows
    AssocStats r;
    for (int i = 0; i < 1; ++i)
        r.NoteRow(RowOutcome::Associated, BoxReason::NoRun);
    for (int i = 0; i < 2; ++i)
        r.NoteRow(RowOutcome::Associated, BoxReason::Untagged);
    for (int i = 0; i < 3; ++i)
        r.NoteRow(RowOutcome::Associated, BoxReason::Strides);
    for (int i = 0; i < 4; ++i)
        r.NoteRow(RowOutcome::Associated, BoxReason::NonFinite);
    for (int i = 0; i < 5; ++i)
        r.NoteRow(RowOutcome::Associated, BoxReason::Ok);
    EXPECT(r.noBoxNoRun == 1 && r.noBoxUntagged == 2 && r.noBoxStrides == 3 && r.noBoxNonFinite == 4 && r.boxes == 5, "reasons");
    EXPECT(r.noBox == 10 && r.NoBoxReasons() == r.noBox && r.associated == r.boxes + r.noBox, "no box = the sum of its reasons");

    AssocStats e;
    e.refusedBusy = 1;
    EXPECT(e.Any(), "a refusal on the enqueue side counts too");
    AssocStats f;
    f.unmapped = 1;
    EXPECT(f.Any(), "so does an unmapped record");
    AssocStats g;
    g.rowsCapped = 1;
    EXPECT(g.Any(), "so does a capped row");
    AssocStats h;
    h.batchCut = 1;
    EXPECT(h.Any(), "so does a cut batch");
    AssocStats i;
    i.headerCap = 1;
    EXPECT(i.Any(), "so does a batch refused for its header count");
    AssocStats j;
    j.batchReads = 1;
    EXPECT(j.Any(), "and a batch that was read");

    // the row cap, per run: the rows refused, the frames that refused some, the priority rows among them
    AssocStats c;
    c.NoteCappedFrame(60, 0);
    c.NoteCappedFrame(10, 4);
    EXPECT(c.rowsCapped == 70 && c.framesCapped == 2 && c.priorityCapped == 4, "capped frames");
}

void Summary_()
{
    Summary s;
    s.outputs = 12;
    s.windows = 5;
    s.windowsKnown = 2;
    s.windowsEqual = 2;
    s.windowsTransition = 3;
    s.countHistogram = { { 0, 1 }, { 2, 4 } };
    s.threads = { { 1234, 12 } };
    s.maxLagPosts = 1;
    s.maxLagTicks = 20500;
    s.lateOutputs = 6;
    s.firstLag = { { 0, 2 }, { 1, 3 } };
    s.interval.maxLagPosts = 0;
    s.interval.maxLagTicks = 4500;
    s.interval.lateOutputs = 0;
    s.interval.firstLag = { { 0, 1 } };
    s.posts = 6;
    s.optionsEvents = 6;
    s.orphanOptions = 1;
    s.optionsMatched = 5;
    s.optionsSameThread = 5;
    s.optionsDelayTicks = 200;
    s.optionsDelayMaxTicks = 90;
    s.postIntervals = 5;
    s.postIntervalTicks = 80000;
    SummaryExtras x;
    x.ringDrops = 3;
    x.mergedLate = 1;
    x.reordered = 2;
    x.ticksPerMs = 1000;
    const std::string head =
        "Plate probe: outputs 12, batches 5, batches with count == M 100.0% (2 of 2 judged, 3 settling, 0 without M), count histogram "
        "{0:1, 2:4}, threads {1234:12}, max lag (outputs seen after the next Post) 1 posts, 20.500 ms, 6 late outputs (this interval: 0 posts, "
        "4.500 ms, 0 late), order check first-output lag {0:2, 1:3} (this interval {0:1}), pushed out of order 2, late events 1, posts 6, "
        "options per Post 1.00, options orphaned 1, options on the Post's thread 100.0% (5 of 5), options delay after Post mean 0.040 ms, max "
        "0.090 ms (Post interval mean 16.000 ms), resets 0, probe drops 3";
    EXPECT_EQ(SummaryLine(s, x),
              head +
                  ", draw rows 0 (not indexed 0, with packet 0, no packet 0, rejected 0 {stale:0, changed:0, mismatch:0}), box from vertices 0, "
                  "without box 0 {untagged:0, strides:0, norun:0, nonfinite:0}, records made 0, refused {capacity:0, read:0, busy:0}, indices "
                  "outside packets 0, rows capped 0 (frames 0), priority rows lost 0, batch cut 0 of 0, header cap hit 0");

    // the packet association, the row cap and the batch reads are the tail of the line, every key always present
    x.assoc.rows = 20;
    x.assoc.notIndexed = 2;
    x.assoc.associated = 11;
    x.assoc.noPacket = 4;
    x.assoc.stale = 1;
    x.assoc.changed = 1;
    x.assoc.mismatch = 1;
    x.assoc.boxes = 9;
    x.assoc.noBox = 2;
    x.assoc.noBoxUntagged = 1;
    x.assoc.noBoxNoRun = 1;
    x.assoc.made = 30;
    x.assoc.refusedCapacity = 3;
    x.assoc.refusedRead = 4;
    x.assoc.refusedBusy = 5;
    x.assoc.unmapped = 6;
    x.assoc.rowsCapped = 70;
    x.assoc.framesCapped = 4;
    x.assoc.priorityCapped = 5;
    x.assoc.batchReads = 12;
    x.assoc.batchCut = 2;
    x.assoc.headerCap = 1;
    const std::string tail =
        ", draw rows 20 (not indexed 2, with packet 11, no packet 4, rejected 3 {stale:1, changed:1, mismatch:1}), box from vertices 9, without "
        "box 2 {untagged:1, strides:0, norun:1, nonfinite:0}, records made 30, refused {capacity:3, read:4, busy:5}, indices outside packets 6, "
        "rows capped 70 (frames 4), priority rows lost 5, batch cut 2 of 12, header cap hit 1";
    const std::string line = SummaryLine(s, x);
    EXPECT(line.size() > tail.size() && line.compare(line.size() - tail.size(), tail.size(), tail) == 0, "association counters close the line: %s",
           line.c_str());
    EXPECT_EQ(AssocText(x.assoc), tail);
    EXPECT(line.compare(0, head.size(), head) == 0, "the output probe's part is before the tail");

    s.windowsKnown = 0;
    s.windowsEqual = 0;
    s.windowsUnknown = 1;
    EXPECT(SummaryLine(s, x).find("batches with count == M n/a (0 judged, 3 settling, 1 without M)") != std::string::npos, "n/a form");

    s.windowsKnown = 3;
    s.windowsEqual = 1;
    EXPECT(SummaryLine(s, x).find("33.3% (1 of 3 judged") != std::string::npos, "a share below 100%%");

    // the multiplier phase: a share below 100%, a delay of about one Post interval (a frame late), and no Options event matched at all
    s.optionsSameThread = 1;
    s.optionsMatched = 3;
    s.optionsDelayTicks = 3 * 16000;
    s.optionsDelayMaxTicks = 17000;
    EXPECT(SummaryLine(s, x).find("options on the Post's thread 33.3% (1 of 3), options delay after Post mean 16.000 ms, max 17.000 ms (Post interval mean "
                                  "16.000 ms)") != std::string::npos,
           "phase: %s", SummaryLine(s, x).c_str());
    s.optionsMatched = 0;
    s.optionsSameThread = 0;
    s.optionsDelayTicks = 0;
    s.optionsDelayMaxTicks = 0;
    s.postIntervals = 0;
    s.postIntervalTicks = 0;
    s.orphanOptions = 6;
    EXPECT(SummaryLine(s, x).find("options orphaned 6, options on the Post's thread n/a (0 of 0), options delay after Post mean 0.000 ms, max 0.000 ms "
                                  "(Post interval mean 0.000 ms), resets") != std::string::npos,
           "no matched Options event: %s", SummaryLine(s, x).c_str());

    // a long thread list is cut, never unbounded
    s.threads.clear();
    for (uint32_t t = 1; t <= 12; ++t)
        s.threads[t] = 1;
    EXPECT(SummaryLine(s, x).find("{1:1, 2:1, 3:1, 4:1, 5:1, 6:1, 7:1, 8:1, ...}") != std::string::npos, "thread list is cut at eight");
}

DepthState Base()
{
    DepthState d;
    d.dsvBound = true;
    d.depthEnable = true;
    d.depthWrite = false;
    d.depthFunc = kCompareGreaterEqual;
    d.stencilEnable = false;
    return d;
}

void RuleR0()
{
    EXPECT(IsCandidate(Base(), RtClass::Backbuffer), "base case");
    EXPECT(IsCandidate(Base(), RtClass::BackbufferLike), "backbuffer-like");
    EXPECT(!IsCandidate(Base(), RtClass::Other), "other target");
    EXPECT(!IsCandidate(Base(), RtClass::None), "no target");
    {
        DepthState d = Base();
        d.dsvBound = false;
        EXPECT(!IsCandidate(d, RtClass::Backbuffer), "no depth view");
    }
    {
        DepthState d = Base();
        d.depthEnable = false;
        EXPECT(!IsCandidate(d, RtClass::Backbuffer), "depth test off");
    }
    {
        DepthState d = Base();
        d.depthWrite = true;
        EXPECT(!IsCandidate(d, RtClass::Backbuffer), "depth write on");
    }
    {
        DepthState d = Base();
        d.stencilEnable = true;
        EXPECT(!IsCandidate(d, RtClass::Backbuffer), "stencil on");
    }
    for (uint32_t func : { 1u, 2u, 3u, 4u, 5u, 6u, 8u })
    {
        DepthState d = Base();
        d.depthFunc = func;
        EXPECT(!IsCandidate(d, RtClass::Backbuffer), "function %u", func);
    }
    // no depth-stencil state object bound: the D3D11 default (test on, LESS, writes on) is never a candidate, with or without a view
    EXPECT(!IsCandidate(DefaultDepthState(true), RtClass::Backbuffer), "default state with a view");
    EXPECT(!IsCandidate(DefaultDepthState(false), RtClass::Backbuffer), "default state without a view");
    EXPECT(DefaultDepthState(true).depthEnable && DefaultDepthState(true).depthWrite && DefaultDepthState(true).depthFunc == kCompareLess &&
               !DefaultDepthState(true).stencilEnable,
           "the default is LESS with writes");
    EXPECT(kCompareGreaterEqual == 7 && kCompareLess == 2, "D3D11 comparison numbers");
}

DrawRow SampleRow()
{
    DrawRow r;
    r.frame = 700;
    r.draw = 42;
    r.vs = "0x11223344";
    r.ps = "0xE6A2D0CE";
    r.stage = 6;
    r.kind = 'I';
    r.indexCount = 36;
    r.instanceCount = 1;
    r.startIndex = 120;
    r.baseVertex = -4;
    r.topology = 4;
    r.stride = 32;
    r.rt = RtClass::Backbuffer;
    r.rtWidth = 3840;
    r.rtHeight = 2160;
    r.rtFormat = 28;
    r.vpWidth = 3840;
    r.vpHeight = 2160;
    r.depth = Base();
    r.blend = true;
    r.srcBlend = 5;
    r.dstBlend = 6;
    r.blendOp = 1;
    r.srcAlpha = 2;
    r.dstAlpha = 6;
    r.srvs = "7/512x512/28";
    return r;
}

void DrawsCsv()
{
    EXPECT(Columns(kDrawsHeader) == kDrawsColumns && kDrawsColumns == 31, "header columns %zu", Columns(kDrawsHeader));
    const DrawRow r = SampleRow();
    EXPECT_EQ(DrawsRow(r),
              "700,42,0x11223344,0xE6A2D0CE,6,I,36,1,120,-4,4,32,BB,3840,2160,28,3840,2160,1,1,0,7,0,1,5,6,1,2,6,7/512x512/28,1\n");
    EXPECT(Columns(DrawsRow(r)) == kDrawsColumns, "row columns");

    DrawRow other = r;
    other.rt = RtClass::Other;
    other.srvs = "a,b";
    const std::string text = DrawsRow(other);
    EXPECT(text.find(",OTHER,") != std::string::npos, "other target");
    EXPECT(text.find(",\"a,b\",0\n") != std::string::npos, "a field with a comma is quoted: %s", text.c_str());
    EXPECT(Columns(kDrawsHeader) == kDrawsColumns, "a quoted field does not change the header");
    EXPECT(DrawsHeaderText().find(kDrawsHeader) != std::string::npos, "header text carries the header");
    // r0 is computed from the recorded state, not stored
    DrawRow w = r;
    w.depth.depthWrite = true;
    EXPECT(DrawsRow(w).back() == '\n' && DrawsRow(w).substr(DrawsRow(w).size() - 3) == ",0\n", "r0 follows the depth write");
}

void Schedules()
{
    EXPECT(!LogThisFrame(0), "frame 0");
    EXPECT(LogThisFrame(1) && LogThisFrame(599) && LogThisFrame(600), "first 600");
    EXPECT(!LogThisFrame(601) && !LogThisFrame(699), "between");
    EXPECT(LogThisFrame(700) && LogThisFrame(1000) && LogThisFrame(123400), "every 100th");
    EXPECT(!LogThisFrame(701) && !LogThisFrame(999), "not on the other frames");
    EXPECT(RewriteShadersThisFrame(120) && RewriteShadersThisFrame(600) && RewriteShadersThisFrame(1800) && RewriteShadersThisFrame(3600),
           "rewrite frames");
    EXPECT(!RewriteShadersThisFrame(0) && !RewriteShadersThisFrame(121) && !RewriteShadersThisFrame(601) && !RewriteShadersThisFrame(1799) &&
               !RewriteShadersThisFrame(2000),
           "no rewrite");
    EXPECT(FrameClass(1) == 0 && FrameClass(600) == 0 && FrameClass(601) == 1 && FrameClass(100000) == 1, "frame classes");
}

void Caps()
{
    ByteBudget b { 10 };
    EXPECT(b.Take(6) && !b.truncated, "fits");
    EXPECT(!b.Take(5) && b.truncated, "over the cap: refused and flagged");
    EXPECT(!b.Take(1), "after a refusal nothing more is taken, even what would fit");
    ByteBudget exact { 10 };
    EXPECT(exact.Take(10) && !exact.truncated, "exactly the cap fits");
    EXPECT(!exact.Take(1), "one more does not");

    // the early frames cannot use up the room of the in-world samples
    DrawBudget d;
    EXPECT(d.Take(1, kEarlyDrawBytes) && !d.Take(2, 1), "early frames hit their own cap");
    EXPECT(d.early.truncated && !d.world.truncated, "only the early budget is spent");
    EXPECT(d.Take(601, 1000) && d.Take(700, 1000), "world frames still have room");
    DrawBudget edge;
    EXPECT(edge.Take(600, kEarlyDrawBytes) && !edge.Take(600, 1) && edge.Take(601, kWorldDrawBytes) && !edge.Take(601, 1), "frame 600 is early, 601 is world");
    // explicit per-run caps
    EXPECT(kEarlyDrawBytes == (6ull << 20) && kWorldDrawBytes == (40ull << 20) && kOutputsBytes == (48ull << 20), "cap values");
    EXPECT(kMaxDrawRowsPerFrame == 400 && kMaxShaderRows == 5000, "row caps");
    EXPECT(kMaxPriorityRowsPerFrame > kMaxDrawRowsPerFrame, "the bypass limit is the higher one");
}

void RowCaps()
{
    EXPECT(kMaxDrawRowsPerFrame == 400 && kMaxPriorityRowsPerFrame == 1200, "the two limits");
    // a normal row: the 400th is kept, the 401st is refused
    RowCap normal;
    for (size_t i = 0; i < kMaxDrawRowsPerFrame; ++i)
        EXPECT(normal.Admit(false), "normal row %zu", i + 1);
    EXPECT(normal.kept == 400 && normal.dropped == 0 && !normal.Capped(), "400 kept");
    EXPECT(!normal.Admit(false), "normal row 401 is refused");
    EXPECT(normal.Capped() && normal.dropped == 1 && normal.priorityDropped == 0 && normal.kept == 400, "counted, not kept");
    // a priority row goes on past 400 up to 1200 rows in the frame (all rows count, not only priority ones)
    for (size_t i = kMaxDrawRowsPerFrame; i < kMaxPriorityRowsPerFrame; ++i)
        EXPECT(normal.Admit(true), "priority row %zu", i + 1);
    EXPECT(normal.kept == 1200 && normal.dropped == 1 && normal.priorityDropped == 0, "1200 kept");
    EXPECT(!normal.Admit(true), "priority row 1201 is refused");
    EXPECT(normal.dropped == 2 && normal.priorityDropped == 1 && normal.kept == 1200, "a refused priority row is counted twice over: dropped and priority");
    EXPECT(!normal.Admit(false) && normal.dropped == 3 && normal.priorityDropped == 1, "normal rows stay refused");
    // priority rows alone: 1200 kept, the 1201st refused
    RowCap only;
    for (size_t i = 0; i < kMaxPriorityRowsPerFrame; ++i)
        EXPECT(only.Admit(true), "priority row %zu", i + 1);
    EXPECT(!only.Admit(true) && only.priorityDropped == 1, "1201");
    // normal rows that arrive after priority rows filled the frame past 400 are refused
    RowCap mixed;
    for (size_t i = 0; i < 450; ++i)
        EXPECT(mixed.Admit(true), "early priority %zu", i + 1);
    EXPECT(!mixed.Admit(false) && mixed.dropped == 1 && mixed.priorityDropped == 0, "a normal row after 450 rows");
    EXPECT(mixed.Admit(true), "a priority row still fits");
    // next frame starts clean
    normal.NextFrame();
    EXPECT(normal.kept == 0 && normal.dropped == 0 && normal.priorityDropped == 0 && !normal.Capped(), "next frame");
    EXPECT(normal.Admit(false), "and admits again");

    // the marker rows
    RowCap marked;
    EXPECT(CapMarker(700, marked).empty(), "no marker without a drop");
    for (size_t i = 0; i < 400; ++i)
        marked.Admit(false);
    for (int i = 0; i < 35; ++i)
        marked.Admit(false);
    EXPECT_EQ(CapMarker(700, marked), "# frame 700 capped at 400 rows, 35 draws dropped\n");
    for (size_t i = 400; i < 1200; ++i)
        marked.Admit(true);
    for (int i = 0; i < 3; ++i)
        marked.Admit(true);
    EXPECT_EQ(CapMarker(1000, marked), "# frame 1000 capped at 400 rows, 38 draws dropped\n"
                                       "# frame 1000 priority limit 1200 reached, 3 r0 draws dropped\n");
    EXPECT(CapMarker(1, marked).rfind("# frame 1 capped at 400 rows,", 0) == 0, "the marker starts with a comment sign");
    // draws.csv explains the marker
    EXPECT(DrawsHeaderText().find("# frame N capped at 400 rows, M draws dropped") != std::string::npos, "the header says what the marker is");
}

void Shaders()
{
    ShaderTable t;
    const size_t a = t.Find("0xAAAA0001", 6);
    const size_t b = t.Find("0xBBBB0002", 6);
    EXPECT(t.Find("0xAAAA0001", 6) == a && t.Size() == 2, "Find is stable");
    for (uint64_t f = 1; f <= 600; ++f)
        t.Add(a, f, f % 2 == 0, RtClass::Other); // 300 early r0 draws
    // world frames: 701 (3 draws, 2 r0), 702 (1 draw, 0 r0), 800 (2 draws, 2 r0)
    t.Add(a, 701, true, RtClass::Backbuffer);
    t.Add(a, 701, true, RtClass::BackbufferLike);
    t.Add(a, 701, false, RtClass::Backbuffer);
    t.Add(a, 702, false, RtClass::None);
    t.Add(a, 800, true, RtClass::Backbuffer);
    t.Add(a, 800, true, RtClass::Backbuffer);
    t.Add(b, 701, false, RtClass::Other);
    const auto rows = t.Rows();
    EXPECT(rows.size() == 2 && rows[0].ps == "0xAAAA0001", "sorted by world r0 first");
    const ShaderPlateRow& r = rows[0];
    EXPECT(r.earlyDraws == 600 && r.earlyR0 == 300, "early %llu/%llu", (unsigned long long) r.earlyDraws, (unsigned long long) r.earlyR0);
    EXPECT(r.worldDraws == 6 && r.worldR0 == 4, "world %llu/%llu", (unsigned long long) r.worldDraws, (unsigned long long) r.worldR0);
    EXPECT(r.worldFrames == 3 && r.worldR0Frames == 2, "frames %llu/%llu (the frame in progress is included)", (unsigned long long) r.worldFrames,
           (unsigned long long) r.worldR0Frames);
    EXPECT(r.worldMax == 3 && r.worldR0Max == 2, "max per frame %llu/%llu", (unsigned long long) r.worldMax, (unsigned long long) r.worldR0Max);
    EXPECT(r.firstFrame == 1, "first frame");
    EXPECT(r.rt[0] == 1 && r.rt[1] == 4 && r.rt[2] == 1 && r.rt[3] == 600, "targets %llu %llu %llu %llu", (unsigned long long) r.rt[0],
           (unsigned long long) r.rt[1], (unsigned long long) r.rt[2], (unsigned long long) r.rt[3]);
    EXPECT(rows[1].ps == "0xBBBB0002" && rows[1].worldDraws == 1 && rows[1].worldR0 == 0 && rows[1].firstFrame == 701, "second shader");
    // reading the table twice gives the same answer (the frame in progress is counted on a copy)
    EXPECT(t.Rows()[0].worldFrames == 3, "Rows() does not change the table");

    // vertex shader set: four, then "..."
    t.NoteVertexShader(a, "0x1");
    t.NoteVertexShader(a, "0x1");
    t.NoteVertexShader(a, "-");
    t.NoteVertexShader(a, "");
    t.NoteVertexShader(a, "0x2");
    t.NoteVertexShader(a, "0x3");
    t.NoteVertexShader(a, "0x4");
    EXPECT(t.Rows()[0].vsSet == "0x1|0x2|0x3|0x4", "vs set %s", t.Rows()[0].vsSet.c_str());
    t.NoteVertexShader(a, "0x5");
    EXPECT(t.Rows()[0].vsSet == "0x1|0x2|0x3|0x4|...", "vs set overflow %s", t.Rows()[0].vsSet.c_str());
    EXPECT(t.Rows()[1].vsSet == "-", "no vertex shader seen");

    const std::string csv = t.Csv();
    EXPECT(csv.find(kShadersHeader) != std::string::npos, "csv header");
    EXPECT(Columns(kShadersHeader) == 16, "header columns %zu", Columns(kShadersHeader));
    EXPECT(Columns(ShadersRow(r)) == 16, "row columns");
    EXPECT(csv.find("0xAAAA0001,0x1|0x2|0x3|0x4|...,6,1,600,300,6,4,3,2,3,2,1,4,1,600\n") != std::string::npos, "row text: %s", csv.c_str());
    EXPECT(csv.find(kTruncatedLine) == std::string::npos, "small table is not truncated");

    // the table is bounded in the file
    ShaderTable many;
    for (size_t i = 0; i < kMaxShaderRows + 10; ++i)
    {
        char name[32];
        snprintf(name, sizeof name, "0x%08zX", i);
        many.Add(many.Find(name, 6), 700, false, RtClass::Other);
    }
    const std::string big = many.Csv();
    size_t lines = 0;
    for (char c : big)
        lines += c == '\n';
    EXPECT(many.Rows().size() == kMaxShaderRows, "rows are cut at the cap: %zu", many.Rows().size());
    EXPECT(big.find(kTruncatedLine) != std::string::npos, "marker");
    EXPECT(lines == 5 + 1 + kMaxShaderRows + 1, "lines %zu", lines); // comment lines of the header text + column line + rows + marker
}

void RunFolders()
{
    std::tm t {};
    t.tm_year = 126;
    t.tm_mon = 9;
    t.tm_mday = 28;
    t.tm_hour = 7;
    t.tm_min = 5;
    t.tm_sec = 9;
    EXPECT_EQ(FormatRunId(t), "20261028-070509");
    EXPECT(IsRunId("20261028-070509") && !IsRunId("2026102-0070509") && !IsRunId("20261028-07050x") && !IsRunId("notes") && !IsRunId(""), "run ids");
    std::vector<std::string> names { "20261020-100000", "20261021-100000", "20261022-100000", "20261023-100000",
                                     "20261024-100000", "20261025-100000", "20261026-100000", "keep-me", "readme.txt" };
    const auto prune = RunsToPrune(names);
    EXPECT(prune.size() == 2 && prune[0] == "20261020-100000" && prune[1] == "20261021-100000", "the two oldest of seven runs");
    names.resize(5);
    EXPECT(RunsToPrune(names).empty(), "five runs are kept");
    names.push_back("20261030-100000");
    const auto one = RunsToPrune(names);
    EXPECT(one.size() == 1 && one[0] == "20261020-100000", "the sixth run prunes one");
    EXPECT(RunsToPrune({ "keep-me", "readme.txt" }).empty(), "folders that are not runs are never listed");
    EXPECT(RunsToPrune({ "20261020-100000", "20261020-100000" }).empty(), "duplicates count once");
    EXPECT(kKeepRuns == 5 && RunsToPrune({ "20261020-100000", "20261021-100000" }, 1).size() == 1, "keep parameter");
}

// ---- literal pins found missing by the mutation checks ---------------------------------------------------------------
static_assert(kKeepRuns == 5 && kMaxShaderRows == 5000 && kEarlyFrames == 600 && kFormatVersion == 3, "the numbers are written out on purpose");
static_assert(kOutputsBytes == (48ull << 20) && kEarlyDrawBytes == (6ull << 20) && kWorldDrawBytes == (40ull << 20), "byte caps");
static_assert(kMaxDrawRowsPerFrame == 400 && kMaxPriorityRowsPerFrame == 1200, "row caps");

std::vector<std::string> SplitCsv(std::string line)
{
    if (!line.empty() && line.back() == '\n')
        line.pop_back();
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;)
    {
        const size_t c = line.find(',', pos);
        if (c == std::string::npos)
        {
            out.push_back(line.substr(pos));
            return out;
        }
        out.push_back(line.substr(pos, c - pos));
        pos = c + 1;
    }
}

void TextPins()
{
    // the text that is written to the analysis files
    EXPECT_EQ(kTruncatedLine, "# TRUNCATED: byte cap of this file reached, later rows were dropped\n");
    EXPECT_EQ(RtName(RtClass::None), "NONE");
    EXPECT_EQ(RtName(RtClass::Backbuffer), "BB");
    EXPECT_EQ(RtName(RtClass::BackbufferLike), "BBLIKE");
    EXPECT_EQ(RtName(RtClass::Other), "OTHER");
    EXPECT_EQ(kDrawsHeader,
              "frame,draw,vs,ps,stage,kind,index_count,instance_count,start_index,base_vertex,topology,stride,rt,rt_w,rt_h,"
              "rt_fmt,vp_w,vp_h,dsv,depth,depth_write,depth_func,stencil,blend,src,dst,op,src_a,dst_a,srvs,r0");
    EXPECT_EQ(kShadersHeader,
              "ps,vs_set,stage,first_frame,early_draws,early_r0,world_draws,world_r0,world_frames,world_r0_frames,world_max_per_frame,"
              "world_r0_max_per_frame,rt_none,rt_bb,rt_bblike,rt_other");

    // the draws.csv row, field by field against the header's own column names
    const std::vector<std::string> names = SplitCsv(kDrawsHeader);
    const std::vector<std::string> fields = SplitCsv(DrawsRow(SampleRow()));
    EXPECT(names.size() == kDrawsColumns && fields.size() == kDrawsColumns, "columns %zu/%zu", names.size(), fields.size());
    auto field = [&](const char* name) -> std::string
    {
        for (size_t i = 0; i < names.size() && i < fields.size(); ++i)
            if (names[i] == name)
                return fields[i];
        return "<no such column>";
    };
    EXPECT_EQ(field("frame"), "700");
    EXPECT_EQ(field("draw"), "42");
    EXPECT_EQ(field("vs"), "0x11223344");
    EXPECT_EQ(field("ps"), "0xE6A2D0CE");
    EXPECT_EQ(field("stage"), "6");
    EXPECT_EQ(field("r0"), "1");

    // the outputs.csv row of an output exactly on its batch's Post: zero lag is 0.000, not the "no batch" -1
    OutputRecord zero;
    zero.qpc = 5000;
    zero.lagPosts = 0;
    zero.lagTicks = 0;
    zero.sinceLastTicks = 0;
    zero.ticksSincePost = 0;
    EXPECT_EQ(OutputsRow(zero, 1000, 1000.0), "5000,4.000,0,-1,0,0,0,0,0,0.000,0.000,0\n");

    // both shader names are escaped like the other text fields
    DrawRow q = SampleRow();
    q.vs = "a,b";
    q.ps = "c\"d";
    EXPECT(DrawsRow(q).find(",\"a,b\",\"c\"\"d\",6,I,") != std::string::npos, "escaped shader names: %s", DrawsRow(q).c_str());
    EXPECT(SplitCsv(DrawsRow(SampleRow())).size() == kDrawsColumns, "a plain row still has the header's column count");
}

void HistogramLimit()
{
    Summary s;
    SummaryExtras x;
    x.ticksPerMs = 1000;
    auto histogram = [&](uint32_t n)
    {
        s.countHistogram.clear();
        for (uint32_t i = 1; i <= n; ++i)
            s.countHistogram[i] = 1;
        const std::string line = SummaryLine(s, x);
        const std::string key = "count histogram ";
        const size_t begin = line.find(key);
        const size_t end = line.find(", threads ");
        return begin == std::string::npos || end == std::string::npos ? std::string("<not found>") : line.substr(begin + key.size(), end - begin - key.size());
    };
    EXPECT_EQ(histogram(10), "{1:1, 2:1, 3:1, 4:1, 5:1, 6:1, 7:1, 8:1, 9:1, 10:1}");
    EXPECT_EQ(histogram(12), "{1:1, 2:1, 3:1, 4:1, 5:1, 6:1, 7:1, 8:1, 9:1, 10:1, 11:1, 12:1}");
    EXPECT_EQ(histogram(13), "{1:1, 2:1, 3:1, 4:1, 5:1, 6:1, 7:1, 8:1, 9:1, 10:1, 11:1, 12:1, ...}");
}

void DefaultsAndMarkers()
{
    EXPECT(!DefaultDepthState(false).dsvBound && DefaultDepthState(true).dsvBound, "the default state keeps the view flag");
    EXPECT(DefaultDepthState(false).depthFunc == 2 && DefaultDepthState(false).depthEnable && DefaultDepthState(false).depthWrite, "and the D3D11 default");

    // exactly one refused row still gets its marker
    RowCap one;
    for (size_t i = 0; i < 400; ++i)
        EXPECT(one.Admit(false), "row %zu", i + 1);
    EXPECT(!one.Admit(false) && one.dropped == 1, "the 401st is refused");
    EXPECT_EQ(CapMarker(7, one), "# frame 7 capped at 400 rows, 1 draws dropped\n");
}

void ShaderOrderAndLimit()
{
    // equal world_r0: more world draws first; fully tied rows: by name, ascending (inserted in the opposite order)
    ShaderTable t;
    auto draws = [&](const char* name, int n)
    {
        const size_t i = t.Find(name, 6);
        for (int k = 0; k < n; ++k)
            t.Add(i, 700, false, RtClass::Other);
    };
    draws("0xSSSS", 2);
    draws("0xRRRR", 2);
    draws("0xPPPP", 5);
    draws("0xQQQQ", 9);
    const auto rows = t.Rows();
    EXPECT(rows.size() == 4 && rows[0].ps == "0xQQQQ" && rows[1].ps == "0xPPPP" && rows[2].ps == "0xRRRR" && rows[3].ps == "0xSSSS",
           "order: %s %s %s %s", rows.size() > 0 ? rows[0].ps.c_str() : "-", rows.size() > 1 ? rows[1].ps.c_str() : "-",
           rows.size() > 2 ? rows[2].ps.c_str() : "-", rows.size() > 3 ? rows[3].ps.c_str() : "-");

    // a table of exactly 5000 shaders is complete; the 5001st is the first to be cut and to put the marker in the file
    auto build = [](size_t count)
    {
        ShaderTable many;
        for (size_t i = 0; i < count; ++i)
        {
            char name[32];
            snprintf(name, sizeof name, "0x%08zX", i);
            many.Add(many.Find(name, 6), 700, false, RtClass::Other);
        }
        return many;
    };
    auto lines = [](const std::string& text)
    {
        size_t n = 0;
        for (char c : text)
            n += c == '\n';
        return n;
    };
    const ShaderTable exact = build(5000);
    const std::string full = exact.Csv();
    EXPECT(exact.Rows().size() == 5000 && full.find(kTruncatedLine) == std::string::npos, "5000 shaders: no marker");
    EXPECT(lines(full) == 5 + 1 + 5000, "5000 rows and no marker line: %zu", lines(full));
    const ShaderTable over = build(5001);
    const std::string cut = over.Csv();
    EXPECT(over.Rows().size() == 5000 && cut.find(kTruncatedLine) != std::string::npos, "5001 shaders: cut and marked");
    EXPECT(lines(cut) == 5 + 1 + 5000 + 1, "5000 rows and the marker line: %zu", lines(cut));
}

void PruneOrder()
{
    // the oldest are pruned whatever order the directory listing came in
    const auto one = RunsToPrune({ "20261003-000001", "20261001-000001", "20261005-000001", "20261002-000001", "20261006-000001", "20261004-000001" });
    EXPECT(one.size() == 1 && one[0] == "20261001-000001", "unsorted listing: %zu pruned, first %s", one.size(), one.empty() ? "-" : one[0].c_str());
    const auto two = RunsToPrune({ "20261004-000001", "20261007-000001", "20261001-000001", "20261006-000001", "20261003-000001", "20261005-000001",
                                   "20261002-000001" });
    EXPECT(two.size() == 2 && two[0] == "20261001-000001" && two[1] == "20261002-000001", "two oldest of an unsorted listing");
}
} // namespace

int main()
{
    TextPins();
    HistogramLimit();
    DefaultsAndMarkers();
    ShaderOrderAndLimit();
    PruneOrder();
    Escaping();
    OutputsCsv();
    Association();
    Summary_();
    RuleR0();
    DrawsCsv();
    Schedules();
    Caps();
    RowCaps();
    Shaders();
    RunFolders();
    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: probe formats (csv escaping, outputs row, summary line, rule R0, draws row, schedules, caps, shader table, run folders)");
    return 0;
}
