// Automatic ReShade scene depth: what the six event callbacks do (ReShadeDepthDispatch.h) over the real binder and a fake ReShade
// world: every event reaches the binder, the callbacks do not look at the enabled flag (a dirty or open runtime is repaired after the
// option was switched off), no event ever moves the frame counter (it belongs to the capture side: one tick per capture), many passes
// between two captures all see the same stamp and bind, the runtime registry follows begin/finish/destroy, and nothing escapes a callback.
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
    bool throws = false;
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
};

// The capture side stamps its capture with the same counter the binder reads through Frame(), and ticks it once per capture.
struct EvRig
{
    Fake fake;
    FakeHost host;
    std::atomic<uint64_t> counter { 100 };
    DepthBinder binder { fake, nullptr };
    EventDispatcher d { binder, host };

    EvRig()
    {
        fake.frameSource = &counter;
        binder.SetEnabled(true);
    }
    void Capture() // the capture side, after an evaluate: one tick of the counter, the capture stamped with it, and the clock
    {
        DepthCapture c;
        c.valid = true;
        c.outputW = 3840;
        c.outputH = 2160;
        c.renderW = 1920;
        c.renderH = 1080;
        c.frame = ++counter;
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
            r.Capture();
            r.d.Begin(kRt);
            r.d.Finish(kRt);
            CHECK(r.fake.defQueries > queries);
            CHECK(r.fake.textureEnums > enums); // (every pass enumerates the variables; the cache is the macros)
            CHECK(r.Clean());
        }
    }
    { // a reload while a pass is open ends it (the effects are new: the binding is put back, the runtime forgotten); the cache events do not
        for (const bool reload : { true, false })
        {
            EvRig r;
            r.Capture();
            r.d.Begin(kRt);
            CHECK(r.fake.DepthIs(kCopy, kCopy));
            if (reload)
                r.d.Reloaded(kRt);
            else
                r.d.PresetPath(kRt);
            CHECK(r.fake.HoldsCopy() == !reload);
            r.d.Finish(kRt);
            CHECK(r.Clean());
        }
    }
    { // the manual transform cache really is what the technique-state and preset-path events clear
        EvRig r;
        r.Capture();
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; // set behind the cache's back
        r.Capture();
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy)); // still bound: nobody told us
        r.d.Finish(kRt);
        r.d.PresetPath(kRt);
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
    { // the counter belongs to the capture side: one tick per capture, and no event moves it
        EvRig r;
        CHECK(r.counter == 100);
        r.Capture();
        CHECK(r.counter == 101);
        r.d.Begin(kRt);
        CHECK(r.counter == 101);
        r.d.Finish(kRt);
        CHECK(r.counter == 101);
        r.d.Finish(kRt); // a finish with no pass open
        r.d.Reloaded(kRt);
        r.d.TechniqueState(kRt);
        r.d.PresetPath(kRt);
        r.d.Destroy(kRt);
        CHECK(r.counter == 101);
    }
    { // one capture, then five passes (frame generation: ReShade runs every OUTPUT frame, the evaluate once per real frame): all bind
        EvRig r;
        r.Capture();
        for (int pass = 0; pass < 5; ++pass)
        {
            r.d.Begin(kRt);
            CHECK(r.fake.DepthIs(kCopy, kCopy));
            r.d.Finish(kRt);
            CHECK(r.fake.DepthIs(kOrig, kOrigSrgb));
        }
        CHECK(r.binder.GetCounters().passesBound == 5 && r.binder.GetCounters().staleCaptures == 0);
        CHECK(r.counter == 101 && r.Clean());
    }
    { // a second capture replaces the stamp, the next pass binds, and the counter moved once for it
        EvRig r;
        r.Capture();
        for (int pass = 0; pass < 3; ++pass)
        {
            r.d.Begin(kRt);
            r.d.Finish(kRt);
        }
        r.fake.now += 16;
        r.Capture();
        CHECK(r.counter == 102);
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        r.d.Finish(kRt);
        CHECK(r.binder.GetCounters().passesBound == 4 && r.binder.GetCounters().staleCaptures == 0);
    }
    { // no capture for more than 250 ms: stale by AGE (the real guard) although the passes continue; the counter did not move
        EvRig r;
        r.Capture();
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        r.fake.now += 250; // exactly the limit is still fresh
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        r.d.Finish(kRt);
        r.fake.now += 1; // 251 ms
        r.d.Begin(kRt);
        CHECK(!r.fake.HoldsCopy() && r.binder.GetCounters().staleCaptures == 1);
        r.d.Finish(kRt);
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.binder.GetCounters().staleCaptures == 2 && r.counter == 101);
        r.Capture(); // the evaluate is back
        r.d.Begin(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        r.d.Finish(kRt);
    }
    { // refused passes never move the counter: no capture, disabled, stale, not our device
        EvRig r; // no capture yet
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 100);
        r.binder.SetEnabled(false);
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 100);
        r.binder.SetEnabled(true);
        r.Capture();
        CHECK(r.counter == 101);
        r.fake.now += 500; // stale
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 101 && r.binder.GetCounters().staleCaptures == 1);
        r.fake.sameDevice = false; // not our device for the binder
        r.fake.now += 16;
        r.Capture();
        CHECK(r.counter == 102);
        r.d.Begin(kRt);
        r.d.Finish(kRt);
        CHECK(r.counter == 102);
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
    CHECK(r.counter == 101); // (the one capture of the rig; no event moved it)
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
