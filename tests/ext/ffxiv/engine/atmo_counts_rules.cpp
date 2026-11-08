// Atmosphere: the timed counts line's rules on the host (INT2, AtmoCounts.h). No device.
//
// What a wrong rule would break, and what is checked here:
//   the cadence: the first look only starts the window (no line at the start of a session), nothing is due before ten seconds, a line is due at ten seconds and
//     after, a clock that went backwards starts the window again instead of writing a line, and a taken line starts the next window at the time it was taken;
//   the tallies: runs and reaches are counted per entry (own seam / chained hook / owner hook) and never mixed, declines per reason, skips per reason, and a
//     NaN-like out-of-range reason is dropped instead of corrupting a neighbour;
//   the window and the total: a line shows the window it closes and a total that includes it, the next window starts empty, the total keeps rising, and a
//     window in which the stage was never reached still produces a line (the heartbeat's case) that says so;
//   the line's text: it names the three entries, the declines and skips that happened (and only those), the rendered frames of the window and the running
//     total, and the settings; every decline reason has its own key (a list of reasons is unreadable when two share a word).
#include "check.h"

#include "ext/ffxiv/engine/AtmoCounts.h"

#include <cstdio>
#include <set>
#include <string>

using namespace ext::ffxiv::engine::atmo;

namespace
{
bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
} // namespace

