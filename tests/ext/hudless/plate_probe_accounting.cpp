// Output-stream accounting of the developer probe (ext/hudless/PlateProbeAccounting.h): a pure state machine fed with QPC-ordered
// events. One tick is one microsecond in these streams. The window count is measured from the stream, so the dropped-output and
// the bunched-present streams must LOWER the percentage and change the histogram; first-in-first-out assignment only measures lag.
#include "ext/hudless/PlateProbeAccounting.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
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

constexpr uint64_t kPeriod = 16000; // 62.5 real frames per second
constexpr uint64_t kStart = 1000000;

struct Stream
{
    std::vector<Event> events;
    uint64_t seq = 0;

    uint64_t Post(uint64_t qpc, uint64_t frame, uint32_t multiplier, bool withOptions = true)
    {
        Event p;
        p.type = EventType::Post;
        p.qpc = qpc;
        p.thread = 1;
        p.seq = ++seq;
        p.frame = frame;
        events.push_back(p);
        if (withOptions)
        {
            Event o;
            o.type = EventType::Options;
            o.qpc = qpc + 10;
            o.thread = 1;
            o.seq = p.seq;
            o.multiplier = multiplier;
            events.push_back(o);
        }
        return p.seq;
    }
    void Output(uint64_t qpc, uint32_t thread = 2, int32_t backBuffer = -1)
    {
        Event e;
        e.type = EventType::Output;
        e.qpc = qpc;
        e.thread = thread;
        e.backBuffer = backBuffer;
        events.push_back(e);
    }
    void Reset(uint64_t qpc)
    {
        Event e;
        e.type = EventType::Reset;
        e.qpc = qpc;
        events.push_back(e);
    }
};

struct Result
{
    Summary summary;
    std::vector<OutputRecord> records;
};

Result Run(std::vector<Event> events)
{
    EventMerger merger(100000);
    for (const Event& e : events)
        merger.Push(e);
    std::vector<Event> ordered;
    merger.ReleaseAll(ordered);
    Accounting accounting;
    Result r;
    for (const Event& e : ordered)
    {
        OutputRecord record;
        if (accounting.Process(e, record))
            r.records.push_back(record);
    }
    r.summary = accounting.GetSummary();
    return r;
}

// A steady stream: frame n (1-based) is posted at kStart + n * period; its M outputs arrive `lag` batches later, spread inside one
// period. dropBatch/dropIndex remove one output; delayBatch moves the two outputs of one batch one period later (bunching).
struct Spec
{
    uint32_t multiplier = 2;
    uint32_t lag = 0;
    uint32_t frames = 100;
    uint32_t dropBatch = 0, dropIndex = 0;
    uint32_t delayBatch = 0;
    uint32_t changeAt = 0, changeTo = 0; // multiplier from frame changeAt on
    uint32_t threadOf0 = 2, threadOf1 = 2;
};

Stream Make(const Spec& s)
{
    Stream st;
    // `lag` more Posts follow the last frame so that the last batches arrive in a window of their own, like in a running game
    for (uint32_t n = 1; n <= s.frames + s.lag; ++n)
    {
        const uint32_t m = (s.changeAt && n >= s.changeAt) ? s.changeTo : s.multiplier;
        st.Post(kStart + n * kPeriod, 100 + n, m);
    }
    for (uint32_t n = 1; n <= s.frames; ++n)
    {
        const uint32_t m = (s.changeAt && n >= s.changeAt) ? s.changeTo : s.multiplier;
        for (uint32_t k = 0; k < m; ++k)
        {
            if (n == s.dropBatch && k == s.dropIndex)
                continue;
            uint64_t t = kStart + n * kPeriod + uint64_t(s.lag) * kPeriod + 2000 + k * (13000 / m);
            if (n == s.delayBatch)
                t += kPeriod;
            st.Output(t, k % 2 ? s.threadOf1 : s.threadOf0);
        }
    }
    return st;
}

