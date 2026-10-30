// The several-window counters of the preview diagnostics (CharaWindowStats.h): pure counters, so every rule is checked here without a
// device. The test drives the same calls the dispatcher makes (a finisher draw, a pass start, a UI draw that samples an image, a
// replay refusal, a slot failure) and checks the numbers the burst log prints, including the bounds of an entry that is not an output
// entry, the histogram cap, the collision detector's frame boundaries and the burst reset.
#include "check.h"

#include "ext/ffxiv/chara/CharaWindowStats.h"

#include <set>
#include <vector>

using namespace ext::ffxiv::chara;

namespace
{
constexpr uintptr_t kA = 0xA000;
constexpr uintptr_t kB = 0xB000;
constexpr uintptr_t kC = 0xC000;

void TestEntryBuckets()
{
    // entries 0..7 are their own buckets; -1 and 8 or more share the "not an output entry" bucket
    CHECK(WindowStats::Bucket(0) == 0);
    CHECK(WindowStats::Bucket(7) == 7);
    CHECK(WindowStats::Bucket(-1) == kWindowOtherEntry);
    CHECK(WindowStats::Bucket(8) == kWindowOtherEntry);
    CHECK(WindowStats::Bucket(99) == kWindowOtherEntry);

    WindowStats s;
    s.NoteFinisher(0, true, kA, 10);
    s.NoteFinisher(7, true, kB, 11);
    s.NoteFinisher(-1, false, 0, 12);
    s.NoteFinisher(8, true, kC, 13);
    const auto& e = s.Entries();
    CHECK(e[0].finishers == 1 && e[7].finishers == 1);
    CHECK(e[kWindowOtherEntry].finishers == 1 && e[kWindowOtherEntry].rejected == 1);
    CHECK(s.DistinctEntries() == 2); // 0 and 7; the bucket for non-output entries is not an output entry

    // an evaluation, a processed UI draw and a failure land in the entry of their window
    s.NoteEvaluated(7);
    s.NoteProcessedDraw(7);
    s.NoteProcessedDraw(7);
    s.NoteSrFailure(3, 7);
    CHECK(e[7].evaluated == 1 && e[7].processedDraws == 2 && e[7].failures == 1);
    CHECK(s.SlotFailures(3) == 1 && s.SlotFailures(2) == 0 && s.SlotFailures(-1) == 0 && s.SlotFailures(99) == 0);

    // the finisher verdicts are counted per entry, and a value outside the enum is dropped instead of writing out of bounds
    s.NoteFinishVerdict(7, dlss::FinishVerdict::WaitingForMotion);
    s.NoteFinishVerdict(7, dlss::FinishVerdict::Proceed);
    s.NoteFinishVerdict(7, static_cast<dlss::FinishVerdict>(200));
    CHECK(e[7].verdicts[static_cast<size_t>(dlss::FinishVerdict::WaitingForMotion)] == 1);
    CHECK(e[7].verdicts[static_cast<size_t>(dlss::FinishVerdict::Proceed)] == 1);
}

void TestCollision()
{
    WindowStats s;
    // one image, two accepted finishers in different game frames: not a collision
    CHECK(!s.NoteFinisher(0, true, kA, 10));
    CHECK(!s.NoteFinisher(0, true, kA, 11));
    CHECK(s.Collisions() == 0);
    // two accepted finishers of the same image in one frame: a collision, counted once
    CHECK(!s.NoteFinisher(0, true, kA, 12));
    CHECK(s.NoteFinisher(0, true, kA, 12));
    CHECK(s.LastFinisherCollided());
    CHECK(s.Collisions() == 1);
    // the same frame number with another image is not one
    CHECK(!s.NoteFinisher(1, true, kB, 12));
    CHECK(!s.LastFinisherCollided());
    // a rejected finisher is not a render and does not move the detector
    CHECK(!s.NoteFinisher(0, false, kA, 13));
    CHECK(!s.LastFinisherCollided());
    // no frame counter (0) never collides: the game may have no frame callback at all
    CHECK(!s.NoteFinisher(0, true, kA, 0));
    CHECK(!s.NoteFinisher(0, true, kA, 0));
    CHECK(s.Collisions() == 1);
    // an unknown image pointer is never a collision
    CHECK(!s.NoteFinisher(0, true, 0, 20));
    CHECK(!s.NoteFinisher(0, true, 0, 20));
    CHECK(s.Collisions() == 1);
}

void TestSampledImages()
{
    WindowStats s;
    s.NoteSample(kA, 0, 1000);
    CHECK(s.PeakSampled() == 1);
    CHECK(s.DistinctEntries() == 1);
    s.NoteSample(kB, 1, 1000);
    CHECK(s.PeakSampled() == 2);
    // the same image sampled again in the window is one image, not two
    s.NoteSample(kA, 0, 1100);
    CHECK(s.PeakSampled() == 2);
    // a third image 300 ms after the first two: only it is inside the 250 ms window
    s.NoteSample(kC, 2, 1301);
    CHECK(s.PeakSampled() == 2);
    CHECK(s.DistinctEntries() == 3);
    // a null pointer is ignored
    s.NoteSample(0, 3, 1301);
    CHECK(s.DistinctEntries() == 3);
    // more images than the table holds: the oldest leaves, the peak stays bounded by the table
    WindowStats many;
    for (uintptr_t i = 1; i <= kSampledImages + 8; ++i)
        many.NoteSample(0x1000 + i, static_cast<int>(i % 8), 1000 + i);
    CHECK(many.PeakSampled() <= kSampledImages);
    CHECK(many.PeakSampled() > 1);
}

void TestRendersPerFrame()
{
    WindowStats s;
    s.NoteRenderStart(1);
    s.NoteRenderStart(1);
    s.NoteRenderStart(2);
    s.NoteRenderStart(3);
    s.NoteRenderStart(3);
    s.NoteRenderStart(3);
    s.NoteRenderStart(3);
    s.NoteRenderStart(3);
    s.NoteRenderStart(7);
    s.FlushFrame();
    const auto& h = s.RendersPerFrame();
    CHECK(h[0] == 2); // frames 2 and 7 had one render
    CHECK(h[1] == 1); // frame 1 had two
    CHECK(h[2] == 0);
    CHECK(h[3] == 1); // frame 3 had five: the "4 or more" bucket
    // a second flush of the same frame counts nothing twice
    s.FlushFrame();
    CHECK(h[3] == 1);
    // a frame that is counted again after the flush is a new count of that frame (the flush ended the earlier one)
    s.NoteRenderStart(7);
    s.FlushFrame();
    CHECK(h[0] == 3);
}

void TestReplaySkips()
{
    WindowStats s;
    s.NoteReplaySkip(motion::Skip::OpenQuery);
    s.NoteReplaySkip(motion::Skip::UnknownShader);
    s.NoteReplaySkip(motion::Skip::UnknownShader);
    CHECK(s.ReplaySkips(motion::Skip::OpenQuery) == 1);
    CHECK(s.ReplaySkips(motion::Skip::UnknownShader) == 2);
    CHECK(s.ReplaySkips(motion::Skip::NoTarget) == 0);
    CHECK(s.ReplaySkips(static_cast<motion::Skip>(200)) == 0); // out of range: no write, no crash
    s.NoteReplaySkip(static_cast<motion::Skip>(200));

    // the untagged shapes: one record per distinct shape, at most kUnknownShapes per burst
    UnknownShape a;
    a.crc = 0x1234;
    a.vsCb0Bytes = 944;
    a.psCb2Bytes = 1024;
    a.rtvEntry = { 8, 9, 10, 11, 12 };
    UnknownShape b = a;
    b.crc = 0x5678;
    CHECK(s.NoteUnknownShape(a));
    CHECK(!s.NoteUnknownShape(a)); // the same shape is not recorded twice
    CHECK(s.NoteUnknownShape(b));
    CHECK(s.UnknownShapeCount() == 2);
    for (uint32_t i = 0; i < kUnknownShapes + 4; ++i)
    {
        UnknownShape more = a;
        more.crc = 0x9000 + i;
        s.NoteUnknownShape(more);
    }
    CHECK(s.UnknownShapeCount() == kUnknownShapes);
    CHECK(s.UnknownShapeAt(0).crc == 0x1234); // the first shapes stay readable
    CHECK(s.UnknownShapeAt(kUnknownShapes + 10).crc == 0x1234); // an index past the end reads the first record, not past the array
    // the sizes and entries are part of the shape: the same shader with another camera block is another record
    WindowStats t;
    CHECK(t.NoteUnknownShape(a));
    UnknownShape wide = a;
    wide.psCb2Bytes = 944;
    CHECK(t.NoteUnknownShape(wide));
}

void TestReset()
{
    WindowStats s;
    s.NoteFinisher(0, true, kA, 10);
    s.NoteFinisher(0, true, kA, 10);
    s.NoteEvaluated(0);
    s.NoteSample(kA, 0, 1000);
    s.NoteRenderStart(1);
    s.NoteReplaySkip(motion::Skip::OpenQuery);
    UnknownShape shape;
    shape.crc = 1;
    s.NoteUnknownShape(shape);
    s.NoteSrFailure(0, 0);
    s.FlushFrame();
    s.Reset();
    CHECK(s.Collisions() == 0);
    CHECK(!s.LastFinisherCollided());
    CHECK(s.DistinctEntries() == 0 && s.PeakSampled() == 0);
    CHECK(s.RendersPerFrame()[0] == 0);
    CHECK(s.ReplaySkips(motion::Skip::OpenQuery) == 0);
    CHECK(s.UnknownShapeCount() == 0);
    CHECK(s.SlotFailures(0) == 0);
    CHECK(s.Entries()[0].finishers == 0 && s.Entries()[0].evaluated == 0 && s.Entries()[0].failures == 0);
    // after the reset the collision detector starts from nothing: the first finisher of the next burst never collides
    CHECK(!s.NoteFinisher(0, true, kA, 10));
    CHECK(s.Collisions() == 0);
}
void TestNrBurstText()
{
    NrBurstCounts c;
    CHECK(NrBurstText(c) == "0 runs, skipped: full table 0, settling 0, no command list 0; streams peak 0 of 16, now 0; per stream: none");
    c.runs = 3;
    c.skipTableFull = 1;
    c.skipSettling = 2;
    c.skipNoList = 3;
    c.streamsPeak = 2;
    c.streamsNow = 2;
    c.entries[0] = 1;
    c.streamRuns[0] = 3;
    c.entries[1] = -1;
    c.entries[7] = 4;
    c.streamRuns[7] = 0;
    c.entries[3] = 0; // output entry 0 is a real stream, not an absent one
    std::string text = NrBurstText(c);
    CHECK(text == "3 runs, skipped: full table 1, settling 2, no command list 3; streams peak 2 of 16, now 2; per stream: stream 0 (output "
                   "entry 1) 3 runs, stream 3 (output entry 0) 0 runs, stream 7 (output entry 4) 0 runs");
    CHECK(text.find("stream 1") == std::string::npos); // a stream that does not exist is not listed
}

// The NR part of the burst line is only written when it says something: with preview NR off no stream exists and no call was made, and the line
// would repeat "0 runs" at every burst end.
void TestNrBurstWorthLogging()
{
    NrBurstCounts c;
    CHECK(!NrBurstWorthLogging(c)); // preview NR off: nothing to say
    c.runs = 1;
    CHECK(NrBurstWorthLogging(c));
    c.runs = 0;
    c.skipNoList = 1;
    CHECK(NrBurstWorthLogging(c));
    c.skipNoList = 0;
    c.streamsNow = 1; // a stream exists: the line says so even with no run in this burst
    CHECK(NrBurstWorthLogging(c));
    c.streamsNow = 0;
    c.entries[5] = 3; // a stream existed in this burst (a closed window's model)
    CHECK(NrBurstWorthLogging(c));
    c.entries[5] = -1;
    c.streamRuns[5] = 2;
    CHECK(NrBurstWorthLogging(c));
    c.streamRuns[5] = 0;
    c.streamsPeak = 4; // the peak is a session total: it alone does not make the line worth writing
    CHECK(!NrBurstWorthLogging(c));
}

// The identity part of the burst line: how the passes were named, and what the finishers taught.
void TestIdentityText()
{
    IdentityCounts c;
    CHECK(IdentityBurstText(c) == "0 passes named for a processed window, 0 named for one outside the set, 0 not learned yet, 0 ambiguous, "
                                 "0 with no camera block bound; 0 camera blocks learned, 0 ambiguity ended, 0 known now, 0 mis-named");
    c.named = 3;
    c.outside = 1;
    c.unknown = 2;
    c.ambiguous = 0;
    c.invalid = 4;
    c.learned = 2;
    c.cleared = 1;
    c.known = 3;
    c.mismatch = 1;
    CHECK(IdentityBurstText(c) == "3 passes named for a processed window, 1 named for one outside the set, 2 not learned yet, 0 ambiguous, "
                                 "4 with no camera block bound; 2 camera blocks learned, 1 ambiguity ended, 3 known now, 1 mis-named");
}

// The counters behind that line: one per WindowSet::Name value, and the things a finisher teaches (a binding, the end of an ambiguity, a pass the
// binding named for another image).
void TestIdentityCounters()
{
    WindowStats s;
    IdentityCounts c = s.Identity();
    CHECK(c.named == 0 && c.outside == 0 && c.unknown == 0 && c.ambiguous == 0 && c.invalid == 0 && c.learned == 0 && c.cleared == 0 &&
          c.mismatch == 0);
    s.NoteIdentity(WindowSet::Name::Named);
    s.NoteIdentity(WindowSet::Name::Named);
    s.NoteIdentity(WindowSet::Name::Outside);
    s.NoteIdentity(WindowSet::Name::Unknown);
    s.NoteIdentity(WindowSet::Name::Ambiguous);
    s.NoteIdentity(WindowSet::Name::Invalid);
    s.NoteIdentityLearned();
    s.NoteIdentityCleared();
    s.NoteIdentityCleared();
    s.NoteIdentityMismatch();
    c = s.Identity();
    CHECK(c.named == 2 && c.outside == 1 && c.unknown == 1 && c.ambiguous == 1 && c.invalid == 1 && c.learned == 1 && c.cleared == 2 &&
          c.mismatch == 1);
    // the counters belong to one burst
    s.Reset();
    c = s.Identity();
    CHECK(c.named == 0 && c.outside == 0 && c.unknown == 0 && c.ambiguous == 0 && c.invalid == 0 && c.learned == 0 && c.cleared == 0 &&
          c.mismatch == 0);
}

} // namespace

int main()
{
    TestEntryBuckets();
    TestCollision();
    TestSampledImages();
    TestRendersPerFrame();
    TestReplaySkips();
    TestNrBurstText();
    TestNrBurstWorthLogging();
    TestIdentityText();
    TestIdentityCounters();
    TestReset();
    return TEST_RESULT("chara_window_stats");
}
