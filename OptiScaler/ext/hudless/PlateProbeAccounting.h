#pragma once
// Developer probe of the frame generator's output stream (docs/HUDLESS.md, "Developer probes"). This header is the pure part:
// it turns raw timestamped events into batch accounting. No I/O, no clock, no threads, no D3D.
//
// Three kinds of event come from different threads:
//   Post     once per real frame, on the game thread, with the game's frame number
//   Options  once per real frame, on the same thread right after the Post (the expected number of outputs per real frame is
//            only known when the frame generator is dispatched, which is after the Post of the same frame)
//   Output   once per output present, on the pacer thread(s)
// EventMerger orders them by QPC (at flush time, on one thread). Accounting then reads them in that order and keeps two views
// of the stream, because one of them can never fail:
//   window   the outputs that arrived between one Post and the next. Its size is measured from the stream, so a dropped or a
//            bunched output shows up as a window whose size differs from the multiplier.
//   batch    first-in-first-out: output k belongs to the real frame whose multiplier-sized slot contains k. Only used to
//            measure how far the outputs trail the game (lag), never to decide whether a count is right.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace ext::hudless::plate
{
// The numeric value is the tie order at equal QPC: a Reset first, then the Post, then the Options that belong to it, then outputs.
enum class EventType : uint8_t
{
    Reset = 0,
    Post = 1,
    Options = 2,
    Output = 3,
};

struct Event
{
    EventType type = EventType::Output;
    uint64_t qpc = 0;
    uint32_t thread = 0;
    uint64_t seq = 0;          // Post: ordinal of the real frame (the probe's own counter). Options: ordinal of the Post it belongs to
    uint64_t frame = 0;        // Post: the game's frame number (it can jump; the probe's own counter never does)
    uint32_t multiplier = 0;   // Options: expected outputs per real frame (0 = unknown)
    int32_t backBuffer = -1;   // Output: swap-chain back buffer index (-1 = not available)
};

// A window or a batch is not judged for this many windows after the stream starts, after a reset and after the multiplier
// changed: with a pacer that trails by up to two batches the old multiplier is still in the stream for that long.
inline constexpr uint32_t kSettleWindows = 3;
// A real-frame number further than this ahead of the last one (or any step back) means the stream restarted.
inline constexpr uint64_t kMaxFrameJump = 8;
inline constexpr size_t kKeepPosts = 16;

struct OutputRecord
{
    uint64_t qpc = 0;
    uint32_t thread = 0;
    int32_t backBuffer = -1;
    uint64_t window = 0;         // ordinal of the Post that opened the window the output arrived in (0 = before the first Post)
    uint64_t batch = 0;          // ordinal of the real frame it is assigned to by order (0 = none)
    uint32_t indexInBatch = 0;   // 1-based position in that batch
    uint32_t multiplier = 0;     // expected outputs of that batch (0 = unknown)
    int32_t lagPosts = -1;       // Posts that happened between that batch's Post and this output (-1 = no batch)
    int64_t lagTicks = -1;       // ticks from that batch's Post to this output (-1 = no batch)
    int64_t sinceLastTicks = 0;  // ticks from the previous output (0 for the first)
    int64_t ticksSincePost = -1; // ticks from the Post that opened the window the output arrived in (-1 = before the first Post)
};

struct Summary
{
    uint64_t outputs = 0;
    uint64_t posts = 0;
    uint64_t resets = 0;
    uint64_t optionsEvents = 0;
    uint64_t orphanOptions = 0;      // an Options event whose Post is not (or no longer) there
    uint64_t windows = 0;            // closed windows
    uint64_t windowsKnown = 0;       // closed, multiplier known and settled: the denominator of the percentage
    uint64_t windowsEqual = 0;       // of those, the output count equals the multiplier
    uint64_t windowsTransition = 0;  // closed, inside the settle after a start, a reset or a multiplier change
    uint64_t windowsUnknown = 0;     // closed, multiplier unknown
    std::map<uint32_t, uint64_t> countHistogram; // outputs in a closed window -> windows
    std::map<uint32_t, uint64_t> threads;        // thread id -> outputs
    std::map<int32_t, uint64_t> firstLag;        // lag in Posts of the first output of a batch -> batches
    int32_t maxLagPosts = 0;
    int64_t maxLagTicks = 0;
    uint64_t lateOutputs = 0;        // outputs that arrived after the next Post of their batch (lag of at least one Post)
    uint64_t outputsBeforePost = 0;  // arrived with no Post known (start of the stream or right after a reset)
    uint64_t outputsAhead = 0;       // more outputs than the multipliers of all known batches asked for

    // Is the multiplier noted for the right frame? The Options event is tagged with the latest Post when it is pushed; that is only right if
    // the frame generator is dispatched after the Post of the same frame, on the same thread. If it is dispatched before it, the multiplier
    // of frame N+1 lands on N, and with a constant multiplier nothing else shows it. Two measures: the thread, and how long after its Post the
    // Options event came (a few microseconds when it follows the Post, about one Post interval when it is a frame late).
    uint64_t optionsMatched = 0;       // Options events whose Post was found
    uint64_t optionsSameThread = 0;    // of those, pushed from the thread that pushed the Post
    uint64_t optionsDelayTicks = 0;    // sum of the ticks from the Post to its Options event
    int64_t optionsDelayMaxTicks = 0;
    uint64_t postIntervals = 0;        // gaps between consecutive Posts of one stream
    uint64_t postIntervalTicks = 0;    // their sum

    // The same lag values as above for the time since the last summary line (the cumulative ones keep the start-up glitch for good).
    struct Interval
    {
        int32_t maxLagPosts = 0;
        int64_t maxLagTicks = 0;
        uint64_t lateOutputs = 0;
        std::map<int32_t, uint64_t> firstLag;
    };
    Interval interval;

    // Share of judged windows whose size equals the multiplier, in percent (0 when none was judged).
    double Percent() const { return windowsKnown ? 100.0 * double(windowsEqual) / double(windowsKnown) : 0.0; }
};

class Accounting
{
  public:
    Accounting() { _settle = kSettleWindows; }

    // Dispatch of one event in QPC order. Fills `record` (and returns true) for an Output.
    bool Process(const Event& e, OutputRecord& record)
    {
        switch (e.type)
        {
        case EventType::Reset:
            Reset();
            return false;
        case EventType::Post:
            OnPost(e.seq, e.frame, e.qpc, e.thread);
            return false;
        case EventType::Options:
            OnMultiplier(e.seq, e.multiplier, e.thread, e.qpc);
            return false;
        case EventType::Output:
            record = OnOutput(e.qpc, e.thread, e.backBuffer);
            return true;
        }
        return false;
    }

    void OnPost(uint64_t seq, uint64_t frame, uint64_t qpc, uint32_t thread = 0)
    {
        if (!_posts.empty())
        {
            const uint64_t previous = _posts.back().frame;
            if (frame < previous || frame > previous + kMaxFrameJump)
                Reset();
        }
        if (!_posts.empty())
        {
            if (qpc > _posts.back().qpc)
            {
                ++_s.postIntervals;
                _s.postIntervalTicks += qpc - _posts.back().qpc;
            }
            CloseWindow(_posts.back());
        }

        PostRec post;
        post.seq = seq;
        post.frame = frame;
        post.qpc = qpc;
        post.thread = thread;
        _posts.push_back(post);
        ++_s.posts;
        while (_posts.size() > kKeepPosts)
        {
            _posts.pop_front();
            ++_base;
        }
    }

    // `thread` and `qpc` are those of the Options event; the Post it belongs to is found by its ordinal.
    void OnMultiplier(uint64_t seq, uint32_t multiplier, uint32_t thread = 0, uint64_t qpc = 0)
    {
        ++_s.optionsEvents;
        for (auto it = _posts.rbegin(); it != _posts.rend(); ++it)
            if (it->seq == seq)
            {
                it->multiplier = multiplier;
                ++_s.optionsMatched;
                if (it->thread == thread)
                    ++_s.optionsSameThread;
                const int64_t delay = qpc > it->qpc ? static_cast<int64_t>(qpc - it->qpc) : 0;
                _s.optionsDelayTicks += static_cast<uint64_t>(delay);
                _s.optionsDelayMaxTicks = std::max(_s.optionsDelayMaxTicks, delay);
                return;
            }
        ++_s.orphanOptions;
    }

    OutputRecord OnOutput(uint64_t qpc, uint32_t thread, int32_t backBuffer)
    {
        OutputRecord r;
        r.qpc = qpc;
        r.thread = thread;
        r.backBuffer = backBuffer;
        ++_s.outputs;
        ++_s.threads[thread];
        if (_haveOutput)
            r.sinceLastTicks = qpc > _lastOutput ? static_cast<int64_t>(qpc - _lastOutput) : 0;
        _haveOutput = true;
        _lastOutput = qpc;

        if (_posts.empty())
        {
            ++_s.outputsBeforePost;
            return r;
        }
        PostRec& open = _posts.back();
        r.window = open.seq;
        r.ticksSincePost = qpc > open.qpc ? static_cast<int64_t>(qpc - open.qpc) : 0;
        ++open.count;

        // first-in-first-out assignment to the real frame whose slot holds this output
        for (;;)
        {
            if (_fifo < _base)
                _fifo = _base;
            const size_t index = static_cast<size_t>(_fifo - _base);
            if (index >= _posts.size())
            {
                ++_s.outputsAhead;
                return r;
            }
            PostRec& batch = _posts[index];
            const bool isOpen = index + 1 == _posts.size();
            if (batch.multiplier != 0 && batch.assigned >= batch.multiplier)
            {
                ++_fifo; // full
                continue;
            }
            if (batch.multiplier == 0 && !isOpen)
            {
                ++_fifo; // no multiplier and a newer frame exists: nothing more will be assigned to it
                continue;
            }
            ++batch.assigned;
            r.batch = batch.seq;
            r.indexInBatch = batch.assigned;
            r.multiplier = batch.multiplier;
            r.lagPosts = static_cast<int32_t>(_posts.size() - 1 - index);
            r.lagTicks = qpc > batch.qpc ? static_cast<int64_t>(qpc - batch.qpc) : 0;
            break;
        }

        _s.maxLagPosts = std::max(_s.maxLagPosts, r.lagPosts);
        _s.maxLagTicks = std::max(_s.maxLagTicks, r.lagTicks);
        _s.interval.maxLagPosts = std::max(_s.interval.maxLagPosts, r.lagPosts);
        _s.interval.maxLagTicks = std::max(_s.interval.maxLagTicks, r.lagTicks);
        if (r.lagPosts >= 1)
        {
            ++_s.lateOutputs;
            ++_s.interval.lateOutputs;
        }
        if (r.indexInBatch == 1)
        {
            ++_s.firstLag[r.lagPosts];
            ++_s.interval.firstLag[r.lagPosts];
        }
        return r;
    }

    // Called by the writer right after it printed a summary line: the next line's "this interval" values start from zero. The cumulative
    // values are not touched.
    void ResetInterval() { _s.interval = Summary::Interval(); }

    // Playback restarted (DLSS-G switched off and on, a jump in the frame numbers): forget the open window and the batches.
    void Reset()
    {
        _posts.clear();
        _base = 0;
        _fifo = 0;
        _lastMultiplier = 0;
        _settle = kSettleWindows;
        ++_s.resets;
    }

    const Summary& GetSummary() const { return _s; }

  private:
    struct PostRec
    {
        uint64_t seq = 0;
        uint64_t frame = 0;
        uint64_t qpc = 0;
        uint32_t thread = 0;
        uint32_t multiplier = 0;
        uint32_t count = 0;    // outputs that arrived in the window of this Post
        uint32_t assigned = 0; // outputs the first-in-first-out assignment gave to this real frame
    };

    void CloseWindow(const PostRec& p)
    {
        ++_s.windows;
        ++_s.countHistogram[p.count];
        if (p.multiplier == 0)
        {
            ++_s.windowsUnknown;
            return;
        }
        if (_lastMultiplier != 0 && p.multiplier != _lastMultiplier)
            _settle = kSettleWindows;
        _lastMultiplier = p.multiplier;
        if (_settle > 0)
        {
            --_settle;
            ++_s.windowsTransition;
            return;
        }
        ++_s.windowsKnown;
        if (p.count == p.multiplier)
            ++_s.windowsEqual;
    }

    Summary _s;
    std::deque<PostRec> _posts; // oldest first; the last one is the open window
    uint64_t _base = 0;         // absolute index of _posts.front()
    uint64_t _fifo = 0;         // absolute index of the batch the next output is assigned to
    uint32_t _lastMultiplier = 0;
    uint32_t _settle = 0;
    bool _haveOutput = false;
    uint64_t _lastOutput = 0;
};

// Puts the raw events of several threads in QPC order. A producer reads the clock and then pushes, so an event can reach the
// ring after a flush already looked at it: Release() therefore only hands out events older than `holdBackTicks` before `now`
// and keeps the newer ones for the next flush. An event that still arrives below what was already released is counted
// (`late`) and handed out with the next batch.
class EventMerger
{
  public:
    explicit EventMerger(uint64_t holdBackTicks) : _holdBack(holdBackTicks) {}

    void Push(const Event& e)
    {
        if (_havePushed && e.qpc < _lastPushed)
            ++_reordered;
        _havePushed = true;
        _lastPushed = e.qpc;
        if (_haveReleased && e.qpc < _releasedUpTo)
            ++_late;
        _pending.push_back(e);
    }

    // Events with qpc <= now - holdBack, in (qpc, type) order.
    void Release(uint64_t nowQpc, std::vector<Event>& out)
    {
        const uint64_t cutoff = nowQpc > _holdBack ? nowQpc - _holdBack : 0;
        ReleaseUpTo(cutoff, out);
    }

    void ReleaseAll(std::vector<Event>& out) { ReleaseUpTo(UINT64_MAX, out); }

    uint64_t Late() const { return _late; }            // arrived after a newer event was already released
    uint64_t Reordered() const { return _reordered; }  // pushed out of QPC order (a race between producers)
    size_t Pending() const { return _pending.size(); }

  private:
    void ReleaseUpTo(uint64_t cutoff, std::vector<Event>& out)
    {
        auto mid = std::stable_partition(_pending.begin(), _pending.end(), [&](const Event& e) { return e.qpc <= cutoff; });
        std::vector<Event> ready(_pending.begin(), mid);
        _pending.erase(_pending.begin(), mid);
        std::stable_sort(ready.begin(), ready.end(),
                         [](const Event& a, const Event& b)
                         { return a.qpc != b.qpc ? a.qpc < b.qpc : static_cast<uint8_t>(a.type) < static_cast<uint8_t>(b.type); });
        for (const Event& e : ready)
        {
            _haveReleased = true;
            _releasedUpTo = std::max(_releasedUpTo, e.qpc);
            out.push_back(e);
        }
    }

    uint64_t _holdBack;
    std::vector<Event> _pending;
    uint64_t _releasedUpTo = 0;
    uint64_t _lastPushed = 0;
    uint64_t _late = 0;
    uint64_t _reordered = 0;
    bool _haveReleased = false;
    bool _havePushed = false;
};
} // namespace ext::hudless::plate