void SteadyStreams()
{
    for (uint32_t m : { 2u, 3u, 4u })
        for (uint32_t lag : { 0u, 1u, 2u })
        {
            Spec s;
            s.multiplier = m;
            s.lag = lag;
            const Result r = Run(Make(s).events);
            const Summary& u = r.summary;
            EXPECT(u.posts == 100 + lag && u.windows == 99 + lag, "m=%u lag=%u posts=%llu windows=%llu", m, lag, (unsigned long long) u.posts,
                   (unsigned long long) u.windows);
            // the first three windows are not judged; every other window holds exactly M outputs
            EXPECT(u.windowsTransition == kSettleWindows, "m=%u lag=%u transition=%llu", m, lag, (unsigned long long) u.windowsTransition);
            EXPECT(u.windowsKnown == u.windows - kSettleWindows && u.windowsEqual == u.windowsKnown, "m=%u lag=%u known=%llu equal=%llu", m,
                   lag, (unsigned long long) u.windowsKnown, (unsigned long long) u.windowsEqual);
            EXPECT(u.Percent() == 100.0, "m=%u lag=%u percent=%f", m, lag, u.Percent());
            EXPECT(u.countHistogram.count(m) == 1 && u.countHistogram.at(m) == 99, "m=%u lag=%u hist[m]", m, lag);
            EXPECT(u.maxLagPosts == int32_t(lag), "m=%u lag=%u maxLagPosts=%d", m, lag, u.maxLagPosts);
            EXPECT(u.lateOutputs == (lag ? u.outputs : 0), "m=%u lag=%u late=%llu outputs=%llu", m, lag, (unsigned long long) u.lateOutputs,
                   (unsigned long long) u.outputs);
            EXPECT(u.outputs == uint64_t(100) * m, "outputs %llu", (unsigned long long) u.outputs);
            EXPECT(u.firstLag.size() == 1 && u.firstLag.count(int32_t(lag)) == 1 && u.firstLag.at(int32_t(lag)) == 100,
                   "m=%u lag=%u firstLag", m, lag);
            EXPECT(u.maxLagTicks > int64_t(lag) * int64_t(kPeriod) && u.maxLagTicks < int64_t(lag + 1) * int64_t(kPeriod),
                   "m=%u lag=%u maxLagTicks=%lld", m, lag, (long long) u.maxLagTicks);
            EXPECT(u.optionsEvents == 100 + lag && u.orphanOptions == 0, "options");
            // every output got a batch with the right index and multiplier
            for (const OutputRecord& o : r.records)
            {
                EXPECT(o.batch != 0 && o.indexInBatch >= 1 && o.indexInBatch <= m && o.multiplier == m, "record m=%u", m);
                EXPECT(o.lagPosts == int32_t(lag), "record lag %d expected %u", o.lagPosts, lag);
            }
        }
}

// A dropped output leaves a window that is one short: the percentage must fall and the histogram must show it.
void DroppedOutput()
{
    Spec s;
    s.multiplier = 3;
    s.dropBatch = 50;
    s.dropIndex = 1;
    const Result r = Run(Make(s).events);
    const Summary& u = r.summary;
    EXPECT(u.countHistogram.count(2) == 1 && u.countHistogram.at(2) == 1, "one window with two outputs");
    EXPECT(u.windowsKnown == 99 - kSettleWindows && u.windowsEqual == u.windowsKnown - 1, "known=%llu equal=%llu", (unsigned long long) u.windowsKnown,
           (unsigned long long) u.windowsEqual);
    EXPECT(u.Percent() < 100.0 && u.Percent() > 98.0, "percent=%f", u.Percent());
    // the missing output also shifts the first-in-first-out view of everything after it by one output: lag grows
    EXPECT(u.maxLagPosts >= 1, "maxLagPosts=%d", u.maxLagPosts);
}

// Two batches delivered together: one empty window followed by a double one. The percentage falls by two windows.
void BunchedPresents()
{
    Spec s;
    s.multiplier = 2;
    s.delayBatch = 40; // batch 40's outputs arrive in the window of batch 41
    const Result r = Run(Make(s).events);
    const Summary& u = r.summary;
    EXPECT(u.countHistogram.count(0) == 1 && u.countHistogram.at(0) == 1, "an empty window");
    EXPECT(u.countHistogram.count(4) == 1 && u.countHistogram.at(4) == 1, "a window with four outputs");
    EXPECT(u.windowsEqual == u.windowsKnown - 2, "known=%llu equal=%llu", (unsigned long long) u.windowsKnown, (unsigned long long) u.windowsEqual);
    EXPECT(u.maxLagPosts == 1, "maxLagPosts=%d", u.maxLagPosts);
}

// The multiplier changes from 2 to 3 while the pacer trails by one batch: the windows around the change are set aside, the rest are exact.
void MultiplierChange()
{
    Spec s;
    s.multiplier = 2;
    s.lag = 1;
    s.changeAt = 41;
    s.changeTo = 3;
    const Result r = Run(Make(s).events);
    const Summary& u = r.summary;
    EXPECT(u.windowsTransition == 2 * kSettleWindows, "transition=%llu", (unsigned long long) u.windowsTransition);
    EXPECT(u.windowsEqual == u.windowsKnown && u.windowsKnown == u.windows - 2 * kSettleWindows, "known=%llu equal=%llu",
           (unsigned long long) u.windowsKnown, (unsigned long long) u.windowsEqual);
    EXPECT(u.countHistogram.count(2) == 1 && u.countHistogram.count(3) == 1, "both counts seen");
}

