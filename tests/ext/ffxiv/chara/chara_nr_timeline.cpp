// A two-queue timeline model of the preview NR hand-off (D3D11 queue, D3D12 queue, fences with "last written value" semantics).
//
// The production templates RunFrame / RunHandoff / ConsumerWait (CharaNrRules.h) drive a recording Ops that appends the operations
// to the two queues exactly as the module would queue them. The model then executes the queues under many schedules (one queue
// always first, the other always first, alternating, and a few hundred random interleavings). A GPU queue signals by WRITING a value
// (it does not take the maximum), a wait is satisfied when the fence holds at least its value, and a queue runs its operations in
// order. For every schedule the test checks:
//   * the queues drain (no wait is left that no signal will ever satisfy: the shared bridge queue would hang the game),
//   * a fence value is never lowered by a signal,
//   * a wait is never satisfied by anything but the signal of the value it waits for,
//   * every data dependency holds in the order the operations really ran (colour copy before the run that reads it, the run before the
//     consumer that samples its result, the consumer before the run that overwrites the same result image, the run before the copy that
//     overwrites the colour image it read).
// The single-shared-fence plan of the first version is reproduced as a negative control: the same model must find its defects in the late
// variant (and must find none in the default variant, which was correct).
#include "check.h"

#include "ext/ffxiv/chara/CharaNrRules.h"

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace ext::ffxiv::chara::nr;

namespace
{
constexpr int kD3D11 = 0;
constexpr int kD3D12 = 1;
constexpr int kReady = 0;
constexpr int kDone = 1; // in the single-fence plan both map to fence 0

struct Op
{
    enum Kind { Signal, Wait, Work } kind;
    int fence;
    uint64_t value;
    int event; // Work only
};

struct Outcome
{
    bool drained = false;
    int fenceDecreases = 0;
    int satisfiedByOther = 0; // a wait that ran although the signal of its value had not run
    std::vector<int> order;   // events in the order they ran
};

class Timeline
{
  public:
    int AddEvent() { return _events++; }
    void Push(int queue, Op op) { _queues[queue].push_back(op); }

    // policy 0: D3D11 first, 1: D3D12 first, 2: alternate, 3: random with `seed`
    Outcome Execute(int policy, uint32_t seed) const
    {
        Outcome out;
        size_t head[2] = { 0, 0 };
        uint64_t fence[2] = { 0, 0 };
        std::map<std::pair<int, uint64_t>, bool> signalled;
        std::mt19937 rng(seed);
        int turn = 0;
        for (;;)
        {
            bool runnable[2] = { false, false };
            for (int q = 0; q < 2; ++q)
            {
                if (head[q] >= _queues[q].size())
                    continue;
                const Op& op = _queues[q][head[q]];
                runnable[q] = op.kind != Op::Wait || fence[op.fence] >= op.value;
            }
            if (!runnable[0] && !runnable[1])
                break;
            int pick;
            if (policy == 0)
                pick = runnable[0] ? 0 : 1;
            else if (policy == 1)
                pick = runnable[1] ? 1 : 0;
            else if (policy == 2)
            {
                pick = runnable[turn] ? turn : 1 - turn;
                turn = 1 - turn;
            }
            else
                pick = runnable[0] && runnable[1] ? static_cast<int>(rng() & 1u) : (runnable[0] ? 0 : 1);

            const Op& op = _queues[pick][head[pick]++];
            switch (op.kind)
            {
            case Op::Signal:
                if (op.value < fence[op.fence])
                    ++out.fenceDecreases;
                fence[op.fence] = op.value; // a signal writes the value
                signalled[{ op.fence, op.value }] = true;
                break;
            case Op::Wait:
                if (!signalled[{ op.fence, op.value }])
                    ++out.satisfiedByOther;
                break;
            case Op::Work: out.order.push_back(op.event); break;
            }
        }
        out.drained = head[0] == _queues[0].size() && head[1] == _queues[1].size();
        return out;
    }

    const std::vector<Op>& Queue(int q) const { return _queues[q]; }