int main()
{
    // ---- the constants ---------------------------------------------------------------------------------------------------------------
    CHECK_EQ(kCountsLogMs, 10000u);                                              // about every ten seconds, as the session check asks
    CHECK_EQ(kEntryCount, 3u);
    CHECK_EQ(kDeclineReasonCount, uint32_t(Decline::Exception) + 1u);            // every decline has a slot
    CHECK_EQ(kSkipReasonCount, 3u);

    // ---- every reason has its own key -------------------------------------------------------------------------------------------------
    {
        std::set<std::string> keys;
        for (uint32_t i = 0; i < kDeclineReasonCount; ++i)
        {
            const std::string key = DeclineKey(Decline(i));
            CHECK(!key.empty() && key != "unknown");
            keys.insert(key);
        }
        CHECK_EQ(keys.size(), size_t(kDeclineReasonCount));
        std::set<std::string> skipKeys;
        for (uint32_t i = 1; i <= kSkipReasonCount; ++i)
            skipKeys.insert(SkipKey(SkipReason(i)));
        CHECK_EQ(skipKeys.size(), size_t(kSkipReasonCount));
        std::set<std::string> entries;
        for (uint32_t i = 0; i < kEntryCount; ++i)
            entries.insert(EntryName(Entry(i)));
        CHECK_EQ(entries.size(), size_t(kEntryCount));
    }

    // ---- the cadence ------------------------------------------------------------------------------------------------------------------
    {
        TimedCounts counts;
        CHECK(!counts.Due(5000, 100));                  // the first look starts the window at 5 s, 100 frames
        CHECK(!counts.Due(5000, 100));
        CHECK(!counts.Due(14999, 700));                 // 9.999 s: not yet
        CHECK(counts.Due(15000, 800));                  // 10 s: due
        CHECK(counts.Due(40000, 2000));                 // and later (a window that was missed is still one line)
        const std::string line = counts.TakeLine(40000, 2000, Params {});
        CHECK(Contains(line, "in the last 35.0 s (1900 rendered frames)"));
        // the next window starts where the line was taken
        CHECK(!counts.Due(49999, 2500));
        CHECK(counts.Due(50000, 2600));
        // a clock that went backwards starts the window again and writes nothing
        CHECK(!counts.Due(1000, 3000));
        CHECK(!counts.Due(10999, 3100));
        CHECK(counts.Due(11000, 3200));
        // the very first look at time 0 is a start, not a line
        TimedCounts zero;
        CHECK(!zero.Due(0, 0));
        CHECK(!zero.Due(9999, 10));
        CHECK(zero.Due(10000, 20));
    }

    // ---- the tallies ------------------------------------------------------------------------------------------------------------------
    {
        TimedCounts counts;
        counts.NoteReached(Entry::Seam);
        counts.NoteReached(Entry::Chain);
        counts.NoteReached(Entry::Chain);
        counts.NoteReached(Entry::Owner);
        counts.NoteReached(Entry::Owner);
        counts.NoteReached(Entry::Owner);
        counts.NoteRan(Entry::Chain);
        counts.NoteRan(Entry::Chain);
        counts.NoteRan(Entry::Owner);
        counts.NoteDeclined(Decline::SceneHdr);
        counts.NoteDeclined(Decline::SceneHdr);
        counts.NoteDeclined(Decline::PassFailed);
        counts.NoteDeclined(Decline(200)); // out of range: dropped, no neighbour is touched
        counts.NoteSkipped(SkipReason::NoCamera);
        counts.NoteSkipped(SkipReason::NoFog);
        counts.NoteSkipped(SkipReason::NoFog);
        counts.NoteSkipped(SkipReason::None);       // not a skip
        counts.NoteSkipped(SkipReason(77));         // out of range
        const Tally& window = counts.Window();
        CHECK_EQ(window.reached[0], 1u);
        CHECK_EQ(window.reached[1], 2u);
        CHECK_EQ(window.reached[2], 3u);
        CHECK_EQ(window.ran[0], 0u);
        CHECK_EQ(window.ran[1], 2u);
        CHECK_EQ(window.ran[2], 1u);
        CHECK_EQ(window.declined[uint32_t(Decline::SceneHdr)], 2u);
        CHECK_EQ(window.declined[uint32_t(Decline::PassFailed)], 1u);
        CHECK_EQ(window.Declined(), 3u);
        CHECK_EQ(window.skipped[1], 1u);  // camera
        CHECK_EQ(window.skipped[2], 2u);  // fog
        CHECK_EQ(window.skipped[0], 0u);  // depth
        CHECK_EQ(window.Skipped(), 3u);
        CHECK_EQ(window.Reached(), 6u);
        CHECK_EQ(window.Ran(), 3u);
    }

    // ---- the window, the total and the line ---------------------------------------------------------------------------------------------
    {
        TimedCounts counts;
        CHECK(!counts.Due(0, 0));
        // window 1: the chained hook reached 590 times and ran 589, one scene-HDR decline
        for (int i = 0; i < 590; ++i)
            counts.NoteReached(Entry::Chain);
        for (int i = 0; i < 589; ++i)
            counts.NoteRan(Entry::Chain);
        counts.NoteDeclined(Decline::SceneHdr);
        CHECK(counts.Due(10000, 600));
        Params params;
        params.density = 3.0f;
        params.start = 40.0f;
        params.sunStrength = 0.5f; // explicit: the line prints the settings, and the defaults are L2b's business
        const std::string first = counts.TakeLine(10000, 600, params);
        CHECK(Contains(first, "Atmosphere: in the last 10.0 s (600 rendered frames)"));
        CHECK(Contains(first, "reached 590 times (own seam 0, chained hook 590, owner hook 0)"));
        CHECK(Contains(first, "ran 589 times (own seam 0, chained hook 589, owner hook 0)"));
        CHECK(Contains(first, "declined 1 (scene HDR 1)"));
        CHECK(Contains(first, "skipped 0;"));
        CHECK(Contains(first, "since the start: 600 frames, reached 590, ran 589, declined 1, skipped 0"));
        CHECK(Contains(first, "density 3.00/km") && Contains(first, "start 40 m") && Contains(first, "sun 0.50"));
        CHECK(Contains(first, "night 0.10, neutral 0.60")); // the L2b defaults of the two keys the settings tail ends with
        // the window is empty now; the total kept the first window
        CHECK_EQ(counts.Window().Reached(), 0u);
        CHECK_EQ(counts.Total().Reached(), 590u);
        CHECK_EQ(counts.Total().Ran(), 589u);
        CHECK(!counts.Due(19999, 1100));
        // window 2: nobody reached the stage (the heartbeat's case): the line says so and the total does not move
        CHECK(counts.Due(20000, 1200));
        const std::string second = counts.TakeLine(20000, 1200, params);
        CHECK(Contains(second, "reached 0 times (own seam 0, chained hook 0, owner hook 0)"));
        CHECK(Contains(second, "ran 0 times"));
        CHECK(Contains(second, "since the start: 1200 frames, reached 590, ran 589, declined 1, skipped 0"));
        // window 3: the running total keeps rising across the lines (the session check)
        for (int i = 0; i < 590; ++i)
        {
            counts.NoteReached(Entry::Seam);
            counts.NoteRan(Entry::Seam);
        }
        counts.NoteSkipped(SkipReason::NoDepth);
        counts.NoteDeclined(Decline::NoCamera);
        counts.NoteDeclined(Decline::NoCamera);
        CHECK(counts.Due(30000, 1800));
        const std::string third = counts.TakeLine(30000, 1800, params);
        CHECK(Contains(third, "reached 590 times (own seam 590, chained hook 0, owner hook 0)"));
        CHECK(Contains(third, "ran 590 times (own seam 590, chained hook 0, owner hook 0)"));
        CHECK(Contains(third, "declined 2 (no camera 2)"));
        CHECK(Contains(third, "skipped 1 (depth 1)"));
        CHECK(Contains(third, "since the start: 1800 frames, reached 1180, ran 1179, declined 3, skipped 1"));
        CHECK_EQ(counts.Total().Ran(), 1179u);
        // a reset forgets everything, including the clock
        counts.Reset();
        CHECK_EQ(counts.Total().Reached(), 0u);
        CHECK(!counts.Due(100000, 9000));
    }

    // ---- the text of a busy window --------------------------------------------------------------------------------------------------------
    {
        Tally window;
        window.reached[0] = 1;
        window.reached[1] = 2;
        window.reached[2] = 3;
        window.ran[0] = 1;
        window.ran[1] = 1;
        window.ran[2] = 1;
        window.declined[uint32_t(Decline::SceneHdr)] = 4;
        window.declined[uint32_t(Decline::CharaReplay)] = 5;
        window.skipped[0] = 6;
        window.skipped[1] = 7;
        window.skipped[2] = 8;
        const std::string line = FormatTimedCounts(window, window, 9500, 570, 12345, Params {});
        CHECK(Contains(line, "in the last 9.5 s (570 rendered frames)"));
        CHECK(Contains(line, "reached 6 times (own seam 1, chained hook 2, owner hook 3)"));
        CHECK(Contains(line, "declined 9 (character preview 5, scene HDR 4)")); // in the order of the enum
        CHECK(Contains(line, "skipped 21 (depth 6, camera 7, fog 8)"));
        CHECK(Contains(line, "since the start: 12345 frames"));
        // a decline that did not happen is not in the list
        CHECK(!Contains(line, "no depth") && !Contains(line, "exception"));
    }

    return Finish("atmo_counts_rules");
}