// DLSS-G switched off and on: the open window and the batches are forgotten, stragglers do not attach to old frames, and the
// first windows after the restart are not judged.
void Restart()
{
    Spec s;
    s.multiplier = 2;
    s.lag = 1;
    s.frames = 30;
    Stream st = Make(s);
    const uint64_t resetAt = kStart + 31 * kPeriod + 500; // after the last Post of the first run and before its trailing outputs
    st.Reset(resetAt);
    for (uint32_t n = 1; n <= 60; ++n)
    {
        st.Post(resetAt + 1000 + n * kPeriod, 5000 + n, 4);
    }
    for (uint32_t n = 1; n <= 60; ++n)
        for (uint32_t k = 0; k < 4; ++k)
            st.Output(resetAt + 1000 + n * kPeriod + 1000 + k * 3000, 3);
    const Result r = Run(st.events);
    const Summary& u = r.summary;
    EXPECT(u.resets == 1, "resets=%llu", (unsigned long long) u.resets);
    // outputs of the first run that arrive after the reset find no frame
    EXPECT(u.outputsBeforePost >= 1, "outputsBeforePost=%llu", (unsigned long long) u.outputsBeforePost);
    // the first output after the new first Post belongs to the new first frame
    const uint64_t firstNew = 32; // sequence numbers continue: the first run used 1..31 (30 frames and the one trailing Post of the lag)
    bool seen = false;
    for (const OutputRecord& o : r.records)
        if (o.thread == 3 && !seen)
        {
            seen = true;
            EXPECT(o.batch == firstNew, "batch=%llu", (unsigned long long) o.batch);
            EXPECT(o.lagPosts == 0, "lag=%d", o.lagPosts);
        }
    EXPECT(seen, "a record of the new run");
    // the new run is judged from its fourth window on and every window holds four
    EXPECT(u.countHistogram.count(4) == 1, "four-output windows");
}

// The game's frame number steps back or jumps far ahead: the stream restarted.
void FrameNumberJump()
{
    Accounting a;
    a.OnPost(1, 100, 1000);
    a.OnPost(2, 101, 2000);
    a.OnPost(3, 101, 3000); // the same number twice is not a jump
    a.OnPost(4, 104, 4000); // small steps are fine
    EXPECT(a.GetSummary().resets == 0, "resets=%llu", (unsigned long long) a.GetSummary().resets);
    a.OnPost(5, 4 + 100 + kMaxFrameJump, 5000); // exactly at the limit
    EXPECT(a.GetSummary().resets == 0, "at the limit");
    a.OnPost(6, 104 + 2 * kMaxFrameJump + 2, 6000); // beyond it
    EXPECT(a.GetSummary().resets == 1, "forward jump");
    a.OnPost(7, 3, 7000); // backwards
    EXPECT(a.GetSummary().resets == 2, "backward jump");
    // after a jump the new Post is the first one: an output goes to it, not to an earlier frame
    a.OnMultiplier(7, 2);
    const OutputRecord o = a.OnOutput(7100, 2, -1);
    EXPECT(o.batch == 7 && o.lagPosts == 0 && o.indexInBatch == 1, "batch=%llu lag=%d", (unsigned long long) o.batch, o.lagPosts);
}

void ThreadChange()
{
    Spec s;
    s.multiplier = 2;
    s.threadOf0 = 7;
    s.threadOf1 = 9;
    const Result r = Run(Make(s).events);
    EXPECT(r.summary.threads.size() == 2 && r.summary.threads.at(7) == 100 && r.summary.threads.at(9) == 100, "threads");
    // the output stream moves to another thread half way
    Stream st;
    for (uint32_t n = 1; n <= 40; ++n)
        st.Post(kStart + n * kPeriod, n, 2);
    for (uint32_t n = 1; n <= 40; ++n)
        for (uint32_t k = 0; k < 2; ++k)
            st.Output(kStart + n * kPeriod + 2000 + k * 6000, n <= 20 ? 11 : 12);
    const Result r2 = Run(st.events);
    EXPECT(r2.summary.threads.size() == 2 && r2.summary.threads.at(11) == 40 && r2.summary.threads.at(12) == 40, "moved threads");
    EXPECT(r2.summary.windowsEqual == r2.summary.windowsKnown, "a thread change alone changes no count");
}

void UnknownMultiplier()
{
    Stream st;
    for (uint32_t n = 1; n <= 30; ++n)
        st.Post(kStart + n * kPeriod, n, 0, false);
    for (uint32_t n = 1; n <= 30; ++n)
        for (uint32_t k = 0; k < 2; ++k)
            st.Output(kStart + n * kPeriod + 2000 + k * 6000);
    const Result r = Run(st.events);
    const Summary& u = r.summary;
    EXPECT(u.windows == 29 && u.windowsUnknown == 29 && u.windowsKnown == 0 && u.Percent() == 0.0, "unknown windows");
    EXPECT(u.optionsEvents == 0, "no options");
    // an unknown multiplier assigns every output of the window to its own frame, with no multiplier
    for (const OutputRecord& o : r.records)
        EXPECT(o.multiplier == 0 && o.batch != 0, "record");
    // an Options event for a frame that is not there is counted, not applied
    Accounting a;
    a.OnPost(1, 1, 100);
    a.OnMultiplier(99, 2);
    EXPECT(a.GetSummary().orphanOptions == 1, "orphan options");
}