  private:
    std::vector<Op> _queues[2];
    int _events = 0;
};

// The kind of a data dependency (the pre-SR scenario tags its edges so a negative control can show WHICH edge a defect broke).
enum DepKind
{
    kCopyBeforeRun = 0,       // the run reads the copy: the copy must be done before it (also the kind of every edge a scenario does not tag)
    kRunBeforeEvaluate = 1,   // the SR evaluate reads a finished result
    kRunBeforeNextCopy = 2,   // the next copies overwrite the one colour, depth and motion image: the run that reads them is finished first
    kEvaluateBeforeReuse = 3, // run k+2 overwrites the result image the evaluate of run k read
    kDepKinds = 4,
};

struct Dependency
{
    int before;
    int after;
    int kind = kCopyBeforeRun;
};

struct Stream
{
    bool late = false;
    uint64_t doneOfBuffer[2] = { 0, 0 };
    std::vector<int> copy, run, read; // event ids per run index (read: per consumed run)
    std::vector<uint64_t> ready;      // the ready fence value of each run (for the per-stream order checks)
    std::vector<uint64_t> done;       // the done fence value of each run
};

struct Scenario
{
    Timeline timeline;
    std::vector<Dependency> deps;
};

// What the module queues for one stream's run (the production templates drive this).
struct RecordingOps
{
    Timeline& t;
    FenceCounter& done;
    int copyEvent;
    int runEvent;
    bool WaitColourFree(uint64_t v)
    {
        t.Push(kD3D11, { Op::Wait, kDone, v, 0 });
        return true;
    }
    void CopyColour() { t.Push(kD3D11, { Op::Work, 0, 0, copyEvent }); }
    bool Signal11(uint64_t v)
    {
        t.Push(kD3D11, { Op::Signal, kReady, v, 0 });
        return true;
    }
    bool Wait12(uint64_t v)
    {
        t.Push(kD3D12, { Op::Wait, kReady, v, 0 });
        return true;
    }
    bool Record(uint64_t& d)
    {
        t.Push(kD3D12, { Op::Work, 0, 0, runEvent });
        d = done.Next();
        t.Push(kD3D12, { Op::Signal, kDone, d, 0 });
        return true;
    }
    bool Wait11(uint64_t d)
    {
        t.Push(kD3D11, { Op::Wait, kDone, d, 0 });
        return true;
    }
};

struct ConsumerRecording
{
    Timeline& t;
    bool WaitResult(uint64_t v)
    {
        t.Push(kD3D11, { Op::Wait, kDone, v, 0 });
        return true;
    }
};

void AddDeps(Scenario& s, const Stream& st, int frames)
{
    for (int k = 0; k < frames; ++k)
    {
        s.deps.push_back({ st.copy[k], st.run[k] }); // the run reads the colour copy
        const int reuse = st.late ? k + 2 : k + 1;   // colour images: two in the late variant, one otherwise
        if (reuse < frames)
            s.deps.push_back({ st.run[k], st.copy[reuse] }); // the run has read the colour image before the next copy overwrites it
        if (st.read[k] >= 0)
            s.deps.push_back({ st.run[k], st.read[k] }); // the consumer samples a finished result
    }
}

// The plan of this version: two fences, the production templates. `lates` lists the streams (late or not); `frames` frames.
Scenario BuildNew(const std::vector<bool>& lates, int frames, uint32_t consumerFrames = ~0u)
{
    Scenario s;
    FenceCounter ready, done;
    std::vector<Stream> streams(lates.size());
    for (size_t i = 0; i < streams.size(); ++i)
        streams[i].late = lates[i];
    // read[k] is filled per consumed run below; allocate the events first
    for (int k = 0; k < frames; ++k)
    {
        for (Stream& st : streams)
        {
            st.copy.push_back(s.timeline.AddEvent());
            st.run.push_back(s.timeline.AddEvent());
            st.read.push_back(-1);
            RecordingOps ops { s.timeline, done, st.copy.back(), st.run.back() };
            const HandoffOutcome o = RunFrame(ops, ready, st.late, st.doneOfBuffer[k & 1]);
            CHECK(o.ok);
            st.doneOfBuffer[k & 1] = o.done;
        }
        for (Stream& st : streams)
        {
            if ((consumerFrames >> k & 1u) == 0)
                continue; // no draw samples the preview in this frame: nothing else orders the D3D11 queue behind the runs
            // the consumer draw of this frame: which result and which wait come from the production functions
            const int buffer = ConsumerBuffer(st.late, static_cast<uint64_t>(k) + 1, static_cast<uint64_t>(k) + 1);
            if (buffer < 0)
                continue;
            const int consumed = (buffer == (k & 1)) ? k : k - 1;
            ConsumerRecording cons { s.timeline };
            CHECK(ConsumerWait(cons, st.late, st.doneOfBuffer[buffer]));
            const int readEvent = s.timeline.AddEvent();
            s.timeline.Push(kD3D11, { Op::Work, 0, 0, readEvent });
            st.read[consumed] = readEvent;
        }
    }
    for (const Stream& st : streams)
        AddDeps(s, st, frames);
    // the result image written by run j+2 was read by the consumer of run j
    for (const Stream& st : streams)
        for (int k = 0; k + 2 < frames; ++k)
            if (st.read[k] >= 0)
                s.deps.push_back({ st.read[k], st.run[k + 2] });
    return s;
}

// The first version, reproduced from the review: one fence signalled from both queues, one colour image, no wait before the copy.
Scenario BuildOld(const std::vector<bool>& lates, int frames, uint32_t consumerFrames = ~0u)
{
    Scenario s;
    uint64_t counter = 0; // one counter for ready and done values: 1, 2, 3, ...
    std::vector<Stream> streams(lates.size());
    for (size_t i = 0; i < streams.size(); ++i)
        streams[i].late = lates[i];
    constexpr int kShared = 0;
    for (int k = 0; k < frames; ++k)
    {
        for (Stream& st : streams)
        {
            st.copy.push_back(s.timeline.AddEvent());
            st.run.push_back(s.timeline.AddEvent());
            st.read.push_back(-1);
            s.timeline.Push(kD3D11, { Op::Work, 0, 0, st.copy.back() });
            const uint64_t readyValue = ++counter;
            s.timeline.Push(kD3D11, { Op::Signal, kShared, readyValue, 0 });
            s.timeline.Push(kD3D12, { Op::Wait, kShared, readyValue, 0 });
            s.timeline.Push(kD3D12, { Op::Work, 0, 0, st.run.back() });
            const uint64_t doneValue = ++counter;
            s.timeline.Push(kD3D12, { Op::Signal, kShared, doneValue, 0 });
            if (!st.late)
                s.timeline.Push(kD3D11, { Op::Wait, kShared, doneValue, 0 });
            st.doneOfBuffer[k & 1] = doneValue;
        }
        for (Stream& st : streams)
        {
            if ((consumerFrames >> k & 1u) == 0)
                continue;
            const int buffer = ConsumerBuffer(st.late, static_cast<uint64_t>(k) + 1, static_cast<uint64_t>(k) + 1);
            if (buffer < 0)
                continue;
            const int consumed = (buffer == (k & 1)) ? k : k - 1;
            if (st.late)
                s.timeline.Push(kD3D11, { Op::Wait, kShared, st.doneOfBuffer[buffer], 0 }); // the wait on the shared fence
            const int readEvent = s.timeline.AddEvent();
            s.timeline.Push(kD3D11, { Op::Work, 0, 0, readEvent });
            st.read[consumed] = readEvent;
        }
    }
    for (Stream& st : streams)
    {
        // one colour image: the next copy waits for this run in every variant
        st.late = false;
        AddDeps(s, st, frames);
    }
    for (const Stream& st : streams)
        for (int k = 0; k + 2 < frames; ++k)
            if (st.read[k] >= 0)
                s.deps.push_back({ st.read[k], st.run[k + 2] });
    return s;
}

// NR before SR: what the module queues for one pre-SR run (the copies of CopyPlanFor, then the hand-off), followed by the caller's next D3D11
// command, the SR evaluate, which reads the result image. The production templates drive it exactly as for the other placements.
//
// What this model can and cannot show. The module's real work on the D3D11 side is a list of commands in queue order (copies, the ready signal, the
// wait, then the caller's evaluate): the evaluate is appended to the same queue right after the hand-off, so "the evaluate runs after the wait" and
// "the next copies run after the evaluate of the previous run" are TRUE BY CONSTRUCTION of the queue order and cannot fail for a defect of the
// module's logic. What the model does test is the shared RunFrame/RunHandoff order, the copy plan and the D3D12 side's waits (fence values, one signal
// per value). Whether the NGX evaluate really honours the queued wait on its own queue, and the D3D12 resource states, are proven only in game.
// The negative controls below break each edge once, so each edge is seen to fire when its order is wrong.
enum class PreSrFault
{
    None,
    NoInlineWait,        // the evaluate is issued without the D3D11 wait on the run (what the late-variant hand-off would do)
    LastCopyAfterSignal, // the motion copy is queued after the ready signal: the run may start before it
    DeferredEvaluate,    // the evaluate of run k is queued only after run k+2's hand-off: run k+2 has overwritten the result it reads
};

struct PreSrOps
{
    Timeline& t;
    FenceCounter& done;
    std::vector<int> copyEvents;
    int runEvent;
    bool lastCopyAfterSignal = false;
    bool WaitColourFree(uint64_t v)
    {
        t.Push(kD3D11, { Op::Wait, kDone, v, 0 });
        return true;
    }
    void CopyColour()
    {
        const CopyPlan plan = CopyPlanFor(Placement::PreSr);
        for (size_t i = 0; i + (lastCopyAfterSignal ? 1 : 0) < plan.count; ++i)
            t.Push(kD3D11, { Op::Work, 0, 0, copyEvents[i] }); // colour, depth, motion, each before the ready signal below
    }
    bool Signal11(uint64_t v)
    {
        t.Push(kD3D11, { Op::Signal, kReady, v, 0 });
        if (lastCopyAfterSignal)
            t.Push(kD3D11, { Op::Work, 0, 0, copyEvents.back() }); // the defect: the last copy is not part of what the signal covers
        return true;
    }
    bool Wait12(uint64_t v)
    {
        t.Push(kD3D12, { Op::Wait, kReady, v, 0 });
        return true;
    }
    bool Record(uint64_t& d)
    {
        t.Push(kD3D12, { Op::Work, 0, 0, runEvent });
        d = done.Next();
        t.Push(kD3D12, { Op::Signal, kDone, d, 0 });
        return true;
    }
    bool Wait11(uint64_t d)
    {
        t.Push(kD3D11, { Op::Wait, kDone, d, 0 });
        return true;
    }
};

// `fault` selects the defect of a negative control (see PreSrFault); None is the real order.
Scenario BuildPreSr(int frames, PreSrFault fault = PreSrFault::None)
{
    Scenario s;
    FenceCounter ready, done;
    // PreviewNRLate belongs to the bring-up: a pre-SR stream is never late, so its run always waits inline
    const StreamShape shape = ShapeFor(Placement::PreSr, 576, 960, 2, 0, /*lateWanted=*/true, 576, 960);
    CHECK(!LateVariantInUse(shape));
    const bool late = fault == PreSrFault::NoInlineWait ? true : LateVariantInUse(shape);
    std::vector<std::vector<int>> copies;
    std::vector<int> runs, evaluates;
    for (int k = 0; k < frames; ++k)
    {
        PreSrOps ops { s.timeline, done, {}, 0 };
        ops.lastCopyAfterSignal = fault == PreSrFault::LastCopyAfterSignal;
        for (size_t i = 0; i < CopyPlanFor(Placement::PreSr).count; ++i)
            ops.copyEvents.push_back(s.timeline.AddEvent());
        ops.runEvent = s.timeline.AddEvent();
        const HandoffOutcome o = RunFrame(ops, ready, late, 0);
        CHECK(o.ok);
        evaluates.push_back(s.timeline.AddEvent());
        copies.push_back(ops.copyEvents);
        runs.push_back(ops.runEvent);
        // the caller's next D3D11 command: the SR evaluate reading the result (the defect queues it two runs late)
        const int due = fault == PreSrFault::DeferredEvaluate ? k - 2 : k;
        if (due >= 0)
            s.timeline.Push(kD3D11, { Op::Work, 0, 0, evaluates[static_cast<size_t>(due)] });
    }
    if (fault == PreSrFault::DeferredEvaluate)
        for (int k = frames > 2 ? frames - 2 : 0; k < frames; ++k)
            s.timeline.Push(kD3D11, { Op::Work, 0, 0, evaluates[static_cast<size_t>(k)] });
    CHECK(copies.empty() || copies[0].size() == 3);
    for (int k = 0; k < frames; ++k)
    {
        for (const int copy : copies[k])
            s.deps.push_back({ copy, runs[k], kCopyBeforeRun }); // the run reads all three copies
        s.deps.push_back({ runs[k], evaluates[k], kRunBeforeEvaluate }); // the evaluate reads a finished result
        if (k + 1 < frames)
            for (const int copy : copies[k + 1])
                s.deps.push_back({ runs[k], copy, kRunBeforeNextCopy }); // one colour, depth and motion image: the next copies wait for the run that read them
        if (k + 2 < frames)
            s.deps.push_back({ evaluates[k], runs[k + 2], kEvaluateBeforeReuse }); // two result images: run k+2 overwrites the one the evaluate of run k read
    }
    return s;
}

struct Verdict
{
    int drainFailures = 0;
    int decreases = 0;
    int satisfiedByOther = 0;
    int hazards = 0;
    int hazardsByKind[kDepKinds] = {}; // hazards by DepKind (the scenarios that do not tag their edges count everything under kind 0)
    int schedules = 0;
};

Verdict Check(const Scenario& s)
{
    Verdict v;
    auto account = [&](const Outcome& o)
    {
        ++v.schedules;
        if (!o.drained)
            ++v.drainFailures;
        v.decreases += o.fenceDecreases;
        v.satisfiedByOther += o.satisfiedByOther;
        std::map<int, size_t> position;
        for (size_t i = 0; i < o.order.size(); ++i)
            position[o.order[i]] = i;
        for (const Dependency& d : s.deps)
        {
            const auto b = position.find(d.before);
            const auto a = position.find(d.after);
            if (b == position.end() || a == position.end())
                continue; // the schedule did not drain: counted above
            if (b->second > a->second)
            {
                ++v.hazards;
                ++v.hazardsByKind[d.kind];
            }
        }
    };
    for (int policy = 0; policy < 3; ++policy)
        account(s.timeline.Execute(policy, 0));
    for (uint32_t seed = 1; seed <= 400; ++seed)
        account(s.timeline.Execute(3, seed));
    return v;
}

const std::vector<std::vector<bool>> kStreamSets = { { true }, { false }, { true, false }, { true, true }, { false, false }, { false, true, true } };

// Frames in which a draw samples the preview: every frame, none, every other frame, only the first ones. The colour image the late variant
// overwrites two runs after its last reader is protected by its own wait, not by the consumer waits (which may not exist in a frame).
const uint32_t kConsumerPatterns[] = { ~0u, 0u, 0x55u, 0xAAu, 0x3u, 0x80u };

void TestNewPlanIsSound()
{
    for (const auto& lates : kStreamSets)
    {
        for (const uint32_t pattern : kConsumerPatterns)
        {
            const Verdict v = Check(BuildNew(lates, 8, pattern));
            CHECK(v.schedules == 403);
            CHECK(v.drainFailures == 0);      // every wait gets its signal: nothing is left blocking the shared queue
            CHECK(v.decreases == 0);          // fence values never go down
            CHECK(v.satisfiedByOther == 0);   // a wait is only ever satisfied by the signal of its own value
            CHECK(v.hazards == 0);            // every data dependency holds in every schedule
        }
    }
}

void TestStructure()
{
    // each fence has one signaller: ready only the D3D11 context, done only the D3D12 queue; waits are on the other side
    for (const auto& lates : kStreamSets)
    {
        const Scenario s = BuildNew(lates, 8, 0x5Fu);
        for (int q = 0; q < 2; ++q)
        {
            uint64_t lastSignal[2] = { 0, 0 };
            for (const Op& op : s.timeline.Queue(q))
            {
                if (op.kind == Op::Signal)
                {
                    CHECK(op.fence == (q == kD3D11 ? kReady : kDone)); // D3D11 signals ready, D3D12 signals done
                    CHECK(op.value > lastSignal[op.fence]);            // strictly growing within the queue
                    lastSignal[op.fence] = op.value;
                }
                if (op.kind == Op::Wait)
                    CHECK(op.fence == (q == kD3D11 ? kDone : kReady)); // D3D11 waits on done, D3D12 on ready
            }
        }
    }
}

void TestOldPlanShowsTheDefects()
{
    // the default variant of the first version was correct: the model must agree
    for (const auto& lates : std::vector<std::vector<bool>> { { false }, { false, false } })
    {
        const Verdict v = Check(BuildOld(lates, 8));
        CHECK(v.drainFailures == 0 && v.hazards == 0 && v.decreases == 0 && v.satisfiedByOther == 0);
    }
    // the late variant of the first version was not: the consumer wait on the shared fence is satisfied early, the fence goes down,
    // the colour image is overwritten while the previous run reads it, and a wait can be left blocking the queue
    const Verdict late = Check(BuildOld({ true }, 8));
    CHECK(late.satisfiedByOther > 0);
    CHECK(late.decreases > 0);
    CHECK(late.hazards > 0);
    const Verdict two = Check(BuildOld({ true, true }, 8));
    CHECK(two.hazards > 0 && two.decreases > 0);
}

// NR before SR: three copies (colour, depth, motion) before the ready signal, and the SR evaluate that follows the run on the D3D11 queue is ordered
// after the run in every schedule; the result image a later run overwrites has been read by then.
void TestPreSrHandoff()
{
    for (const int frames : { 1, 2, 3, 8 })
    {
        const Verdict v = Check(BuildPreSr(frames));
        CHECK(v.schedules == 403);
        CHECK(v.drainFailures == 0 && v.decreases == 0 && v.satisfiedByOther == 0 && v.hazards == 0);
    }
    // each frame queues the three copies and then the ready signal on the D3D11 queue, in this order, and only then the wait on the run
    const Scenario s = BuildPreSr(2);
    const std::vector<Op>& q11 = s.timeline.Queue(kD3D11);
    size_t copiesBeforeSignal = 0;
    bool signalled = false, waitedAfterSignal = false, evaluateAfterWait = false;
    for (const Op& op : q11)
    {
        if (op.kind == Op::Work && !signalled)
            ++copiesBeforeSignal;
        else if (op.kind == Op::Signal)
            signalled = true;
        else if (op.kind == Op::Wait && signalled)
            waitedAfterSignal = true;
        else if (op.kind == Op::Work && waitedAfterSignal)
        {
            evaluateAfterWait = true;
            break;
        }
    }
    CHECK(copiesBeforeSignal == 3 && signalled && waitedAfterSignal && evaluateAfterWait);
    // Negative controls: each edge of the hazard model is seen to fire when its order is broken (the model has to say so, or it would pass anything).
    // "403 schedules" is 3 deterministic policies plus 400 pseudo-random interleavings of the two-queue model, whatever the frame count: a fixed
    // sample of schedules, not an enumeration and not a measurement of the GPU.
    // 1. the evaluate is issued without the D3D11 wait on the run: it reads a result that may not be finished, and the next copies overwrite the
    //    colour, depth and motion images the run may still be reading (the wait is what orders both)
    const Verdict noWait = Check(BuildPreSr(4, PreSrFault::NoInlineWait));
    CHECK(noWait.hazardsByKind[kRunBeforeEvaluate] > 0 && noWait.hazardsByKind[kRunBeforeNextCopy] > 0);
    CHECK(noWait.hazardsByKind[kCopyBeforeRun] == 0); // the copies are still before the ready signal
    // 2. one copy queued after the ready signal: the D3D12 run may start before it
    const Verdict lateCopy = Check(BuildPreSr(4, PreSrFault::LastCopyAfterSignal));
    CHECK(lateCopy.hazardsByKind[kCopyBeforeRun] > 0);
    CHECK(lateCopy.drainFailures == 0 && lateCopy.decreases == 0 && lateCopy.satisfiedByOther == 0); // only the data edge is broken
    // 3. the evaluate of a run queued two runs late: run k+2 has overwritten the result image it reads
    const Verdict lateEvaluate = Check(BuildPreSr(5, PreSrFault::DeferredEvaluate));
    CHECK(lateEvaluate.hazardsByKind[kEvaluateBeforeReuse] > 0);
    // The order of the real module is the order the model builds: the edges "evaluate after the wait", "next copies after the evaluate of the previous
    // run" and "evaluate of run k before run k+2 overwrites its result" follow from the D3D11 queue order alone (control 3 has to break the queue
    // order itself to fire). What no offline model sees: that NGX honours the queued wait for its own reads of Color, and the D3D12 resource states.
}

// The window closes after a late frame: the last result is never consumed, and the queues still drain (this is where the shared fence
// hung the bridge's queue).
void TestWindowClosesAfterLateFrame()
{
    for (int frames = 1; frames <= 6; ++frames)
    {
        const Verdict v = Check(BuildNew({ true }, frames));
        CHECK(v.drainFailures == 0 && v.hazards == 0 && v.decreases == 0 && v.satisfiedByOther == 0);
    }
    // the first version could leave the D3D12 queue blocked behind a wait that no later signal satisfies
    int blocked = 0;
    for (int frames = 2; frames <= 6; ++frames)
        blocked += Check(BuildOld({ true }, frames)).drainFailures;
    CHECK(blocked > 0);
}
// ---- several streams in one frame -----------------------------------------------------------------------------------------------
// Position of an operation in a queue (the first match), or -1.
int WorkAt(const std::vector<Op>& q, int event)
{
    for (size_t i = 0; i < q.size(); ++i)
    {
        if (q[i].kind == Op::Work && q[i].event == event)
            return static_cast<int>(i);
    }
    return -1;
}

int SignalAt(const std::vector<Op>& q, int fence, uint64_t value)
{
    for (size_t i = 0; i < q.size(); ++i)
    {
        if (q[i].kind == Op::Signal && q[i].fence == fence && q[i].value == value)
            return static_cast<int>(i);
    }
    return -1;
}

int WaitAt(const std::vector<Op>& q, int fence, uint64_t value)
{
    for (size_t i = 0; i < q.size(); ++i)
    {
        if (q[i].kind == Op::Wait && q[i].fence == fence && q[i].value == value)
            return static_cast<int>(i);
    }
    return -1;
}

// `streams` post-SR streams run in every frame. The streams are returned for the per-stream order checks.
void BuildManyStreams(Scenario& s, size_t streams, int frames, std::vector<Stream>& out)
{
    FenceCounter ready, done;
    out.assign(streams, Stream {});
    for (int k = 0; k < frames; ++k)
    {
        for (Stream& x : out)
        {
            x.copy.push_back(s.timeline.AddEvent());
            x.run.push_back(s.timeline.AddEvent());
            x.read.push_back(-1);
            RecordingOps ops { s.timeline, done, x.copy.back(), x.run.back() };
            const auto o = RunFrame(ops, ready, x.late, x.doneOfBuffer[k & 1]);
            CHECK(o.ok);
            x.doneOfBuffer[k & 1] = o.done;
            x.ready.push_back(o.ready);
            x.done.push_back(o.done);
        }
    }
    for (const Stream& x : out)
        AddDeps(s, x, frames);
}

void TestManyStreamsPerFrame()
{
    for (const size_t streams : { 1, 2, 3, 4 })
    {
        const int frames = 6;
        Scenario s;
        std::vector<Stream> st;
        BuildManyStreams(s, streams, frames, st);
        const Verdict v = Check(s);
        CHECK(v.schedules == 403);
        CHECK(v.drainFailures == 0 && v.decreases == 0 && v.satisfiedByOther == 0 && v.hazards == 0);
        const std::vector<Op>& q11 = s.timeline.Queue(kD3D11);
        const std::vector<Op>& q12 = s.timeline.Queue(kD3D12);
        // the done fence grows across streams and frames (one counter for all of them)
        uint64_t lastDone = 0;
        bool growing = true;
        for (const Op& op : q12)
        {
            if (op.kind == Op::Signal && op.fence == kDone)
            {
                growing = growing && op.value > lastDone;
                lastDone = op.value;
            }
        }
        CHECK(growing);
        size_t runs = 0;
        for (const Op& op : q12)
            runs += op.kind == Op::Work ? 1 : 0;
        CHECK(runs == streams * static_cast<size_t>(frames));
        // each stream's copy is queued before the ready signal that releases its own run
        for (const Stream& x : st)
        {
            for (int k = 0; k < frames; ++k)
            {
                const int copy = WorkAt(q11, x.copy[static_cast<size_t>(k)]);
                const int signal = SignalAt(q11, kReady, x.ready[static_cast<size_t>(k)]);
                CHECK(copy >= 0 && signal >= 0 && copy < signal);
            }
        }
    }
}

// The allocator ring of the module: four lists may be in flight, a list starts when the done value its allocator last carried is
// complete, and a fifth call with nothing complete is refused before anything is queued or any fence value is taken.
class AllocatorRing
{
  public:
    static constexpr size_t kCount = 4;
    // -1 when every allocator is still in flight
    int Begin(uint64_t completed)
    {
        for (size_t i = 0; i < kCount; ++i)
        {
            if (!_inFlight[i] && (_value[i] == 0 || _value[i] <= completed))
            {
                _inFlight[i] = true;
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    void Submitted(int index, uint64_t done)
    {
        _inFlight[static_cast<size_t>(index)] = false;
        _value[static_cast<size_t>(index)] = done;
    }

  private:
    uint64_t _value[kCount] {};
    bool _inFlight[kCount] {};
};

void TestAllocatorExhaustion()
{
    Scenario s;
    FenceCounter ready, done;
    AllocatorRing ring;
    Stream streams[5];
    // Four runs start; each allocator keeps the done value of its list.
    for (int i = 0; i < 4; ++i)
    {
        CHECK(ring.Begin(0) == i);
        streams[i].copy.push_back(s.timeline.AddEvent());
        streams[i].run.push_back(s.timeline.AddEvent());
        RecordingOps ops { s.timeline, done, streams[i].copy.back(), streams[i].run.back() };
        const auto o = RunFrame(ops, ready, false, 0);
        CHECK(o.ok);
        ring.Submitted(i, o.done);
        streams[i].done.push_back(o.done);
    }
    // The fifth call finds no allocator free: it is refused before anything is queued and without taking a fence value.
    const uint64_t readyBefore = ready.last, doneBefore = done.last;
    const size_t q11 = s.timeline.Queue(kD3D11).size(), q12 = s.timeline.Queue(kD3D12).size();
    CHECK(ring.Begin(0) == -1);
    CHECK(ready.last == readyBefore && done.last == doneBefore);
    CHECK(s.timeline.Queue(kD3D11).size() == q11 && s.timeline.Queue(kD3D12).size() == q12);
    // Once the first list is complete its allocator is reused. The module checks the allocator's fence value on the CPU before it
    // records; the model queues that wait, so the dependency on the run it waited for is visible.
    const uint64_t firstDone = streams[0].done[0];
    CHECK(ring.Begin(firstDone) == 0);
    streams[4].copy.push_back(s.timeline.AddEvent());
    streams[4].run.push_back(s.timeline.AddEvent());
    s.timeline.Push(kD3D11, { Op::Wait, kDone, firstDone, 0 });
    RecordingOps ops { s.timeline, done, streams[4].copy.back(), streams[4].run.back() };
    const auto fifth = RunFrame(ops, ready, false, 0);
    CHECK(fifth.ok);
    const Verdict v = Check(s);
    CHECK(v.drainFailures == 0 && v.decreases == 0 && v.satisfiedByOther == 0);
    const std::vector<Op>& q11after = s.timeline.Queue(kD3D11);
    const int copy5 = WorkAt(q11after, streams[4].copy.back());
    const int wait0 = WaitAt(q11after, kDone, firstDone);
    CHECK(copy5 >= 0 && wait0 >= 0 && wait0 < copy5);
    CHECK(SignalAt(s.timeline.Queue(kD3D12), kDone, firstDone) >= 0);
}

// A stream is released while another one is in flight: the release waits for the released stream's own done value on the D3D11 queue
// and nothing waits for the other stream's work.
void TestReleaseWhileAnotherStreamRuns()
{
    Scenario s;
    FenceCounter ready, done;
    Stream a, b;
    for (int k = 0; k < 3; ++k)
    {
        for (Stream* x : { &a, &b })
        {
            x->copy.push_back(s.timeline.AddEvent());
            x->run.push_back(s.timeline.AddEvent());
            RecordingOps ops { s.timeline, done, x->copy.back(), x->run.back() };
            const auto o = RunFrame(ops, ready, false, x->doneOfBuffer[k & 1]);
            CHECK(o.ok);
            x->doneOfBuffer[k & 1] = o.done;
            x->done.push_back(o.done);
        }
    }
    // Stream B runs on alone; stream A is released: the release waits on A's last done value only.
    b.copy.push_back(s.timeline.AddEvent());
    b.run.push_back(s.timeline.AddEvent());
    {
        RecordingOps ops { s.timeline, done, b.copy.back(), b.run.back() };
        const auto o = RunFrame(ops, ready, false, b.doneOfBuffer[0]);
        CHECK(o.ok);
        b.doneOfBuffer[0] = o.done;
        b.done.push_back(o.done);
    }
    const uint64_t aLast = a.done.back();
    s.timeline.Push(kD3D11, { Op::Wait, kDone, aLast, 0 });
    const int release = s.timeline.AddEvent();
    s.timeline.Push(kD3D11, { Op::Work, 0, 0, release });
    const Verdict v = Check(s);
    CHECK(v.drainFailures == 0 && v.decreases == 0 && v.satisfiedByOther == 0 && v.hazards == 0);
    // the release runs after its own wait on A's value, and that value is A's (B's newest run has its own value)
    const std::vector<Op>& q11 = s.timeline.Queue(kD3D11);
    const int waitA = WaitAt(q11, kDone, aLast);
    const int workA = WorkAt(q11, release);
    CHECK(waitA >= 0 && workA > waitA);
    CHECK(aLast != b.done.back());
    // and it is satisfied by A's own signal, not by a later value of another stream
    CHECK(SignalAt(s.timeline.Queue(kD3D12), kDone, aLast) >= 0);
}

// NR before the evaluate and NR after it run in the same frame, one stream each: three copies before the pre-SR ready signal, the
// evaluate ordered after the run, and the post-SR stream's copy before its own ready signal.
void TestPreSrAndPostSrTogether()
{
    Scenario s;
    FenceCounter ready, done;
    Stream post;
    const int frames = 4;
    std::vector<std::vector<int>> preCopies, postCopies;
    std::vector<int> preRuns, preEvaluates, preDone, postRuns;
    for (int k = 0; k < frames; ++k)
    {
        PreSrOps preOps { s.timeline, done, {}, 0 };
        for (size_t i = 0; i < CopyPlanFor(Placement::PreSr).count; ++i)
            preOps.copyEvents.push_back(s.timeline.AddEvent());
        preOps.runEvent = s.timeline.AddEvent();
        const auto o = RunFrame(preOps, ready, false, 0);
        CHECK(o.ok);
        preCopies.push_back(preOps.copyEvents);
        preRuns.push_back(preOps.runEvent);
        preDone.push_back(static_cast<int>(o.done));
        preEvaluates.push_back(s.timeline.AddEvent());
        s.timeline.Push(kD3D11, { Op::Work, 0, 0, preEvaluates.back() }); // the evaluate of this render, after the hand-off

        post.copy.push_back(s.timeline.AddEvent());
        post.run.push_back(s.timeline.AddEvent());
        RecordingOps postOps { s.timeline, done, post.copy.back(), post.run.back() };
        const auto p = RunFrame(postOps, ready, false, post.doneOfBuffer[k & 1]);
        CHECK(p.ok);
        post.doneOfBuffer[k & 1] = p.done;
        postCopies.push_back({ post.copy.back() });
        postRuns.push_back(post.run.back());
    }
    for (int k = 0; k < frames; ++k)
    {
        for (const int copy : preCopies[k])
            s.deps.push_back({ copy, preRuns[k], kCopyBeforeRun });
        s.deps.push_back({ preRuns[k], preEvaluates[k], kRunBeforeEvaluate });
        s.deps.push_back({ postCopies[k][0], postRuns[k], kCopyBeforeRun });
        if (k + 1 < frames)
            for (const int copy : preCopies[k + 1])
                s.deps.push_back({ preRuns[k], copy, kRunBeforeNextCopy });
    }
    const Verdict v = Check(s);
    CHECK(v.schedules == 403);
    CHECK(v.drainFailures == 0 && v.decreases == 0 && v.satisfiedByOther == 0 && v.hazards == 0);
    // the evaluate of each frame follows the wait on that frame's pre-SR run, and the three copies precede the run
    const std::vector<Op>& q11 = s.timeline.Queue(kD3D11);
    for (int k = 0; k < frames; ++k)
    {
        const int wait = WaitAt(q11, kDone, static_cast<uint64_t>(preDone[static_cast<size_t>(k)]));
        const int evaluate = WorkAt(q11, preEvaluates[static_cast<size_t>(k)]);
        const int copy = WorkAt(q11, preCopies[static_cast<size_t>(k)][0]);
        CHECK(wait >= 0 && evaluate > wait && copy < evaluate);
    }
}

// Bridge shutdown with several streams: each stream waits for its own last done value before its objects are released, and every queue
// drains (nothing waits for a value no signal will ever satisfy).
void TestBridgeShutdownWithSeveralStreams()
{
    Scenario s;
    FenceCounter ready, done;
    std::vector<Stream> st(3);
    const int frames = 3;
    for (int k = 0; k < frames; ++k)
    {
        for (Stream& x : st)
        {
            x.copy.push_back(s.timeline.AddEvent());
            x.run.push_back(s.timeline.AddEvent());
            RecordingOps ops { s.timeline, done, x.copy.back(), x.run.back() };
            const auto o = RunFrame(ops, ready, false, x.doneOfBuffer[k & 1]);
            CHECK(o.ok);
            x.doneOfBuffer[k & 1] = o.done;
            x.done.push_back(o.done);
        }
    }
    std::vector<int> releases;
    for (Stream& x : st)
    {
        s.timeline.Push(kD3D11, { Op::Wait, kDone, x.done.back(), 0 });
        releases.push_back(s.timeline.AddEvent());
        s.timeline.Push(kD3D11, { Op::Work, 0, 0, releases.back() });
    }
    const Verdict v = Check(s);
    CHECK(v.schedules == 403);
    CHECK(v.drainFailures == 0 && v.decreases == 0 && v.satisfiedByOther == 0 && v.hazards == 0);
    const std::vector<Op>& q11 = s.timeline.Queue(kD3D11);
    for (size_t i = 0; i < st.size(); ++i)
    {
        const int wait = WaitAt(q11, kDone, st[i].done.back());
        const int work = WorkAt(q11, releases[i]);
        CHECK(wait >= 0 && work > wait);
        CHECK(SignalAt(s.timeline.Queue(kD3D12), kDone, st[i].done.back()) >= 0);
    }
}

// The allocator ring the module's BeginList walks: the first allocator after the last one used whose earlier work finished, wrapping once, and
// nothing at all while the ring is still in flight (the render then keeps the image it has). The production loop resets the allocators in the
// order this returns.
void TestAllocatorChoice()
{
    std::array<uint64_t, kAllocators> value { 10, 20, 30, 40 };
    std::array<size_t, kAllocators> order {};
    const auto doneUpTo = [](uint64_t limit) { return [limit](uint64_t v) { return v <= limit; }; };
    // every allocator finished: the ring is walked from the one after the last used, then wraps
    CHECK(FreeAllocators(value, 0, doneUpTo(100), order) == kAllocators);
    CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3);
    CHECK(FreeAllocators(value, 2, doneUpTo(100), order) == kAllocators);
    CHECK(order[0] == 2 && order[1] == 3 && order[2] == 0 && order[3] == 1); // the wrap
    // the first allocator is still in flight: the rest, in ring order
    CHECK(FreeAllocators(value, 0, doneUpTo(30), order) == 3);
    CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);
    CHECK(FreeAllocators(value, 1, doneUpTo(20), order) == 2);
    CHECK(order[0] == 1 && order[1] == 0); // from 1: 1, then the wrap finds 0

    // nothing finished: no list is started (the render keeps its image)
    CHECK(FreeAllocators(value, 0, doneUpTo(0), order) == 0);
    const std::array<uint64_t, kAllocators> fresh {};
    CHECK(FreeAllocators(fresh, 0, [](uint64_t v) { return v == 0; }, order) == kAllocators);
}

// The release path, as far as it is pure: the module's Maintain asks DecideStream with the stream's own facts (its last live time, whether it has
// GPU work and whether that work finished), and the answer decides whether the objects go. A stream with pending work is never released.
void TestStreamReleasePath()
{
    StreamState st;
    st.key = 0xA000;
    st.lastLive = 1000;
    st.lastSubmitted = 7; // something was submitted: the release waits for the GPU
    // the window is open: the stream stays whatever the GPU is doing
    CHECK(DecideStream(FactsOf(st, false, 1100, LingerMs(5), false)) == StreamAction::Keep);
    // the window closed, the linger is over, and the GPU still reads the stream's images: wait
    CHECK(DecideStream(FactsOf(st, false, 7000, LingerMs(5), false)) == StreamAction::WaitGpu);
    CHECK(DecideStream(FactsOf(st, false, 7000, LingerMs(5), true)) == StreamAction::Release);
    // ... and once NR is switched off the same stream is released as soon as the GPU is done, without the linger
    CHECK(DecideStream(FactsOf(st, true, 1100, LingerMs(5), false)) == StreamAction::WaitGpu);
    CHECK(DecideStream(FactsOf(st, true, 1100, LingerMs(5), true)) == StreamAction::Release);
    // a stream that never submitted anything has no GPU work to wait for
    StreamState idle;
    idle.key = 0xB000;
    idle.lastLive = 1000;
    CHECK(!idle.HasGpuWork());
    CHECK(DecideStream(FactsOf(idle, false, 7000, LingerMs(5), false)) == StreamAction::Release);
}

} // namespace

int main()
{
    TestNewPlanIsSound();
    TestStructure();
    TestOldPlanShowsTheDefects();
    TestPreSrHandoff();
    TestWindowClosesAfterLateFrame();
    TestManyStreamsPerFrame();
    TestAllocatorExhaustion();
    TestAllocatorChoice();
    TestReleaseWhileAnotherStreamRuns();
    TestStreamReleasePath();
    TestPreSrAndPostSrTogether();
    TestBridgeShutdownWithSeveralStreams();
    return TEST_RESULT("chara_nr_timeline");
}
