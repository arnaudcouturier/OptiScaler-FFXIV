// Automatic ReShade scene depth: what the six event callbacks do (ReShadeDepthDispatch.h) over the real binder and a fake ReShade
// world: every event reaches the binder, the callbacks do not look at the enabled flag (a dirty or open runtime is repaired after the
// option was switched off), the frame counter ticks once per finish of a runtime on our device and also when the pass was refused,
// capture and begin of a frame agree on it, the runtime registry follows begin/finish/destroy, and nothing escapes a callback.
#include "pch.h"

#include "ext/reshade/ReShadeDepthDispatch.h"

#include "reshade_depth_fake.h"

#include <type_traits>

using namespace depthtest;

namespace
{
struct FakeHost final : IEventHost
{
    std::vector<uint64_t> noted, forgotten;
    bool ours = true;
    bool throws = false;
    int ourChecks = 0;
    void NoteRuntime(uint64_t rt) override
    {
        if (throws)
            throw std::runtime_error("note");
        noted.push_back(rt);
    }
    void ForgetRuntime(uint64_t rt) override
    {
        if (throws)
            throw std::runtime_error("forget");
        forgotten.push_back(rt);
    }
    bool IsOurRuntime(uint64_t) override
    {
        ++ourChecks;
        if (throws)
            throw std::runtime_error("ours");
        return ours;
    }
};

// The capture side stamps its capture with the same counter the binder reads through Frame().
struct EvRig
{
    Fake fake;
    FakeHost host;
    std::atomic<uint64_t> counter { 100 };
    DepthBinder binder { fake, nullptr };
    EventDispatcher d { binder, host, counter };

    EvRig()
    {
        fake.frameSource = &counter;
        binder.SetEnabled(true);
    }
    void Capture() // the capture side, after an evaluate: stamped with the counter and the clock
    {
        DepthCapture c;
        c.valid = true;
        c.outputW = 3840;
        c.outputH = 2160;
        c.renderW = 1920;
        c.renderH = 1080;
        c.frame = counter.load();
        c.tickMs = fake.now;
        c.copyView = kCopy;
        c.device = kDevice;
        binder.PublishCapture(c);
    }
    bool Clean() const { return !fake.HoldsCopy() && !fake.DepthEntryDead() && fake.Balanced(); }
};

// every callback of the dispatcher is noexcept
static_assert(noexcept(std::declval<EventDispatcher&>().Begin(0)));
static_assert(noexcept(std::declval<EventDispatcher&>().Finish(0)));
static_assert(noexcept(std::declval<EventDispatcher&>().Destroy(0)));
static_assert(noexcept(std::declval<EventDispatcher&>().Reloaded(0)));
static_assert(noexcept(std::declval<EventDispatcher&>().TechniqueState(0)));
static_assert(noexcept(std::declval<EventDispatcher&>().PresetPath(0)));

void EveryEventReachesTheBinder()
{
    { // begin and finish: a pass
        EvRig r;
        r.Capture();
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        CHECK(r.binder.GetCounters().passesBound == 1);
        r.d.Finish(kRt);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // destroy: a runtime that holds the copy is kept as an orphan (no ReShade call, nothing released) ...
        EvRig r;
        r.Capture();
        r.d.Begin(kRt);
        r.fake.log.clear();
        r.d.Destroy(kRt);
        CHECK(r.fake.log.empty());
        CHECK(r.fake.CoreRefs(kCopy) == 1);
        // ... and a runtime that holds nothing is forgotten (what it had learned goes with it)
        EvRig q;
        q.Capture();
        q.d.Begin(kRt);
        q.d.Finish(kRt);
        const int queries = q.fake.defQueries;
        q.d.Destroy(kRt);
        q.counter++;
        q.Capture();
        q.d.Begin(kRt);
        CHECK(q.fake.defQueries > queries);
        q.d.Finish(kRt);
    }
    { // reloaded: the manual transform cache and the trust latch go; technique state and preset path clear the cache only
        for (int which = 0; which < 3; ++which)
        {
            EvRig r;
            r.Capture();
            r.d.Begin(kRt);
            r.d.Finish(kRt);
            int queries = r.fake.defQueries;
            r.counter++;
            r.Capture();
            r.d.Begin(kRt); // cached: no new definition queries
            r.d.Finish(kRt);
            CHECK(r.fake.defQueries == queries);
            queries = r.fake.defQueries;
            const int enums = r.fake.textureEnums;
            if (which == 0)
                r.d.Reloaded(kRt);
            else if (which == 1)
                r.d.TechniqueState(kRt);
            else
                r.d.PresetPath(kRt);
            r.counter++;
            r.Capture();
            r.d.Begin(kRt);
            r.d.Finish(kRt);
            CHECK(r.fake.defQueries > queries);
            CHECK(r.fake.textureEnums > enums); // (every pass enumerates the variables; the cache is the macros)
            CHECK(r.Clean());
        }
    }
    { // the manual transform cache really is what the technique-state and preset-path events clear
        EvRig r;
        r.Capture();
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; // set behind the cache's back
        r.counter++;
        r.Capture();
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy)); // still bound: nobody told us
        r.d.Finish(kRt);
        r.d.PresetPath(kRt);
        r.counter++;
        r.Capture();
        r.d.Begin(kRt);
        CHECK(!r.fake.HoldsCopy()); // told: stands down
        r.d.Finish(kRt);
    }
}