void OutputsBeforeFirstPostAndAhead()
{
    Accounting a;
    OutputRecord o = a.OnOutput(10, 5, 1);
    EXPECT(o.batch == 0 && o.window == 0 && o.lagPosts == -1, "before the first Post");
    EXPECT(a.GetSummary().outputsBeforePost == 1, "counted");
    a.OnPost(1, 1, 100);
    a.OnMultiplier(1, 2);
    a.OnOutput(110, 5, 0);
    a.OnOutput(120, 5, 1);
    o = a.OnOutput(130, 5, 0); // a third output for a frame that asked for two, and no newer frame
    EXPECT(o.batch == 0 && o.lagPosts == -1, "more outputs than asked");
    EXPECT(a.GetSummary().outputsAhead == 1, "ahead counted");
    EXPECT(o.sinceLastTicks == 10, "since last %lld", (long long) o.sinceLastTicks);
}

// Events pushed by several threads in the wrong order are put back in QPC order, whatever the push order was.
void MergerOrdering()
{
    Spec s;
    s.multiplier = 3;
    s.lag = 1;
    const Stream st = Make(s);
    const Result baseline = Run(st.events);

    std::vector<Event> shuffled = st.events;
    std::mt19937 rng(12345);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    const Result r = Run(shuffled);
    EXPECT(r.summary.outputs == baseline.summary.outputs, "outputs");
    EXPECT(r.summary.windowsEqual == baseline.summary.windowsEqual && r.summary.windowsKnown == baseline.summary.windowsKnown, "windows");
    EXPECT(r.summary.maxLagPosts == baseline.summary.maxLagPosts && r.summary.maxLagTicks == baseline.summary.maxLagTicks, "lag");
    EXPECT(r.records.size() == baseline.records.size(), "records");
    for (size_t i = 0; i < r.records.size() && i < baseline.records.size(); ++i)
        EXPECT(r.records[i].qpc == baseline.records[i].qpc && r.records[i].batch == baseline.records[i].batch, "record %zu", i);

    // equal QPC: the Post comes before its Options, which come before the outputs
    EventMerger m(0);
    Event out;
    out.type = EventType::Output;
    out.qpc = 500;
    Event opt;
    opt.type = EventType::Options;
    opt.qpc = 500;
    Event post;
    post.type = EventType::Post;
    post.qpc = 500;
    Event reset;
    reset.type = EventType::Reset;
    reset.qpc = 500;
    for (const Event& e : { out, opt, post, reset })
        m.Push(e);
    std::vector<Event> ordered;
    m.ReleaseAll(ordered);
    EXPECT(ordered.size() == 4 && ordered[0].type == EventType::Reset && ordered[1].type == EventType::Post &&
               ordered[2].type == EventType::Options && ordered[3].type == EventType::Output,
           "tie order");
    EXPECT(m.Reordered() == 0, "same QPC is not a reordering");
}

// A producer that read the clock and pushed after a flush: the hold-back keeps recent events for the next flush, and an event that
// still comes in below what was released is counted.
void MergerHoldBack()
{
    EventMerger m(100);
    std::vector<Event> out;
    auto event = [](EventType t, uint64_t qpc)
    {
        Event e;
        e.type = t;
        e.qpc = qpc;
        return e;
    };
    m.Push(event(EventType::Output, 1000));
    m.Push(event(EventType::Output, 1090)); // newer than now - holdBack
    m.Release(1150, out);                    // cutoff 1050
    EXPECT(out.size() == 1 && out[0].qpc == 1000, "only the old event is released");
    EXPECT(m.Pending() == 1, "the recent one waits");
    m.Push(event(EventType::Post, 1060)); // the Post was read before the output but pushed after the flush
    EXPECT(m.Late() == 0, "1060 is above the released 1000");
    m.Release(1300, out); // cutoff 1200
    EXPECT(out.size() == 3 && out[1].qpc == 1060 && out[1].type == EventType::Post && out[2].qpc == 1090,
           "the late-pushed Post is back in order with the held output");
    // below what was already released
    m.Push(event(EventType::Output, 900));
    EXPECT(m.Late() == 1, "late counted");
    out.clear();
    m.Release(1400, out);
    EXPECT(out.size() == 1 && out[0].qpc == 900, "still handed out");
    // pushed out of QPC order by two producers
    EventMerger r(0);
    r.Push(event(EventType::Output, 50));
    r.Push(event(EventType::Output, 40));
    r.Push(event(EventType::Output, 60));
    EXPECT(r.Reordered() == 1, "one push went backwards: %llu", (unsigned long long) r.Reordered());
    // the cutoff never underflows
    EventMerger z(1000);
    std::vector<Event> none;
    z.Push(event(EventType::Output, 5));
    z.Release(10, none);
    EXPECT(none.empty() && z.Pending() == 1, "hold-back longer than the clock");
}

// Two batches that straddle a Post: the first-in-first-out view tells which frame each output belongs to even when the window does not.
void BatchAssignment()
{
    Accounting a;
    a.OnPost(1, 1, 1000);
    a.OnMultiplier(1, 2);
    a.OnPost(2, 2, 2000);
    a.OnMultiplier(2, 3);
    a.OnPost(3, 3, 3000);
    a.OnMultiplier(3, 2);
    // four outputs arrive in the window of Post 3: two finish frame 1 (arrived late), two start frame 2... no: assignment is by slots
    const OutputRecord o1 = a.OnOutput(3100, 2, -1);
    const OutputRecord o2 = a.OnOutput(3200, 2, -1);
    const OutputRecord o3 = a.OnOutput(3300, 2, -1);
    EXPECT(o1.batch == 1 && o1.indexInBatch == 1 && o1.lagPosts == 2, "o1 batch=%llu lag=%d", (unsigned long long) o1.batch, o1.lagPosts);
    EXPECT(o2.batch == 1 && o2.indexInBatch == 2, "o2");
    EXPECT(o3.batch == 2 && o3.indexInBatch == 1 && o3.multiplier == 3 && o3.lagPosts == 1, "o3 batch=%llu lag=%d", (unsigned long long) o3.batch, o3.lagPosts);
    EXPECT(o1.lagTicks == 2100 && o3.lagTicks == 1300, "lag ticks %lld %lld", (long long) o1.lagTicks, (long long) o3.lagTicks);
    EXPECT(o2.window == 3 && o2.sinceLastTicks == 100, "window and spacing");
}
// Is the multiplier noted for the right frame? The thread of the Options event against its Post's, and how long after the Post it came.
void OptionsPhase()
{
    // the usual case: on the Post's thread, 10 ticks after the Post, a Post every 16000 ticks
    Spec s;
    s.frames = 40;
    const Result steady = Run(Make(s).events);
    const Summary& u = steady.summary;
    EXPECT(u.optionsEvents == 40 && u.optionsMatched == 40 && u.optionsSameThread == 40 && u.orphanOptions == 0, "all matched, same thread");
    EXPECT(u.optionsDelayTicks == 400 && u.optionsDelayMaxTicks == 10, "delay %llu max %lld", (unsigned long long) u.optionsDelayTicks,
           (long long) u.optionsDelayMaxTicks);
    EXPECT(u.postIntervals == 39 && u.postIntervalTicks == 39 * kPeriod, "39 gaps between 40 Posts: %llu", (unsigned long long) u.postIntervals);

    // an Options event pushed from another thread than its Post is not counted as the same thread (through the event path)
    Stream other;
    for (uint32_t n = 1; n <= 10; ++n)
    {
        other.Post(kStart + n * kPeriod, n, 2, false);
        Event o;
        o.type = EventType::Options;
        o.qpc = kStart + n * kPeriod + 20;
        o.thread = n % 2 ? 1 : 3; // odd frames: the Post's thread (1), even frames: another
        o.seq = n;
        o.multiplier = 2;
        other.events.push_back(o);
    }
    const Result mixed = Run(other.events);
    EXPECT(mixed.summary.optionsMatched == 10 && mixed.summary.optionsSameThread == 5, "same thread %llu of %llu",
           (unsigned long long) mixed.summary.optionsSameThread, (unsigned long long) mixed.summary.optionsMatched);
    EXPECT(mixed.summary.optionsDelayTicks == 200 && mixed.summary.optionsDelayMaxTicks == 20, "their delay");

    // an Options event whose Post is not there is counted as orphaned and is neither on the thread nor delayed
    Accounting a;
    a.OnPost(1, 1, 1000, 1);
    a.OnMultiplier(1, 2, 1, 1010);
    a.OnMultiplier(99, 2, 1, 1020);
    const Summary& o = a.GetSummary();
    EXPECT(o.optionsEvents == 2 && o.optionsMatched == 1 && o.orphanOptions == 1 && o.optionsSameThread == 1 && o.optionsDelayTicks == 10,
           "orphan: %llu matched %llu", (unsigned long long) o.orphanOptions, (unsigned long long) o.optionsMatched);

    // the phase error the check is for: the multiplier of frame N+1 noted just before Post N+1 gets tagged with Post N, and arrives about
    // one Post interval after it. The delay is then close to the Post interval, which the summary prints beside it.
    Accounting late;
    for (uint64_t n = 1; n <= 10; ++n)
    {
        late.OnPost(n, n, n * kPeriod, 1);
        late.OnMultiplier(n, 2, 1, n * kPeriod + kPeriod - 10);
    }
    const Summary& l = late.GetSummary();
    EXPECT(l.optionsMatched == 10 && l.optionsDelayTicks / l.optionsMatched == kPeriod - 10, "a frame late: delay %llu",
           (unsigned long long) (l.optionsDelayTicks / l.optionsMatched));
    EXPECT(l.postIntervalTicks / l.postIntervals == kPeriod, "the Post interval next to it");

    // no interval is measured across a restart
    Accounting jump;
    jump.OnPost(1, 1, 1000);
    jump.OnPost(2, 2, 2000);
    jump.OnPost(3, 500, 3000); // frame number jump: the stream restarted
    EXPECT(jump.GetSummary().postIntervals == 1 && jump.GetSummary().postIntervalTicks == 1000 && jump.GetSummary().resets == 1, "one gap, then a reset");
    jump.OnPost(4, 501, 3100);
    EXPECT(jump.GetSummary().postIntervals == 2 && jump.GetSummary().postIntervalTicks == 1100, "and counting again");
}