// The callbacks never look at the enabled flag: the binder's retry comes first on purpose.
void NotGatedByTheEnabledFlag()
{
    { // a pass is open when the option is switched off: the finish still puts ReShade's binding back
        EvRig r;
        r.Capture();
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        r.binder.SetEnabled(false);
        r.d.Finish(kRt);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // a dirty runtime (the restore failed) is repaired by the finish of the next frame, with the option off
        EvRig r;
        r.Capture();
        r.fake.failRestore = true;
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.fake.HoldsCopy());
        r.binder.SetEnabled(false);
        r.fake.failRestore = false;
        r.d.Finish(kRt);
        CHECK(!r.fake.HoldsCopy() && r.Clean());
    }
    { // ... or by the next begin, which then refuses to bind
        EvRig r;
        r.Capture();
        r.fake.failRestore = true;
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.fake.HoldsCopy());
        r.binder.SetEnabled(false);
        r.fake.failRestore = false;
        r.counter++;
        r.Capture();
        r.d.Begin(kRt);
        CHECK(!r.fake.HoldsCopy() && r.Clean());
        CHECK(r.binder.GetCounters().passesBound == 1); // no new pass
    }
    { // switched off, a begin binds nothing
        EvRig r;
        r.Capture();
        r.binder.SetEnabled(false);
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.fake.updates.empty());
    }
}

void FrameCounter()
{
    { // one tick per finish of a runtime on our device
        EvRig r;
        r.Capture();
        CHECK(r.counter == 100);
        r.d.Begin(kRt);
        CHECK(r.counter == 100); // a begin does not tick
        r.d.Finish(kRt);
        CHECK(r.counter == 101);
        r.d.Finish(kRt); // a finish with no pass open is still a finish
        CHECK(r.counter == 102);
        r.d.Reloaded(kRt);
        r.d.TechniqueState(kRt);
        r.d.PresetPath(kRt);
        r.d.Destroy(kRt);
        CHECK(r.counter == 102);
    }
    { // a runtime of another device does not tick it
        EvRig r;
        r.host.ours = false;
        r.Capture();
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 100 && r.host.ourChecks == 1);
    }
    { // refused passes tick it, too (a counter that only ticked on bound passes would stall when a capture is missing)
        EvRig r; // no capture yet
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 101);
        r.binder.SetEnabled(false); // disabled
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 102);
        r.binder.SetEnabled(true);
        r.Capture();
        r.fake.now += 500; // stale
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 103 && r.binder.GetCounters().staleCaptures == 1);
        r.fake.d3d11 = true;
        r.fake.sameDevice = false; // not our device for the binder
        r.Capture();
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 104);
    }
    { // the capture side and the begin of one frame agree (delta 0); after the finish the next capture carries the next value
        EvRig r;
        for (int frame = 0; frame < 5; ++frame)
        {
            r.fake.now += 16;
            r.Capture();
            const uint64_t stamped = r.counter.load();
            r.d.Begin(kRt);
            CHECK(r.fake.DepthIs(kCopy, kCopy));
            r.d.Finish(kRt);
            CHECK(r.counter.load() == stamped + 1);
        }
        CHECK(r.binder.GetCounters().passesBound == 5 && r.binder.GetCounters().staleCaptures == 0);
    }
    { // a second pass in the same frame (the boundary pass, then ReShade's own) is one behind and still binds; a third is too stale
        EvRig r;
        r.Capture();
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        r.d.Finish(kRt);
        r.d.Begin(kRt);
        CHECK(!r.fake.HoldsCopy() && r.binder.GetCounters().staleCaptures == 1);
        r.d.Finish(kRt);
        CHECK(r.counter == 103);
    }
}

void RuntimeRegistry()
{
    EvRig r;
    r.Capture();
    r.d.Begin(kRt);
    CHECK(r.host.noted.size() == 1 && r.host.noted[0] == kRt);
    r.d.Finish(kRt);
    CHECK(r.host.noted.size() == 2 && r.host.noted[1] == kRt);
    r.d.Reloaded(kRt);
    r.d.TechniqueState(kRt);
    r.d.PresetPath(kRt);
    CHECK(r.host.noted.size() == 2 && r.host.forgotten.empty());
    r.d.Destroy(kRt);
    CHECK(r.host.forgotten.size() == 1 && r.host.forgotten[0] == kRt);
}

void NothingEscapes()
{
    EvRig r;
    r.host.throws = true;
    r.Capture();
    bool escaped = false;
    try
    {
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        r.d.Destroy(kRt);
        r.d.Reloaded(kRt);
    }
    catch (...)
    {
        escaped = true;
    }
    CHECK(!escaped);
    CHECK(r.counter == 100); // a host that cannot answer is not "ours"
    CHECK(r.binder.GetCounters().passesBound == 1); // the binder was reached all the same
}
} // namespace

int main()
{
    EveryEventReachesTheBinder();
    NotGatedByTheEnabledFlag();
    FrameCounter();
    RuntimeRegistry();
    NothingEscapes();
    if (g_fail != 0)
    {
        std::printf("reshade_depth_events: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_depth_events: all checks passed");
    return 0;
}