void TicksSincePost()
{
    Accounting a;
    OutputRecord r = a.OnOutput(500, 2, -1);
    EXPECT(r.ticksSincePost == -1 && r.window == 0, "before the first Post");
    a.OnPost(1, 1, 1000);
    r = a.OnOutput(1250, 2, -1);
    EXPECT(r.ticksSincePost == 250 && r.window == 1, "250 ticks into the window of Post 1");
    r = a.OnOutput(1000, 2, -1);
    EXPECT(r.ticksSincePost == 0, "an output stamped before its window's Post (merge tie) is 0, not negative");
    a.OnPost(2, 2, 2000);
    r = a.OnOutput(2100, 2, -1);
    EXPECT(r.ticksSincePost == 100 && r.window == 2, "the window of Post 2");
}

// The lag maxima exist twice: for the whole run and for the time since the last summary line.
void IntervalLag()
{
    Accounting a;
    // a start-up glitch: both outputs of frame 1 arrive after Post 3
    a.OnPost(1, 1, 1000);
    a.OnMultiplier(1, 2);
    a.OnPost(2, 2, 2000);
    a.OnMultiplier(2, 2);
    a.OnPost(3, 3, 3000);
    a.OnMultiplier(3, 2);
    a.OnOutput(3100, 2, -1);
    a.OnOutput(3200, 2, -1);
    {
        const Summary& g = a.GetSummary();
        EXPECT(g.maxLagPosts == 2 && g.maxLagTicks == 2200 && g.lateOutputs == 2 && g.firstLag.at(2) == 1, "cumulative sees the glitch");
        EXPECT(g.interval.maxLagPosts == 2 && g.interval.maxLagTicks == 2200 && g.interval.lateOutputs == 2 && g.interval.firstLag.at(2) == 1,
               "and so does the first interval");
    }
    a.ResetInterval();
    {
        const Summary& g = a.GetSummary();
        EXPECT(g.interval.maxLagPosts == 0 && g.interval.maxLagTicks == 0 && g.interval.lateOutputs == 0 && g.interval.firstLag.empty(), "interval cleared");
        EXPECT(g.maxLagPosts == 2 && g.maxLagTicks == 2200 && g.lateOutputs == 2 && g.firstLag.size() == 1 && g.firstLag.at(2) == 1,
               "the cumulative values are untouched");
    }
    // a second interval without a glitch: the stream restarted and outputs follow their Post at once
    a.OnPost(4, 500, 4000);
    a.OnMultiplier(4, 2);
    a.OnOutput(4100, 2, -1);
    a.OnOutput(4200, 2, -1);
    {
        const Summary& g = a.GetSummary();
        EXPECT(g.interval.maxLagPosts == 0 && g.interval.maxLagTicks == 200 && g.interval.lateOutputs == 0 && g.interval.firstLag.size() == 1 &&
                   g.interval.firstLag.at(0) == 1,
               "the second interval shows only its own: %d posts %lld ticks", g.interval.maxLagPosts, (long long) g.interval.maxLagTicks);
        EXPECT(g.maxLagPosts == 2 && g.maxLagTicks == 2200 && g.lateOutputs == 2 && g.firstLag.at(2) == 1 && g.firstLag.at(0) == 1,
               "the whole-run values keep the first glitch and add the new batch");
    }
    // a stream restart (Reset) is not an interval boundary: only the summary line is
    a.Reset();
    EXPECT(a.GetSummary().resets == 2 && a.GetSummary().interval.maxLagTicks == 200 && a.GetSummary().interval.firstLag.size() == 1,
           "a restart keeps the interval");
    // resetting twice, or with nothing in it, changes nothing else
    a.ResetInterval();
    a.ResetInterval();
    EXPECT(a.GetSummary().outputs == 4 && a.GetSummary().resets == 2 && a.GetSummary().interval.firstLag.empty() && a.GetSummary().maxLagTicks == 2200,
           "nothing else moves");
}

// The tuning constants are written out so that changing one means changing this test as well.
static_assert(kSettleWindows == 3, "three windows are set aside after a start, a reset and a multiplier change");
static_assert(kMaxFrameJump == 8, "a game frame number more than 8 ahead is a restart");
static_assert(kKeepPosts == 16, "sixteen Posts are kept");

// Posts with consecutive frame numbers and 1000 ticks apart, each with its multiplier noted.
struct Feed
{
    Accounting a;
    uint64_t seq = 0, frame = 100, qpc = 1000;
    uint64_t Post(uint32_t multiplier)
    {
        ++seq;
        ++frame;
        qpc += 1000;
        a.OnPost(seq, frame, qpc);
        a.OnMultiplier(seq, multiplier);
        return seq;
    }
    OutputRecord Output()
    {
        qpc += 10;
        return a.OnOutput(qpc, 2, -1);
    }
};

// Literal windows: 8 Posts close 7 windows, the first 3 of which are not judged.
void SettleLiterals()
{
    Feed f;
    for (int i = 0; i < 8; ++i)
        f.Post(2);
    const Summary& u = f.a.GetSummary();
    EXPECT(u.windows == 7 && u.windowsTransition == 3 && u.windowsKnown == 4 && u.windowsUnknown == 0, "start: windows=%llu transition=%llu known=%llu",
           (unsigned long long) u.windows, (unsigned long long) u.windowsTransition, (unsigned long long) u.windowsKnown);

    // a Reset starts the settle again: 5 more Posts close 4 windows, 3 of them not judged
    f.a.Reset();
    for (int i = 0; i < 5; ++i)
        f.Post(2);
    EXPECT(u.resets == 1 && u.windowsTransition == 6 && u.windowsKnown == 5, "after the Reset: transition=%llu known=%llu",
           (unsigned long long) u.windowsTransition, (unsigned long long) u.windowsKnown);
}

// The multiplier goes down from 4 to 2: that settles again, like a rise does.
void MultiplierDecrease()
{
    Feed f;
    for (int i = 0; i < 8; ++i)
        f.Post(4);
    for (int i = 0; i < 9; ++i)
        f.Post(2);
    const Summary& u = f.a.GetSummary();
    // 16 closed windows: 3 not judged after the start, 3 not judged after the change to 2, the other 10 judged
    EXPECT(u.windows == 16 && u.windowsTransition == 6 && u.windowsKnown == 10 && u.windowsUnknown == 0, "decrease: transition=%llu known=%llu",
           (unsigned long long) u.windowsTransition, (unsigned long long) u.windowsKnown);
}

// A multiplier of 1 (frame generation off) is a known multiplier, not an unknown one.
void MultiplierOne()
{
    Feed f;
    for (int i = 0; i < 8; ++i)
    {
        f.Post(1);
        f.Output();
    }
    const Summary& u = f.a.GetSummary();
    EXPECT(u.windows == 7 && u.windowsUnknown == 0 && u.windowsTransition == 3 && u.windowsKnown == 4 && u.windowsEqual == 4,
           "multiplier 1: unknown=%llu known=%llu equal=%llu", (unsigned long long) u.windowsUnknown, (unsigned long long) u.windowsKnown,
           (unsigned long long) u.windowsEqual);
}

// A frame number exactly 8 ahead keeps the stream, 9 ahead restarts it.
void FrameJumpLiterals()
{
    Accounting a;
    a.OnPost(1, 100, 1000);
    a.OnPost(2, 108, 2000);
    EXPECT(a.GetSummary().resets == 0, "100 -> 108 is the limit");
    a.OnPost(3, 117, 3000);
    EXPECT(a.GetSummary().resets == 1, "108 -> 117 is 9 ahead");
    a.OnPost(4, 125, 4000);
    EXPECT(a.GetSummary().resets == 1, "117 -> 125 is 8 ahead");
}

// The pacer trails by 15 batches: the oldest of the 16 Posts kept is still there and its output gets lag 15.
void LagFifteen()
{
    Feed f;
    for (int i = 0; i < 16; ++i)
        f.Post(1);
    const OutputRecord first = f.Output();
    EXPECT(first.batch == 1 && first.lagPosts == 15 && first.indexInBatch == 1, "batch=%llu lag=%d", (unsigned long long) first.batch, first.lagPosts);
    EXPECT(f.a.GetSummary().maxLagPosts == 15 && f.a.GetSummary().lateOutputs == 1, "lag 15 is counted");
    // the 17th Post drops the oldest one
    f.Post(1);
    const OutputRecord next = f.Output();
    EXPECT(next.batch == 2 && next.lagPosts == 15, "after the 17th Post: batch=%llu lag=%d", (unsigned long long) next.batch, next.lagPosts);
}

// Two Posts stamped with the same tick are not an interval; the lag of a batch is the first output's, kept at index 1.
void IntervalEdges()
{
    Accounting a;
    a.OnPost(1, 1, 1000);
    a.OnPost(2, 2, 1000);
    EXPECT(a.GetSummary().postIntervals == 0 && a.GetSummary().postIntervalTicks == 0, "identical ticks");

    // first-output lag is taken at the first output of a batch: batch 1 gets its first output in its own window and its second after
    // the next Post; batch 2 gets its first output in its own window
    Accounting b;
    b.OnPost(1, 1, 1000);
    b.OnMultiplier(1, 2);
    const OutputRecord o1 = b.OnOutput(1100, 2, -1);
    b.OnPost(2, 2, 2000);
    b.OnMultiplier(2, 2);
    const OutputRecord o2 = b.OnOutput(2100, 2, -1);
    const OutputRecord o3 = b.OnOutput(2200, 2, -1);
    EXPECT(o1.batch == 1 && o1.indexInBatch == 1 && o1.lagPosts == 0, "o1");
    EXPECT(o2.batch == 1 && o2.indexInBatch == 2 && o2.lagPosts == 1, "o2");
    EXPECT(o3.batch == 2 && o3.indexInBatch == 1 && o3.lagPosts == 0, "o3");
    const Summary& u = b.GetSummary();
    EXPECT(u.firstLag.size() == 1 && u.firstLag.count(0) == 1 && u.firstLag.at(0) == 2, "firstLag {0:2}, size %zu", u.firstLag.size());
    EXPECT(u.interval.firstLag.size() == 1 && u.interval.firstLag.count(0) == 1 && u.interval.firstLag.at(0) == 2, "interval firstLag {0:2}");
    EXPECT(u.lateOutputs == 1 && u.interval.lateOutputs == 1, "one late output");

    // the interval maximum is a maximum: outputs with lag 2, 1 and 0 in this order leave 2, not the last value
    Accounting c;
    c.OnPost(1, 1, 1000);
    c.OnMultiplier(1, 1);
    c.OnPost(2, 2, 2000);
    c.OnMultiplier(2, 1);
    c.OnPost(3, 3, 3000);
    c.OnMultiplier(3, 1);
    c.OnOutput(3100, 2, -1);
    c.OnOutput(3200, 2, -1);
    c.OnOutput(3300, 2, -1);
    const Summary& v = c.GetSummary();
    EXPECT(v.maxLagPosts == 2 && v.interval.maxLagPosts == 2, "max lag posts %d interval %d", v.maxLagPosts, v.interval.maxLagPosts);
    EXPECT(v.maxLagTicks == 2100 && v.interval.maxLagTicks == 2100, "max lag ticks %lld interval %lld", (long long) v.maxLagTicks,
           (long long) v.interval.maxLagTicks);
    EXPECT(v.lateOutputs == 2 && v.interval.lateOutputs == 2, "two late outputs");
}

// The merger's release boundary, the late rule and the released-up-to value.
void MergerEdges()
{
    auto event = [](EventType t, uint64_t qpc)
    {
        Event e;
        e.type = t;
        e.qpc = qpc;
        return e;
    };
    // an event exactly `holdBack` ticks old is released
    {
        EventMerger m(100);
        std::vector<Event> out;
        m.Push(event(EventType::Output, 1000));
        m.Release(1099, out);
        EXPECT(out.empty() && m.Pending() == 1, "99 ticks old is held back");
        m.Release(1100, out);
        EXPECT(out.size() == 1 && m.Pending() == 0, "100 ticks old is released");
    }
    // an event equal to the last released QPC is not late
    {
        EventMerger m(0);
        std::vector<Event> out;
        m.Push(event(EventType::Output, 1000));
        m.Release(1000, out);
        m.Push(event(EventType::Post, 1000));
        EXPECT(m.Late() == 0, "equal to the released value is not late: %llu", (unsigned long long) m.Late());
        m.Push(event(EventType::Post, 999));
        EXPECT(m.Late() == 1, "one below is late");
    }
    // a late event does not lower the released-up-to value
    {
        EventMerger m(0);
        std::vector<Event> out;
        m.Push(event(EventType::Output, 100));
        m.Release(100, out);
        m.Push(event(EventType::Output, 50));
        EXPECT(m.Late() == 1, "50 is below 100");
        m.Release(200, out);
        EXPECT(out.size() == 2 && out[1].qpc == 50, "the late event is handed out with the next batch");
        m.Push(event(EventType::Output, 70));
        EXPECT(m.Late() == 2, "70 is still below the 100 released first: %llu", (unsigned long long) m.Late());
    }
}
} // namespace

int main()
{
    SettleLiterals();
    MultiplierDecrease();
    MultiplierOne();
    FrameJumpLiterals();
    LagFifteen();
    IntervalEdges();
    MergerEdges();
    OptionsPhase();
    TicksSincePost();
    IntervalLag();
    SteadyStreams();
    DroppedOutput();
    BunchedPresents();
    MultiplierChange();
    Restart();
    FrameNumberJump();
    ThreadChange();
    UnknownMultiplier();
    OutputsBeforeFirstPostAndAhead();
    MergerOrdering();
    MergerHoldBack();
    BatchAssignment();
    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: output accounting (2x/3x/4x streams, lagging pacer, bunched, dropped, multiplier change, restart, jump, threads, ordering)");
    return 0;
}
