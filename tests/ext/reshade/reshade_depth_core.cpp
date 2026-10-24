// Automatic ReShade scene depth: the binder driven by a fake ReShade backend (no ReShade, no GPU). The fake (reshade_depth_fake.h)
// models ReShade's single DEPTH binding, counts references per view handle and treats any use of a freed view as a failure, so
// every path is checked for leaving ReShade exactly as it was, or for keeping what a pending restore still needs.
#include "reshade_depth_fake.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>

// Counts allocations while g_countAllocs is set: the refusal paths must not allocate.
static std::atomic<size_t> g_allocs { 0 };
static std::atomic<bool> g_countAllocs { false };
void* operator new(size_t n)
{
    if (g_countAllocs.load(std::memory_order_relaxed))
        ++g_allocs;
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace depthtest;

namespace
{
const char* const kDisabled = "Automatic ReShade depth disabled";
const char* const kWaiting = "Waiting for ReShade depth effects";
const char* const kUnavailable = "Scene depth unavailable; generic depth retained";
const char* const kSizeMismatch = "ReShade size differs from the scene depth; generic depth retained";
const char* const kManual = "Manual ReShade depth transform retained";
const char* const kNoInput = "No depth input found; generic depth retained";
const char* const kNotTrusted = "Depth input not trusted; generic depth retained";
const char* const kFailed = "ReShade call failed; generic depth retained";

// every public entry is noexcept in the declaration
static_assert(noexcept(std::declval<DepthBinder&>().OnBeginEffects(0)));
static_assert(noexcept(std::declval<DepthBinder&>().OnFinishEffects(0)));
static_assert(noexcept(std::declval<DepthBinder&>().OnDestroyRuntime(0)));
static_assert(noexcept(std::declval<DepthBinder&>().OnReloadedEffects(0)));
static_assert(noexcept(std::declval<DepthBinder&>().OnTechniquesChanged(0)));
static_assert(noexcept(std::declval<DepthBinder&>().PublishCapture(DepthCapture {})));
static_assert(noexcept(std::declval<DepthBinder&>().SetEnabled(true)));
static_assert(noexcept(std::declval<const DepthBinder&>().Status()));
static_assert(noexcept(std::declval<const DepthBinder&>().GetCounters()));
static_assert(noexcept(std::declval<const DepthBinder&>().GetConsumer(0).seen));

Fake::Var DepthVar() { return { kDepthVar, "ReShade::DepthBufferTex", "DEPTH", 0, 0 }; }

// ---- binding and restoring ------------------------------------------------------------------------------------------------
void HappyPath()
{
    Rig r;
    const std::string before = r.fake.StateKey();
    CHECK(r.binder.Status() == kWaiting);
    CHECK(r.Begin() == Outcome::Bound);
    CHECK(r.binder.Status() == "Automatic scene depth: 1920x1080 -> 3840x2160");
    CHECK(r.fake.updates.size() == 1);
    CHECK(r.fake.UpdateAt(0).semantic == "DEPTH" && r.fake.UpdateAt(0).srv == kCopy && r.fake.UpdateAt(0).srgb == kCopy);
    CHECK(r.fake.DepthIs(kCopy, kCopy));
    CHECK(r.fake.unis[0].value);
    CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kOrigSrgb) == 1); // held for the pass
    CHECK(r.fake.textureEnums == 1 && r.fake.uniformEnums == 1);
    CHECK(r.binder.GetCounters().passesBound == 1);

    r.Finish();
    CHECK(r.fake.updates.size() == 2);
    CHECK(r.fake.UpdateAt(1).semantic == "DEPTH" && r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(1).srgb == kOrigSrgb);
    CHECK(r.fake.StateKey() == before);
    CHECK(r.Clean());
    CHECK(r.binder.GetCounters().overwrittenByOther == 0);

    // render size equal to the output size: one size in the status
    Rig q;
    DepthCapture c = q.Capture();
    c.renderW = 3840;
    c.renderH = 2160;
    q.binder.PublishCapture(c);
    CHECK(q.Begin() == Outcome::Bound);
    CHECK(q.binder.Status() == "Automatic scene depth: 3840x2160");
    q.Finish();

    // render and output differing in one dimension only still print both
    for (const auto& [w, h] : { std::pair { 3840u, 1080u }, std::pair { 1920u, 2160u } })
    {
        Rig t;
        DepthCapture d = t.Capture();
        d.renderW = w;
        d.renderH = h;
        t.binder.PublishCapture(d);
        CHECK(t.Begin() == Outcome::Bound);
        CHECK(t.binder.Status() == std::format("Automatic scene depth: {}x{} -> 3840x2160", w, h));
        t.Finish();
    }

    // a new capture replaces the copy that gets bound
    Rig p;
    DepthCapture c2 = p.Capture();
    c2.copyView = kCopy2;
    p.binder.PublishCapture(c2);
    CHECK(p.Begin() == Outcome::Bound);
    CHECK(p.fake.UpdateAt(0).srv == kCopy2 && p.fake.UpdateAt(0).srgb == kCopy2);
    p.Finish();
    CHECK(p.Clean());
}

void PassesInOneFrame()
{
    { // no once-per-frame dedupe: ReShade runs its pass once per frame anyway, a second pass binds again
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.binder.GetCounters().passesBound == 2 && r.fake.updates.size() == 4);
        CHECK(r.Clean());
    }
    { // a refused begin consumes nothing: the capture arrives, the very next begin of the same frame binds
        Rig r;
        r.binder.PublishCapture(DepthCapture {});
        CHECK(r.Begin() == Outcome::NoCapture);
        r.Publish();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.Clean());
    }
    { // ... and so does a depth input that shows up
        Rig r;
        r.fake.vars.clear();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        r.fake.vars.push_back(DepthVar());
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.binder.GetCounters().noDepthInput == 1); // counted per refused begin, not per frame
    }
}

void OriginalBindings()
{
    { // "no depth selected": the entry is empty, restoring puts nothing back (the entry is erased again)
        Rig r;
        r.fake.SetDepth(0, 0);
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 0); // no reference to a null view
        r.Finish();
        CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(1).srv == 0 && r.fake.UpdateAt(1).srgb == 0);
        CHECK(!r.fake.hasDepth && r.Clean());
    }
    { // only one of the two views set
        Rig r;
        r.fake.SetDepth(kOrig, 0);
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(1).srgb == 0);
        CHECK(r.fake.DepthIs(kOrig, 0) && r.Clean());
    }
    { // the same view as srv and srgb: two references, two releases
        Rig r;
        r.fake.SetDepth(kOrig, kOrig);
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.CoreRefs(kOrig) == 2);
        r.Finish();
        CHECK(r.Clean());
    }
    // A binding that already holds the copy is never read as an "original": it would be restored as the copy and stay bound
    // for good. The begin is refused and nothing is referenced, saved or bound.
    for (const auto& [srv, srgb] : { std::pair { kCopy, kCopy }, std::pair { kCopy, kOrigSrgb }, std::pair { kOrig, kCopy } })
    {
        Rig r;
        r.fake.SetDepth(srv, srgb);
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.binder.Status() == kFailed);
        CHECK(r.fake.updates.empty() && r.fake.DepthIs(srv, srgb));
        CHECK(r.fake.Balanced());
        r.Finish();
        CHECK(r.fake.updates.empty() && r.fake.Balanced());
    }
}

void RestoreOnlyIfOurs()
{
    { // another add-on replaced both views mid-pass (Generic Depth picked a new buffer): leave them, count it
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.SetDepth(kOther, kOtherSrgb);
        r.Finish();
        CHECK(r.fake.updates.size() == 1); // no restore
        CHECK(r.fake.DepthIs(kOther, kOtherSrgb));
        CHECK(r.binder.GetCounters().overwrittenByOther == 1);
        CHECK(!r.fake.unis[0].value); // the bool is restored anyway
        CHECK(r.Clean());
    }
    // A pair with only one half replaced still holds the copy, so it is ours: put back, never released while the map may hold it
    for (const auto& [srv, srgb] : { std::pair { kCopy, kOtherSrgb }, std::pair { kOther, kCopy } })
    {
        Rig r;
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.SetDepth(srv, srgb);
        r.Finish();
        CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(1).srgb == kOrigSrgb);
        CHECK(!r.fake.HoldsCopy() && r.fake.StateKey() == before);
        CHECK(r.binder.GetCounters().overwrittenByOther == 0);
        CHECK(r.Clean());
    }
    { // replaced by "nothing": also not ours
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.SetDepth(0, 0);
        r.Finish();
        CHECK(r.fake.updates.size() == 1 && r.binder.GetCounters().overwrittenByOther == 1);
        CHECK(r.Clean());
    }
    { // a bind that fails after it was verified, with another add-on's pick in the entry: that pick stays
        int getBool = 0;
        {
            Rig q;
            q.Begin();
            for (size_t i = 0; i < q.fake.log.size() && getBool == 0; ++i)
                if (q.fake.log[i] == "GetUniformBool")
                    getBool = int(i) + 1;
        }
        CHECK(getBool > 0);
        Rig r;
        r.fake.afterCall = [&](const char* name)
        {
            if (std::string(name) == "UniformVariables")
                r.fake.SetDepth(kOther, kOtherSrgb);
        };
        r.fake.faultAt = getBool;
        r.fake.fault = Fault::Fail;
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.fake.DepthIs(kOther, kOtherSrgb) && r.fake.updates.size() == 1);
        CHECK(r.binder.GetCounters().overwrittenByOther == 0); // only a finish counts it
        CHECK(r.Clean());
    }
    { // the binding cannot be read at finish: the copy may still be bound, so the runtime is dirty and keeps its references
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].handle = 0x999; // the variable is gone
        r.Finish();
        CHECK(r.fake.updates.size() == 1 && r.fake.DepthIs(kCopy, kCopy));
        CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kOrigSrgb) == 1);
        CHECK(r.fake.BoundCopiesReferenced());
        CHECK(!r.fake.unis[0].value); // the bools were put back anyway
        CHECK(r.binder.GetCounters().overwrittenByOther == 0); // an unreadable binding is not "replaced by another add-on"
        CHECK(r.binder.Status() == kFailed);
        r.binder.OnDestroyRuntime(kRt); // the runtime and its map are gone: now the references can go
        CHECK(r.fake.Balanced());
    }
}

// ---- gates ----------------------------------------------------------------------------------------------------------------
// Applies `breakIt` to a fresh rig and checks that the begin refuses with `outcome`, that the status is `status` (null = left
// as it was: a runtime that is not ours never writes it), and that ReShade is untouched and nothing is referenced.
template <class F> void Refuses(Outcome outcome, const char* status, F&& breakIt, const char* what)
{
    Rig r;
    breakIt(r);
    const std::string before = r.fake.StateKey();
    const std::string statusBefore = r.binder.Status();
    const Outcome got = r.Begin();
    const std::string want = status != nullptr ? status : statusBefore;
    if (got != outcome || r.binder.Status() != want)
        std::printf("  gate '%s': got outcome %d, status '%s'\n", what, int(got), r.binder.Status().c_str());
    CHECK(got == outcome);
    CHECK(r.binder.Status() == want);
    CHECK(r.fake.updates.empty());
    CHECK(r.fake.StateKey() == before);
    CHECK(r.Clean());
    r.Finish();
    CHECK(r.fake.updates.empty() && r.Clean());
}

void Gates()
{
    Refuses(Outcome::Disabled, kDisabled, [](Rig& r) { r.binder.SetEnabled(false); }, "disabled");
    { // disabled: not one backend call
        Rig r(false);
        CHECK(r.Begin() == Outcome::Disabled);
        CHECK(r.fake.log.empty());
    }
    Refuses(Outcome::NotD3D11, nullptr, [](Rig& r) { r.fake.d3d11 = false; }, "not D3D11");
    Refuses(Outcome::OtherDevice, nullptr, [](Rig& r) { r.fake.sameDevice = false; }, "other device");
    Refuses(Outcome::NoCapture, kWaiting, [](Rig& r) { r.binder.PublishCapture(DepthCapture {}); }, "no capture");
    Refuses(Outcome::NoCapture, kWaiting,
            [](Rig& r)
            {
                DepthCapture c = r.Capture();
                c.valid = false;
                r.binder.PublishCapture(c);
            },
            "withdrawn capture");
    Refuses(Outcome::CaptureUnavailable, kUnavailable,
            [](Rig& r)
            {
                DepthCapture c = r.Capture();
                c.copyView = 0;
                r.binder.PublishCapture(c);
            },
            "capture without a copy");
    Refuses(Outcome::CaptureUnavailable, kUnavailable,
            [](Rig& r)
            {
                DepthCapture c = r.Capture();
                c.device = 0;
                r.binder.PublishCapture(c);
            },
            "capture without a device");
    Refuses(Outcome::CaptureUnavailable, kUnavailable, [](Rig& r) { r.fake.now += 251; }, "251 ms old");
    Refuses(Outcome::CaptureUnavailable, kUnavailable, [](Rig& r) { r.fake.frame += 2; }, "2 frames old");
    Refuses(Outcome::CaptureUnavailable, kUnavailable, [](Rig& r) { r.fake.frame -= 1; }, "from the future");
    Refuses(Outcome::SizeMismatch, kSizeMismatch, [](Rig& r) { r.fake.bbW = 1920; }, "width");
    Refuses(Outcome::SizeMismatch, kSizeMismatch, [](Rig& r) { r.fake.bbH = 2161; }, "height");
    // the back buffer size is read before the capture is looked at: a failed query is a failed pass, not a mismatch
    Refuses(Outcome::Failed, kFailed, [](Rig& r) { r.fake.bbOk = false; }, "size unknown");
    Refuses(Outcome::Failed, kFailed,
            [](Rig& r)
            {
                DepthCapture c = r.Capture();
                c.outputW = c.outputH = 0; // an unreadable size must not pass as "0x0 equals 0x0"
                r.binder.PublishCapture(c);
                r.fake.bbOk = false;
            },
            "size unknown, empty capture");
    Refuses(Outcome::ManualTransform, kManual, [](Rig& r) { r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; }, "manual");
    Refuses(Outcome::NoDepthInput, kNoInput, [](Rig& r) { r.fake.vars.clear(); }, "no variables");
    Refuses(Outcome::Failed, kFailed, [](Rig& r) { r.fake.definitionError = true; }, "definition query failed");

    { // a capture that cannot be bound is never referenced
        for (int variant = 0; variant < 3; ++variant)
        {
            Rig r;
            DepthCapture c = r.Capture();
            c.valid = variant != 0;
            c.copyView = variant == 1 ? 0 : kCopy;
            c.device = variant == 2 ? 0 : kDevice;
            r.binder.PublishCapture(c);
            r.Begin();
            CHECK(r.fake.Calls("AddRef") == 0 && r.fake.Calls("Release") == 0);
        }
    }

    // boundary values pass
    {
        Rig r; // 250 ms old, 1 frame old
        r.fake.now += 250;
        r.fake.frame += 1;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }

    // counters of the gates that have one; "no capture yet" is not a stale capture
    Rig r;
    r.binder.PublishCapture(DepthCapture {});
    r.Begin();
    CHECK(r.binder.GetCounters().staleCaptures == 0);
    r.Publish();
    r.fake.now += 251;
    r.Begin();
    CHECK(r.binder.GetCounters().staleCaptures == 1);
    r.NextFrame();
    r.fake.bbW = 100;
    r.Begin();
    CHECK(r.binder.GetCounters().sizeMismatches == 1);
    r.fake.sameDevice = false;
    r.Begin();
    r.fake.sameDevice = true;
    r.fake.d3d11 = false;
    r.Begin();
    CHECK(r.binder.GetCounters().otherDevice == 2); // not ours: another device, or not D3D11
    const Counters c = r.binder.GetCounters();
    CHECK(c.overwrittenByOther == 0 && c.manualTransformStandDowns == 0 && c.noDepthInput == 0 && c.passesBound == 0);
}

void GateOrder()
{
    // Everything is wrong at once; fixing the gates one by one must move the outcome down the list in order.
    Rig r(false);
    enum class CaptureMode { Withdrawn, Stale, Fresh } mode = CaptureMode::Withdrawn;
    r.fake.d3d11 = false;
    r.fake.sameDevice = false;
    r.fake.globalDefs["RESHADE_DEPTH_INPUT_Y_OFFSET"] = "3";
    r.fake.vars.clear();
    r.fake.bbW = 100;

    auto step = [&](Outcome want, const char* what)
    {
        ++r.fake.frame;
        r.fake.now += 16;
        DepthCapture c = r.Capture(); // the device stays known: a withdrawn capture still says which device it was made on
        c.valid = mode != CaptureMode::Withdrawn;
        if (mode == CaptureMode::Stale)
            c.tickMs -= 1000;
        r.binder.PublishCapture(c);
        const Outcome got = r.Begin();
        if (got != want)
            std::printf("  order: expected '%s', got outcome %d\n", what, int(got));
        CHECK(got == want);
        r.Finish();
    };
    step(Outcome::Disabled, "disabled");
    r.binder.SetEnabled(true);
    step(Outcome::NotD3D11, "not D3D11");
    r.fake.d3d11 = true;
    step(Outcome::OtherDevice, "other device");
    r.fake.sameDevice = true;
    step(Outcome::ManualTransform, "manual transform");
    CHECK(r.fake.textureEnums == 0); // reported without looking for a depth input
    CHECK(!r.binder.GetConsumer(r.fake.now).seen);
    r.fake.globalDefs.clear();
    r.binder.OnReloadedEffects(kRt); // the manual check is cached until effects reload
    step(Outcome::NoDepthInput, "no depth input");
    CHECK(!r.binder.GetConsumer(r.fake.now).seen);
    r.fake.vars.push_back(DepthVar());
    step(Outcome::NoCapture, "no capture");
    CHECK(r.binder.GetConsumer(r.fake.now).seen); // a depth input was found: the capture gate may open
    mode = CaptureMode::Stale;
    step(Outcome::CaptureUnavailable, "stale capture");
    mode = CaptureMode::Fresh;
    step(Outcome::SizeMismatch, "size");
    r.fake.bbW = 3840;
    step(Outcome::Bound, "bound");
    const Counters c = r.binder.GetCounters();
    CHECK(c.otherDevice == 2 && c.manualTransformStandDowns == 1 && c.noDepthInput == 1 && c.staleCaptures == 1 &&
          c.sizeMismatches == 1 && c.passesBound == 1);
    CHECK(r.Clean());
}

void ClockAfterSnapshot()
{
    // The capture side publishes the next capture (and the frame counter and the clock move on) right after the core took its
    // snapshot. Frame and clock are read after the snapshot, so the snapshot is at most one frame and a few milliseconds old.
    // Read before it, the newer capture would look like it came from the future.
    Rig r;
    bool moved = false;
    r.fake.afterCall = [&](const char*)
    {
        if (moved)
            return;
        moved = true;
        ++r.fake.frame;
        r.fake.now += 5;
        r.Publish();
    };
    CHECK(r.Begin() == Outcome::Bound);
    CHECK(moved);
    r.Finish();
    CHECK(r.Clean());
}

void ConsumerInfoWindow()
{
    Rig r;
    CHECK(!r.binder.GetConsumer(r.fake.now).seen); // nothing has looked yet
    CHECK(!r.binder.GetConsumer(0).seen);
    {
        const ConsumerInfo none = r.binder.GetConsumer(r.fake.now);
        CHECK(none.seenMs == 0 && none.width == 0 && none.height == 0);
    }
    r.binder.PublishCapture(DepthCapture {});
    const int64_t at = r.fake.now;
    CHECK(r.Begin() == Outcome::NoCapture); // no capture yet: the begin still records the consumer ...
    {
        const ConsumerInfo c = r.binder.GetConsumer(at);
        CHECK(c.seen && c.seenMs == at);
        CHECK(c.width == 3840 && c.height == 2160); // ... and the size of its back buffer, which the capture side filters on
    }
    CHECK(r.binder.GetConsumer(at + 1000).seen);
    CHECK(!r.binder.GetConsumer(at + 1001).seen); // stale after a second, whatever the size says
    CHECK(r.binder.GetConsumer(at + 1001).width == 3840);
    CHECK(r.binder.GetConsumer(at - 5).seen); // an earlier reading on another thread is not "long ago"

    { // the size is whatever the begin read; a later begin of the same or another runtime replaces it
        r.fake.bbW = 1920;
        r.fake.bbH = 1080;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::SizeMismatch); // the size gate still refuses ...
        const ConsumerInfo c = r.binder.GetConsumer(r.fake.now);
        CHECK(c.seen && c.width == 1920 && c.height == 1080); // ... after the consumer was recorded
    }
    { // the back buffer size cannot be read: not a consumer, and not a size mismatch either
        Rig q;
        q.fake.bbOk = false;
        CHECK(q.Begin() == Outcome::Failed);
        CHECK(q.binder.Status() == kFailed);
        const ConsumerInfo c = q.binder.GetConsumer(q.fake.now);
        CHECK(!c.seen && c.width == 0 && c.height == 0);
        CHECK(q.binder.GetCounters().failed == 1 && q.binder.GetCounters().sizeMismatches == 0);
        q.fake.bbOk = true;
        q.NextFrame();
        CHECK(q.Begin() == Outcome::Bound); // nothing is latched
        q.Finish();
    }
    { // two runtimes: the most recent begin that found a trusted depth input wins
        Fake a, b;
        Router router(a, b);
        DepthBinder binder(router, nullptr);
        binder.SetEnabled(true);
        b.bbW = 1920;
        b.bbH = 1080;
        binder.PublishCapture(DepthCapture {});
        CHECK(binder.OnBeginEffects(kRt) == Outcome::NoCapture);
        CHECK(binder.GetConsumer(a.now).width == 3840);
        a.now += 5;
        CHECK(binder.OnBeginEffects(kRt2) == Outcome::NoCapture);
        ConsumerInfo c = binder.GetConsumer(a.now);
        CHECK(c.width == 1920 && c.height == 1080 && c.seenMs == a.now);
        a.now += 5;
        CHECK(binder.OnBeginEffects(kRt) == Outcome::NoCapture);
        CHECK(binder.GetConsumer(a.now).width == 3840);
        b.sameDevice = false; // a runtime that is not ours does not take over
        a.now += 5;
        CHECK(binder.OnBeginEffects(kRt2) == Outcome::OtherDevice);
        CHECK(binder.GetConsumer(a.now).width == 3840);
    }
    { // before the first capture a runtime of another device does not record a consumer and writes no status
        Rig q;
        q.binder.PublishCapture(DepthCapture {});
        q.fake.sameDevice = false;
        const std::string status = q.binder.Status();
        CHECK(q.Begin() == Outcome::OtherDevice);
        CHECK(!q.binder.GetConsumer(q.fake.now).seen);
        CHECK(q.binder.Status() == status && status == kWaiting);
        CHECK(q.fake.Calls("SameDevice") == 1); // always asked, also with no capture yet
    }

    // only a begin that got past the depth input gate counts: not another device, a manual transform, nothing to read
    struct Case
    {
        const char* what;
        void (*breakIt)(Rig&);
        Outcome outcome;
    };
    const Case cases[] = {
        { "other device", [](Rig& q) { q.fake.sameDevice = false; }, Outcome::OtherDevice },
        { "not D3D11", [](Rig& q) { q.fake.d3d11 = false; }, Outcome::NotD3D11 },
        { "manual transform", [](Rig& q) { q.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; }, Outcome::ManualTransform },
        { "no depth input", [](Rig& q) { q.fake.vars.clear(); }, Outcome::NoDepthInput },
        { "disabled", [](Rig& q) { q.binder.SetEnabled(false); }, Outcome::Disabled },
        { "depth input that is not trusted",[](Rig& q) { q.fake.vars = { { 0x900, "DepthInputTex", "", kColor, kColor } }; },
          Outcome::NoDepthInput },
    };
    for (const Case& c : cases)
    {
        Rig q;
        c.breakIt(q);
        CHECK(q.Begin() == c.outcome);
        CHECK(!q.binder.GetConsumer(q.fake.now).seen);
    }
    // a stale or mismatching capture does not matter: the consumer is there
    Rig s;
    s.fake.now += 300;
    CHECK(s.Begin() == Outcome::CaptureUnavailable);
    CHECK(s.binder.GetConsumer(s.fake.now).seen);
}

void Untrusted()
{
    { // a variable with the right name but no DEPTH semantic: its binding says nothing about the DEPTH entry
        Rig r;
        r.fake.vars = { { 0x900, "DepthInputTex", "", kColor, kColor } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(r.binder.Status() == kNotTrusted);
        CHECK(r.fake.updates.empty() && r.fake.StateKey() == before && r.Clean());
        CHECK(r.binder.GetCounters().noDepthInput == 1);
        CHECK(!r.binder.GetConsumer(r.fake.now).seen);

        // latched until the effects reload, even if the variable changes
        r.fake.vars[0].srv = r.fake.vars[0].srgb = kOrig;
        r.fake.bindingReads.clear();
        r.NextFrame();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(r.fake.bindingReads.empty()); // not looked at again
        CHECK(r.binder.GetCounters().noDepthInput == 2);
        r.fake.vars = { DepthVar() };
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.Pass());
        CHECK(r.Clean());
    }
    { // the latch is per runtime and ends with a destroy, too
        Rig r;
        r.fake.vars = { { 0x900, "DepthInputTex", "", kColor, kColor } };
        CHECK(r.Begin() == Outcome::NoDepthInput);
        r.fake.vars = { DepthVar() };
        r.NextFrame();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        r.binder.OnDestroyRuntime(kRt);
        CHECK(r.Pass());
    }
    { // one bad view of the two is enough
        Rig r;
        r.fake.vars = { { 0x900, "DepthInputTex", "", kOrig, kColor } };
        CHECK(r.Begin() == Outcome::NoDepthInput);
        Rig q;
        q.fake.vars = { { 0x900, "DepthInputTex", "", kColor, kOrigSrgb } };
        CHECK(q.Begin() == Outcome::NoDepthInput);
    }
    { // an empty binding is trusted without asking; depth views are asked about once, at the first sight of the variable
        Rig r;
        r.fake.SetDepth(0, 0);
        CHECK(r.Pass());
        CHECK(r.fake.Calls("IsDepthView") == 0);
        Rig q;
        CHECK(q.Pass());
        CHECK(q.fake.Calls("IsDepthView") == 2); // srv and srgb
        CHECK(q.Pass());
        q.fake.SetDepth(kOther, kOtherSrgb);
        CHECK(q.Pass());
        CHECK(q.fake.Calls("IsDepthView") == 2);
    }
}

// ---- decoys: variables with the exact name of a depth input that do not read ReShade's DEPTH semantic ----------------------
// The bind lands in the semantic entry, but such a variable keeps showing its own binding, so nothing reads the copy back and
// the pass cannot be verified. It is undone with an ERASE of the entry, never with what the decoy showed (that may be a texture
// of an effect, which ReShade frees at the next reload while the entry still pointed at it); the runtime is latched until its
// effects reload, and the status says the depth input is not trusted.
void DecoyShapes()
{
    auto lastUpdateIsErase = [](const Rig& r)
    {
        const Fake::Update u = r.fake.UpdateAt(r.fake.updates.size() - 1);
        return u.semantic == "DEPTH" && u.srv == 0 && u.srgb == 0;
    };
    { // an empty decoy picked before the real reader
        Rig r;
        r.fake.vars = { { 0x900, "DepthInputTex", "", 0, 0 }, { 0x901, "Real::Depth", "DEPTH", 0, 0 } };
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(r.binder.Status() == kNotTrusted);
        CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(0).srv == kCopy && lastUpdateIsErase(r));
        CHECK(!r.fake.hasDepth && !r.fake.MapHoldsCopy());
        CHECK(r.Clean());
        CHECK(r.binder.GetCounters().noDepthInput == 1 && r.binder.GetCounters().passesBound == 0);
        // the latch holds: later begins bind nothing and record no new consumer
        r.Finish();
        const size_t updates = r.fake.updates.size();
        const int64_t seen = r.binder.GetConsumer(r.fake.now).seenMs;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::NoDepthInput && r.binder.Status() == kNotTrusted);
        CHECK(r.fake.updates.size() == updates && r.binder.GetConsumer(r.fake.now).seenMs == seen);
        CHECK(r.binder.GetCounters().noDepthInput == 2);
        // ... until the effects reload and a real reader is there
        r.fake.vars = { DepthVar() };
        r.fake.SetDepth(kOrig, kOrigSrgb);
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.Pass() && r.Clean());
    }
    { // a decoy that shows an effect-internal depth-family view: the entry ends EMPTY and never holds that view
        Rig r;
        r.fake.vars = { { 0x900, "DepthBufferTex", "", kOther, kOtherSrgb }, { 0x901, "Real::Depth", "DEPTH", 0, 0 } };
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(r.binder.Status() == kNotTrusted);
        CHECK(!r.fake.hasDepth && lastUpdateIsErase(r));
        for (const auto& u : r.fake.updates)
            CHECK(u.srv != kOther && u.srgb != kOtherSrgb); // the decoy's view was never written into ReShade's map
        CHECK(r.fake.CoreRefs(kOther) == 0 && r.fake.CoreRefs(kOtherSrgb) == 0);
        // the decoy's texture dies with the next effect reload
        r.fake.OwnerRelease(kOther);
        r.fake.OwnerRelease(kOtherSrgb);
        CHECK(!r.fake.DepthEntryDead() && r.fake.violations == 0 && r.Clean());
        r.Finish();
        CHECK(r.Clean());
    }
    for (int half = 0; half < 2; ++half) // a half pair after the bind does not verify: only a readback of (copy, copy) does
    {
        Rig r;
        r.fake.vars = { { 0x900, "DepthInputTex", "", kOther, kOtherSrgb } };
        bool done = false;
        r.fake.afterCall = [&](const char* name)
        {
            if (!done && std::string(name) == "UpdateTextureBindings")
            {
                done = true; // as if the variable showed the copy in one slot
                r.fake.vars[0].srv = half == 0 ? kCopy : kOther;
                r.fake.vars[0].srgb = half == 0 ? kOther : kCopy;
            }
        };
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(r.binder.Status() == kNotTrusted && !r.fake.hasDepth && r.fake.updates.size() == 2);
        CHECK(r.fake.CoreRefs(kCopy) == 0);
    }
    { // the erase cannot be done: the pass stays dirty with the copy referenced, and the next begin does it first
        Rig r;
        r.fake.vars = { { 0x900, "DepthInputTex", "", 0, 0 }, { 0x901, "Real::Depth", "DEPTH", 0, 0 } };
        r.fake.failRestore = true; // an update that does not bind a copy fails
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(r.binder.Status() == kFailed);
        CHECK(r.fake.MapHoldsCopy() && r.fake.BoundCopiesReferenced() && r.fake.CoreRefs(kCopy) == 1);
        r.Finish(); // a stray finish retries, and fails again
        CHECK(r.fake.MapHoldsCopy() && r.fake.CoreRefs(kCopy) == 1);
        // the owner replaces the copy and frees the old one: the map still holds it, so it must stay alive
        r.copyView = kCopy2;
        r.fake.OwnerRelease(kCopy);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Failed); // nothing is bound over it
        CHECK(!r.fake.IsFreed(kCopy) && r.fake.violations == 0);
        r.fake.failRestore = false;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::NoDepthInput); // the retry erased the entry, the latch holds
        CHECK(!r.fake.MapHoldsCopy() && r.fake.IsFreed(kCopy) && r.Clean());
    }
    { // a dirty runtime, then a reload whose only named variable is a decoy: the entry is erased, not trusted to the decoy
        Rig r;
        r.fake.failRestore = true;
        r.Begin();
        r.Finish();
        CHECK(r.fake.MapHoldsCopy());
        r.fake.failRestore = false;
        r.fake.vars = { { 0x900, "DepthInputTex", "", kColor, kColor } };
        r.binder.OnReloadedEffects(kRt);
        CHECK(!r.fake.hasDepth && !r.fake.MapHoldsCopy() && lastUpdateIsErase(r));
        CHECK(r.Clean());
    }
    { // ... and so is a retry at the next begin
        Rig r;
        r.fake.failRestore = true;
        r.Begin();
        r.Finish();
        r.fake.failRestore = false;
        r.fake.vars = { { 0x900, "DepthBufferTex", "", kOther, kOtherSrgb } };
        r.NextFrame();
        CHECK(r.Begin() == Outcome::NoDepthInput); // the decoy is a candidate now; the erase comes first, and the latch is new
        CHECK(!r.fake.MapHoldsCopy());
        for (const auto& u : r.fake.updates)
            CHECK(u.srv != kOther);
        CHECK(r.Clean());
    }
    { // retry/reload: the real reader is the second candidate; the first one is a decoy that does not show the copy
        Rig r;
        r.fake.vars = { { 0x900, "ReShade::DepthBufferTex", "", kOther, kOtherSrgb }, { 0x901, "DepthInputTex", "DEPTH", 0, 0 } };
        r.fake.failRestore = true;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.MapHoldsCopy());
        r.fake.failRestore = false;
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb)); // vouched for by the second candidate: the original, not an erase
        CHECK(r.Clean());
    }
    { // retry/reload: a candidate that cannot be read leaves the runtime dirty, and a later reload repairs it
        Rig r;
        r.fake.vars = { { 0x900, "ReShade::DepthBufferTex", "", kOther, kOtherSrgb }, { 0x901, "DepthInputTex", "DEPTH", 0, 0 } };
        r.fake.failRestore = true;
        r.Begin();
        r.Finish();
        r.fake.failRestore = false;
        r.fake.calls = 0;
        r.fake.faultAt = 2; // call 1 lists the variables, call 2 reads the first candidate
        r.fake.fault = Fault::Fail;
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.MapHoldsCopy() && r.fake.BoundCopiesReferenced() && r.fake.CoreRefs(kCopy) == 1);
        r.fake.faultAt = 0;
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // the decoy has the better name: the candidate that reads the copy back is the DEPTH reader, and its own read is the original
        Rig r;
        r.fake.vars = { { 0x900, "ReShade::DepthBufferTex", "", kOther, kOtherSrgb }, { 0x901, "DepthInputTex", "DEPTH", 0, 0 } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        CHECK(r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kOrigSrgb) == 1 && r.fake.CoreRefs(kOther) == 0);
        r.Finish();
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.fake.StateKey() == before && r.Clean());
        CHECK(r.binder.GetCounters().noDepthInput == 0 && r.binder.GetCounters().passesBound == 1);
    }
    { // a candidate that already shows the copy is a bind that was never put back, whichever candidate it is
        Rig r;
        r.fake.vars = { DepthVar(), { 0x900, "DepthBufferTex", "", kCopy, kCopy } };
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.fake.updates.empty() && r.fake.CoreRefs(kCopy) == 0 && r.fake.Balanced());
    }
}

// N3: the state "the binding holds the current copy and the core owns no reference to it" is unreachable. Every shape that could
// lead there (decoys, partial pairs, restores that fail, reloads) is run with a fault at every backend call; whatever the fault,
// the copy is never in ReShade's map without a reference of ours, no dead view is touched, and once the effects reload a begin
// never ends in the "Failed" that this state would give.
void StuckCopyIsUnreachable()
{
    struct Shape
    {
        const char* what;
        void (*setup)(Rig&);
        void (*run)(Rig&);
    };
    const Shape shapes[] = {
        { "empty decoy", [](Rig& r) { r.fake.vars = { { 0x900, "DepthInputTex", "", 0, 0 }, { 0x901, "Real::Depth", "DEPTH", 0, 0 } }; },
          [](Rig& r)
          {
              r.Begin();
              r.Finish();
          } },
        { "internal view decoy",
          [](Rig& r) { r.fake.vars = { { 0x900, "DepthBufferTex", "", kOther, kOtherSrgb }, { 0x901, "Real::Depth", "DEPTH", 0, 0 } }; },
          [](Rig& r)
          {
              r.Begin();
              r.Finish();
          } },
        { "decoy with the better name",
          [](Rig& r) { r.fake.vars = { { 0x900, "ReShade::DepthBufferTex", "", kOther, kOtherSrgb }, { 0x901, "DepthInputTex", "DEPTH", 0, 0 } }; },
          [](Rig& r)
          {
              r.Begin();
              r.Finish();
          } },
        { "partial pair",
          [](Rig&) {},
          [](Rig& r)
          {
              if (r.Begin() == Outcome::Bound) // the foreign update only exists if there is a pass to interfere with
                  r.fake.SetDepth(kCopy, kOtherSrgb);
              r.Finish();
          } },
        { "erase that fails, then retried",
          [](Rig& r)
          {
              r.fake.vars = { { 0x900, "DepthInputTex", "", 0, 0 }, { 0x901, "Real::Depth", "DEPTH", 0, 0 } };
              r.fake.failRestore = true;
          },
          [](Rig& r)
          {
              r.Begin();
              r.Finish();
              r.fake.failRestore = false;
              r.NextFrame();
              r.Begin();
          } },
        { "dirty runtime, reload with a decoy",
          [](Rig& r) { r.fake.failRestore = true; },
          [](Rig& r)
          {
              r.Begin();
              r.Finish();
              r.fake.failRestore = false;
              r.fake.vars = { { 0x900, "DepthInputTex", "", kColor, kColor } };
              r.binder.OnReloadedEffects(kRt);
          } },
        { "restore that fails, owner recreates the copy",
          [](Rig& r) { r.fake.failRestore = true; },
          [](Rig& r)
          {
              r.Begin();
              r.Finish();
              r.copyView = kCopy2;
              r.fake.OwnerRelease(kCopy);
              r.NextFrame();
              r.Begin();
              r.fake.failRestore = false;
              r.NextFrame();
              r.Begin();
              r.Finish();
          } },
    };
    const Fault faults[] = { Fault::Throw, Fault::ThrowAfter, Fault::Fail };
    for (const Shape& shape : shapes)
    {
        int total = 0;
        {
            Rig r;
            shape.setup(r);
            r.fake.calls = 0;
            shape.run(r);
            total = r.fake.calls;
        }
        CHECK(total > 5);
        for (int n = 1; n <= total; ++n)
            for (const Fault fault : faults)
            {
                Rig r;
                shape.setup(r);
                r.fake.calls = 0;
                r.fake.faultAt = n;
                r.fake.fault = fault;
                bool escaped = false;
                try
                {
                    shape.run(r);
                }
                catch (...)
                {
                    escaped = true;
                }
                // never in the map without a reference of ours, and nothing dead
                const bool covered = !r.fake.MapHoldsCopy() || r.fake.BoundCopiesReferenced();
                const bool alive = !r.fake.DepthEntryDead() && r.fake.violations == 0;
                // with the fault gone and the effects reloaded (which repairs a dirty runtime and ends a latch)
                r.fake.faultAt = 0;
                r.fake.fault = Fault::None;
                r.fake.failRestore = false;
                r.binder.OnReloadedEffects(kRt);
                r.NextFrame();
                const Outcome outcome = r.Begin();
                r.Finish();
                const bool settled = !r.fake.HoldsCopy() && r.fake.CopyRefs() == 0 && !r.fake.DepthEntryDead() && r.fake.violations == 0;
                if (escaped || !covered || !alive || outcome == Outcome::Failed || !settled)
                    std::printf("  stuck copy: '%s' call #%d fault %d: escaped=%d covered=%d alive=%d outcome=%d settled=%d\n", shape.what, n,
                                int(fault), escaped, covered, alive, int(outcome), settled);
                CHECK(!escaped && covered && alive);
                CHECK(outcome != Outcome::Failed);
                CHECK(settled);
                r.binder.OnDestroyRuntime(kRt);
                CHECK(r.fake.Balanced());
            }
    }
}

void FailedCounter()
{
    { // every Outcome::Failed
        Rig r;
        r.fake.definitionError = true;
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.binder.GetCounters().failed == 1);
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.binder.GetCounters().failed == 2);
    }
    { // a restore left pending counts when it goes pending, and every refused begin counts
        Rig r;
        r.fake.failRestore = true;
        r.Begin();
        r.Finish();
        CHECK(r.binder.GetCounters().failed == 1);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Failed); // the retry fails again (1) and the begin is refused (1)
        CHECK(r.binder.GetCounters().failed == 3);
    }
    { // the stuck copy answer
        Rig r;
        r.fake.SetDepth(kCopy, kCopy);
        CHECK(r.Begin() == Outcome::Failed && r.binder.GetCounters().failed == 1);
    }
    { // the other counters do not move
        Rig r;
        r.fake.vars.clear();
        r.Begin();
        CHECK(r.binder.GetCounters().failed == 0);
    }
}

// ---- manual depth transform -----------------------------------------------------------------------------------------------
void ManualTransform()
{
    const char* const scales[] = { "RESHADE_DEPTH_INPUT_X_SCALE", "RESHADE_DEPTH_INPUT_Y_SCALE" };
    const char* const offsets[] = { "RESHADE_DEPTH_INPUT_X_OFFSET", "RESHADE_DEPTH_INPUT_Y_OFFSET",
                                    "RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET" };
    auto outcomeWith = [](const char* macro, const char* value, const char* effect = nullptr)
    {
        Rig r;
        if (effect == nullptr)
            r.fake.globalDefs[macro] = value;
        else
            r.fake.effectDefs[effect][macro] = value;
        const Outcome o = r.Begin();
        r.Finish();
        CHECK(r.Clean());
        return o;
    };
    auto expectOutcome = [&](const char* macro, const char* value, Outcome want)
    {
        const Outcome got = outcomeWith(macro, value);
        if (got != want)
            std::printf("  %s = '%s': %s\n", macro, value, want == Outcome::Bound ? "should be neutral" : "should stand down");
        CHECK(got == want);
    };
    for (const char* m : scales)
    {
        for (const char* v : { "1", "1.0", "1.0f", "1.0F", " 1 ", "\t1.00\t", "1.", "1e0", "10e-1" })
            expectOutcome(m, v, Outcome::Bound);
        for (const char* v : { "0", "2", "0.5", "1.0001", "abc", "", " ", "1 2", "1.0x", "1,0", "+1", "inf", "-inf", "nan", "0x1", "1ff",
                               "f", "(1.0)", "1.0 f", "-1", "1e1" })
            expectOutcome(m, v, Outcome::ManualTransform);
    }
    for (const char* m : offsets)
    {
        for (const char* v : { "0", "0.0", "0.0f", "-0", "-0.0", " 0 ", "0e5", "000" })
            expectOutcome(m, v, Outcome::Bound);
        for (const char* v : { "1", "0.001", "-0.5", "abc", "", "0 0", "inf", "nan", "1.0f" })
            expectOutcome(m, v, Outcome::ManualTransform);
    }

    // per effect: only effects with an enabled technique count, and any one non-neutral value ends it
    for (const char* m : scales)
        CHECK(outcomeWith(m, "2", "Main.fx") == Outcome::ManualTransform);
    for (const char* m : offsets)
        CHECK(outcomeWith(m, "4", "Main.fx") == Outcome::ManualTransform);
    CHECK(outcomeWith("RESHADE_DEPTH_INPUT_X_SCALE", "1.0", "Main.fx") == Outcome::Bound);
    {
        Rig r; // a disabled technique's effect is ignored
        r.fake.techniques = { { "Main.fx", true }, { "Off.fx", false } };
        r.fake.effectDefs["Off.fx"]["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }
    {
        Rig r; // one enabled technique among disabled ones of the same effect is enough
        r.fake.techniques = { { "Mixed.fx", false }, { "Mixed.fx", true } };
        r.fake.effectDefs["Mixed.fx"]["RESHADE_DEPTH_INPUT_Y_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::ManualTransform);
    }
    {
        Rig r; // several enabled techniques of one effect: the effect is asked once
        r.fake.techniques = { { "Dup.fx", true }, { "Dup.fx", true }, { "Dup.fx", true } };
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.defQueries == 12); // 6 macros globally, 6 for Dup.fx
        r.Finish();
    }
    {
        Rig r; // the last of several effects
        r.fake.techniques = { { "A.fx", true }, { "B.fx", true }, { "C.fx", true } };
        r.fake.effectDefs["C.fx"]["RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET"] = "1";
        CHECK(r.Begin() == Outcome::ManualTransform);
    }
    {
        Rig r; // no enabled technique at all: only the global scope is left to check
        r.fake.techniques = { { "Main.fx", false } };
        r.fake.effectDefs["Main.fx"]["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        Rig q;
        q.fake.techniques.clear();
        q.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(q.Begin() == Outcome::ManualTransform);
    }
    { // no capture yet: the macros are still looked at
        Rig r;
        r.binder.PublishCapture(DepthCapture {});
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::ManualTransform);
    }
    { // no partial application: a stand-down changes nothing at all
        Rig r;
        const std::string before = r.fake.StateKey();
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_OFFSET"] = "0.1";
        CHECK(r.Begin() == Outcome::ManualTransform);
        CHECK(r.fake.updates.empty() && r.fake.StateKey() == before && r.Clean());
        CHECK(r.binder.GetCounters().manualTransformStandDowns == 1);
    }
    { // a query that fails is neither "manual" nor "neutral" and is not remembered
        Rig r;
        r.fake.techniquesOk = false;
        CHECK(r.Begin() == Outcome::Failed);
        r.fake.techniquesOk = true;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        Rig q;
        q.fake.definitionError = true;
        CHECK(q.Begin() == Outcome::Failed);
        CHECK(q.binder.Status() == kFailed);
        q.fake.definitionError = false;
        q.NextFrame();
        CHECK(q.Begin() == Outcome::Bound);
        q.Finish();
        CHECK(q.Clean());
        Rig g; // an error on a global query stands down, whatever else there is to look at
        g.fake.techniques.clear();
        g.fake.definitionError = true;
        CHECK(g.Begin() == Outcome::Failed);
        Rig e; // an error on a per-effect query, after the global ones were neutral
        e.fake.faultAt = 11; // call 1 is the capture's AddRef, 2-3 the runtime gates, 4-9 the global macros, 10 the techniques
        e.fake.fault = Fault::Fail;
        CHECK(e.Begin() == Outcome::Failed);
        CHECK(e.fake.Calls("Definition") == 7 && e.Clean());
    }
}

void ManualTransformCache()
{
    { // neutral is remembered: no new query until the effects reload
        Rig r;
        CHECK(r.Pass());
        const int queries = r.fake.defQueries;
        CHECK(queries > 0);
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; // a change ReShade only applies after a reload
        CHECK(r.Pass());
        CHECK(r.Pass());
        CHECK(r.fake.defQueries == queries);
        r.binder.OnReloadedEffects(kRt);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform);
        CHECK(r.fake.defQueries > queries);
    }
    { // manual is remembered, too, and a reload sees the fix
        Rig r;
        r.fake.effectDefs["Main.fx"]["RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET"] = "5";
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform);
        const int queries = r.fake.defQueries;
        r.fake.effectDefs.clear();
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform && r.fake.defQueries == queries);
        r.binder.OnReloadedEffects(kRt);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
    }
    { // destroy forgets it
        Rig r;
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_Y_SCALE"] = "3";
        CHECK(r.Pass() == false);
        r.fake.globalDefs.clear();
        r.binder.OnDestroyRuntime(kRt);
        CHECK(r.Pass());
    }
    { // a technique switched on later brings its effect's macros: seen after OnTechniquesChanged, not before
        Rig r;
        r.fake.techniques = { { "Main.fx", true }, { "Late.fx", false } };
        r.fake.effectDefs["Late.fx"]["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(r.Pass());
        r.fake.techniques[1].enabled = true;
        CHECK(r.Pass()); // cached
        r.binder.OnTechniquesChanged(kRt);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform);
        r.fake.techniques[1].enabled = false; // ... and switched off again
        r.binder.OnTechniquesChanged(kRt);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.Clean());
    }
    { // OnTechniquesChanged forgets the macros only: an open pass and the trust stay
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.log.clear();
        r.binder.OnTechniquesChanged(kRt);
        r.binder.OnTechniquesChanged(0x1234); // unknown runtime
        CHECK(r.fake.log.empty());
        r.Finish();
        CHECK(r.Clean());
    }
    { // ... and what was learned about the depth input
        Rig r;
        CHECK(r.Pass());
        const int checks = r.fake.Calls("IsDepthView");
        CHECK(checks == 2);
        const int queries = r.fake.defQueries;
        r.binder.OnTechniquesChanged(kRt);
        CHECK(r.Pass());
        CHECK(r.fake.defQueries > queries && r.fake.Calls("IsDepthView") == checks); // macros asked again, trust kept
    }
    { // global scope is asked first: a global value spares the per-effect queries
        Rig r;
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        r.Begin();
        CHECK(r.fake.defQueries == 1 && r.fake.Calls("Techniques") == 0);
    }
}

// ---- depth input ----------------------------------------------------------------------------------------------------------
void DepthInputLookup()
{
    auto restoredOriginal = [](const char* name, uint64_t& restoredSrv)
    {
        Rig r;
        r.fake.vars[0].name = name;
        const Outcome o = r.Begin();
        r.Finish();
        restoredSrv = r.fake.updates.size() == 2 ? r.fake.UpdateAt(1).srv : 0;
        CHECK(r.Clean());
        return o;
    };
    uint64_t srv = 0;
    for (const char* name : { "ReShade::DepthBufferTex", "DepthBufferTex", "DepthInputTex" })
    {
        CHECK(restoredOriginal(name, srv) == Outcome::Bound);
        CHECK(srv == kOrig);
    }
    for (const char* name : { "MXAODepth", "texDepthMip", "depthbuffertex", "DepthBufferTex2", " DepthBufferTex", "DepthBufferTex ",
                              "ReShade::DepthBufferTexture", "Depth", "ReShade::depthbuffertex", "" })
        CHECK(restoredOriginal(name, srv) == Outcome::NoDepthInput);

    { // an MXAO-only runtime: DepthInputTex and decoys, no DepthBufferTex anywhere
        Rig r;
        r.fake.vars = { { 0x510, "MXAODepth", "", kOther, kOtherSrgb },
                        { 0x511, "texDepthMip", "", kOther, kOtherSrgb },
                        { 0x512, "DepthInputTex", "DEPTH", 0, 0 } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.DepthIs(kCopy, kCopy) && r.fake.vars[0].srv == kOther && r.fake.vars[1].srv == kOther);
        r.Finish();
        CHECK(r.fake.StateKey() == before && r.Clean());
        CHECK(r.fake.bindingReads.count(0x510) == 0 && r.fake.bindingReads.count(0x511) == 0); // decoys are never read
    }
    { // decoys only: nothing is read, saved, bound or restored
        Rig r;
        r.fake.vars = { { 0x510, "MXAODepth", "", kOther, kOtherSrgb }, { 0x511, "texDepthMip", "DEPTH", 0, 0 } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        r.Finish();
        CHECK(r.fake.bindingReads.empty() && r.fake.updates.empty());
        CHECK(r.fake.Calls("AddRef") == 1); // only the capture's copy, taken at the start and released again
        CHECK(r.fake.StateKey() == before && r.Clean());
        CHECK(r.binder.GetCounters().noDepthInput == 1 && r.binder.Status() == kNoInput);
    }
    { // the list order decides which candidate is verified and used, not the enumeration order; every candidate is looked at
      // before the bind, and after it the first one that reads the copy back is the DEPTH reader of the pass
        Rig r;
        r.fake.vars = { { 0x520, "DepthInputTex", "DEPTH", 0, 0 },
                        { 0x521, "DepthBufferTex", "DEPTH", 0, 0 },
                        { 0x522, "ReShade::DepthBufferTex", "DEPTH", 0, 0 } };
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.bindingReads[0x520] == 1 && r.fake.bindingReads[0x521] == 1 && r.fake.bindingReads[0x522] >= 3);
        const int before = r.fake.bindingReads[0x522];
        r.Finish();
        CHECK(r.fake.bindingReads[0x520] == 1 && r.fake.bindingReads[0x521] == 1 && r.fake.bindingReads[0x522] == before + 1);
        Rig q;
        q.fake.vars = { { 0x520, "DepthInputTex", "DEPTH", 0, 0 }, { 0x521, "DepthBufferTex", "DEPTH", 0, 0 } };
        CHECK(q.Begin() == Outcome::Bound);
        const int beforeQ = q.fake.bindingReads[0x521];
        q.Finish();
        CHECK(q.fake.bindingReads[0x520] == 1 && q.fake.bindingReads[0x521] == beforeQ + 1);
    }
    { // a name must match exactly, whatever the enumeration order says: a near miss is never even looked at
        Rig r;
        r.fake.vars = { { 0x530, "DepthInputTexX", "DEPTH", 0, 0 }, { 0x531, "DepthInputTex", "DEPTH", 0, 0 } };
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.bindingReads.count(0x530) == 0 && r.fake.bindingReads.count(0x531) == 1);
        r.Finish();
        CHECK(r.fake.bindingReads.count(0x530) == 0);
        CHECK(r.fake.UpdateAt(1).srv == kOrig);
    }
    { // enumerated every pass; handles are never kept for the next one
        Rig r;
        CHECK(r.Pass());
        CHECK(r.fake.textureEnums == 1);
        r.fake.vars[0].handle = 0x777; // a partial reload gave the variable a new handle (the old one is dangling)
        CHECK(r.Pass());
        CHECK(r.fake.textureEnums == 2);
        CHECK(r.fake.bindingReads.count(0x777) >= 1);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb)); // finish used the new handle, too
        CHECK(r.Clean());
    }
}

void BufReadyDepth()
{
    { // only the exact annotation value; other values are left alone
        Rig r;
        r.fake.unis = { { 0x601, "bufready_depth", false },
                        { 0x602, "bufready_depth_x", false },
                        { 0x603, "BUFREADY_DEPTH", false },
                        { 0x604, "", false },
                        { 0x605, "bufready_color", false },
                        { 0x606, " bufready_depth", false },
                        { 0x607, "bufready_depth", false } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        for (const auto& u : r.fake.unis)
            CHECK(u.value == (u.handle == 0x601 || u.handle == 0x607));
        r.Finish();
        CHECK(r.fake.StateKey() == before && r.Clean());
    }
    { // an old value of true stays true; false goes back to false
        Rig r;
        r.fake.unis = { { 0x601, "bufready_depth", true }, { 0x602, "bufready_depth", false } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.unis[0].value && r.fake.unis[1].value);
        r.Finish();
        CHECK(r.fake.unis[0].value && !r.fake.unis[1].value);
        CHECK(r.fake.StateKey() == before);
    }
    { // no such uniform: the pass still binds
        Rig r;
        r.fake.unis.clear();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.Clean());
    }
    { // enumerated every pass, handles never kept
        Rig r;
        CHECK(r.Pass());
        CHECK(r.fake.uniformEnums == 1);
        r.fake.unis[0].handle = 0x688;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.unis[0].value && r.fake.uniformEnums == 2);
        r.Finish();
        CHECK(!r.fake.unis[0].value);
    }
    { // nothing piles up: one set and one restore per pass
        Rig r;
        for (int i = 0; i < 3; ++i)
            CHECK(r.Pass());
        CHECK(r.fake.Calls("SetUniformBool") == 6);
    }
    { // every refusal leaves the uniforms alone
        Rig r;
        r.fake.vars.clear();
        CHECK(r.Begin() == Outcome::NoDepthInput);
        CHECK(!r.fake.unis[0].value && r.fake.uniformEnums == 0);
    }
}

void EnableDisable()
{
    Rig r(false);
    CHECK(r.binder.Status() == kDisabled);
    r.binder.SetEnabled(true);
    CHECK(r.binder.Status() == kWaiting);
    CHECK(r.Begin() == Outcome::Bound);
    const std::string status = r.binder.Status();
    r.binder.SetEnabled(true); // a repeated "on" (a config poll) does not reset the status
    CHECK(r.binder.Status() == status);

    // switched off in the middle of a pass: finish still puts everything back
    const std::string before = Rig().fake.StateKey();
    r.binder.SetEnabled(false);
    CHECK(r.binder.Status() == kDisabled);
    r.Finish();
    CHECK(r.fake.StateKey() == before && r.Clean());
    CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(1).srv == kOrig);

    r.NextFrame();
    CHECK(r.Begin() == Outcome::Disabled);
    r.binder.SetEnabled(true);

    { // a late failure of an earlier pass does not leave "failed" on a feature that is off: a begin says "disabled" again
        Rig f;
        f.fake.failRestore = true;
        f.Begin();
        f.binder.SetEnabled(false);
        f.Finish();
        CHECK(f.binder.Status() == kFailed);
        f.fake.failRestore = false;
        f.NextFrame();
        CHECK(f.Begin() == Outcome::Disabled);
        CHECK(f.binder.Status() == kDisabled);
        CHECK(f.fake.DepthIs(kOrig, kOrigSrgb) && f.Clean());
    }

    // switched off, then a new begin before the finish arrived: the old pass is put back first
    Rig q;
    CHECK(q.Begin() == Outcome::Bound);
    q.binder.SetEnabled(false);
    q.NextFrame();
    CHECK(q.Begin() == Outcome::Disabled);
    CHECK(q.fake.StateKey() == before && q.Clean());
}

// ---- reload, destroy and restores that fail --------------------------------------------------------------------------------
void ReloadMidPass()
{
    { // the effects reload between begin and finish: the copy is taken out of ReShade's map at the event, through the variable
      // as the new effects know it, and only then are the references given back
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].handle = 0x777;
        r.fake.unis[0].handle = 0x688;
        const int boolCalls = r.fake.Calls("SetUniformBool");
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(1).srgb == kOrigSrgb);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb));
        CHECK(r.Clean());
        CHECK(r.fake.Calls("SetUniformBool") == boolCalls); // the uniforms are new objects: nothing of ours in them
        r.fake.log.clear();
        r.Finish(); // the pass is over
        CHECK(r.fake.log.empty());
        CHECK(r.Pass() && r.Clean());
    }
    { // another add-on took the binding over before the reload. The variables as the new effects know them cannot be vouched
      // for (none reads the copy back), so the entry is erased rather than trusted: the other add-on's pick is lost until it
      // selects again, and the copy is certainly gone. The original is not written back either.
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.SetDepth(kOther, kOtherSrgb);
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.updates.size() == 2 && r.fake.UpdateAt(1).srv == 0 && r.fake.UpdateAt(1).srgb == 0);
        CHECK(!r.fake.hasDepth);
        CHECK(r.binder.GetCounters().overwrittenByOther == 0);
        CHECK(r.Clean());
    }
    { // a reload with nothing open forgets the cache and the latch, and keeps nothing
        Rig r;
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        CHECK(r.Begin() == Outcome::ManualTransform);
        r.fake.globalDefs.clear();
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.Pass() && r.Clean());
    }
}

void ReloadCannotResolve()
{
    { // the variable cannot be found at the event: the copy may still be bound, so both references stay
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars.clear();
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kOrigSrgb) == 1);
        CHECK(r.fake.BoundCopiesReferenced());
        CHECK(r.binder.Status() == kFailed);
        // a later reload with the variable back repairs it
        r.fake.vars.push_back(DepthVar());
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // a reload that leaves the runtime dirty still forgets what was learned about the effects: the macros ...
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars.clear();
        r.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; // the new effects carry a manual transform
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.BoundCopiesReferenced());
        r.fake.vars.push_back(DepthVar());
        r.NextFrame();
        CHECK(r.Begin() == Outcome::ManualTransform); // the cached "neutral" is gone
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean()); // ... and the begin repaired the binding first
    }
    { // ... and whether the depth input could be trusted
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.Calls("IsDepthView") == 2);
        r.fake.vars.clear();
        r.binder.OnReloadedEffects(kRt);
        r.fake.vars.push_back(DepthVar());
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.Clean());
        CHECK(r.fake.Calls("IsDepthView") == 4); // looked at again after the reload
    }
    { // the variables cannot even be listed at the event: the same
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.texturesOk = false;
        r.binder.OnReloadedEffects(kRt);
        CHECK(r.fake.DepthIs(kCopy, kCopy) && r.fake.BoundCopiesReferenced());
        CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kOrigSrgb) == 1);
        r.fake.texturesOk = true;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound); // the next begin repairs it
        r.Finish();
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // ... or a later begin does
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars.clear();
        r.binder.OnReloadedEffects(kRt);
        r.fake.vars.push_back(DepthVar());
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // only a destroy lets go of a dirty runtime without ReShade
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars.clear();
        r.binder.OnReloadedEffects(kRt);
        r.fake.log.clear();
        r.binder.OnDestroyRuntime(kRt);
        for (const auto& l : r.fake.log)
            CHECK(l == "Release");
        CHECK(r.fake.Calls("Release") == 3);
        CHECK(r.fake.Balanced());
        CHECK(r.fake.DepthIs(kCopy, kCopy)); // the runtime and its map are gone with it
    }
}

void FailedRestoreRetry()
{
    { // finish cannot put the original back: dirty, references kept; the next begin retries first and binds on the real original
        Rig r;
        const std::string before = r.fake.StateKey();
        r.fake.failRestore = true;
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.DepthIs(kCopy, kCopy) && r.fake.BoundCopiesReferenced());
        CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kOrigSrgb) == 1);
        CHECK(r.binder.Status() == kFailed);
        CHECK(!r.fake.unis[0].value); // the bools were put back anyway
        CHECK(r.fake.Calls("SetUniformBool") == 2); // forced, restored

        r.fake.failRestore = false;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.Calls("SetUniformBool") == 3); // the retry does not put the bools back a second time
        CHECK(r.fake.updates.size() == 3); // bind, the retried restore, bind
        CHECK(r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(1).srgb == kOrigSrgb && r.fake.UpdateAt(2).srv == kCopy);
        CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kOrigSrgb) == 1);
        r.Finish();
        CHECK(r.fake.updates.size() == 4 && r.fake.StateKey() == before && r.Clean());
    }
    { // a stray finish retries too, through the variable as it is now
        Rig r;
        r.fake.failRestore = true;
        r.Begin();
        r.Finish();
        r.fake.failRestore = false;
        r.fake.vars[0].handle = 0x777;
        r.Finish();
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // ... and a begin of a switched-off feature repairs it as well
        Rig r;
        r.fake.failRestore = true;
        r.Begin();
        r.Finish();
        r.fake.failRestore = false;
        r.binder.SetEnabled(false);
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Disabled);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
    }
    { // a restore that keeps failing: nothing is bound over the stuck copy, nothing is re-saved as an original, and the copy
      // stays alive even when its owner recreates and frees it
        Rig r;
        r.fake.failRestore = true;
        r.Begin();
        r.Finish();
        for (int i = 0; i < 3; ++i)
        {
            r.NextFrame();
            CHECK(r.Begin() == Outcome::Failed);
            CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.CoreRefs(kOrig) == 1);
            CHECK(r.fake.updates.size() == 1);
            r.Finish();
        }
        DepthCapture c = r.Capture();
        c.copyView = kCopy2;
        r.binder.PublishCapture(c);
        r.fake.OwnerRelease(kCopy);
        CHECK(!r.fake.IsFreed(kCopy)); // ReShade still holds it, so the core still does
        CHECK(r.fake.violations == 0 && r.fake.BoundCopiesReferenced());
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.fake.violations == 0);

        r.fake.failRestore = false;
        r.copyView = kCopy2;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(2).srv == kCopy2);
        r.Finish();
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
        CHECK(r.fake.IsFreed(kCopy)); // now it can die
        CHECK(r.fake.violations == 0);
    }
    { // the invariant after a pass ends: nothing of ours stays bound and every reference is back
        Rig r;
        for (int i = 0; i < 3; ++i)
        {
            CHECK(r.Pass());
            CHECK(r.Clean());
        }
    }
}

void RollbackResolvesAgain()
{
    { // a pass that never finished is rolled back at the next begin through the variable and uniforms as they are NOW
        Rig r;
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars[0].handle = 0x777;
        r.fake.unis[0].handle = 0x688;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.updates.size() == 3 && r.fake.UpdateAt(1).srv == kOrig && r.fake.UpdateAt(2).srv == kCopy);
        CHECK(r.fake.unis[0].value);
        CHECK(r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kCopy) == 1); // nothing piled up
        r.Finish();
        CHECK(r.fake.StateKey() == before && r.Clean());
        CHECK(r.binder.GetCounters().overwrittenByOther == 0);
    }
    { // the values go back to the uniform they came from
        Rig r;
        r.fake.unis = { { 0x601, "bufready_depth", true }, { 0x602, "bufready_depth", false } };
        const std::string before = r.fake.StateKey();
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.unis[0].handle = 0x688;
        r.fake.unis[1].handle = 0x689;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        r.Finish();
        CHECK(r.fake.StateKey() == before && r.Clean());
    }
    { // the uniforms are matched by position: another set of them is left alone
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.unis.push_back({ 0x602, "bufready_depth", false });
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.Calls("SetUniformBool") == 3); // 1 forced, none put back, 2 forced
        r.Finish();
        CHECK(r.Clean());
    }
    { // no variable to look up at the next begin: the old pass stays dirty and the begin is refused
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.vars.clear();
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.fake.CoreRefs(kCopy) == 1 && r.fake.BoundCopiesReferenced());
        r.binder.OnDestroyRuntime(kRt);
        CHECK(r.fake.Balanced());
    }
}

void PairingAndRuntimeLifetime()
{
    { // finish with no begin: not one backend call
        Rig r;
        r.Finish();
        CHECK(r.fake.log.empty());
    }
    { // finish twice
        Rig r;
        r.Begin();
        r.Finish();
        const size_t n = r.fake.log.size();
        r.Finish();
        CHECK(r.fake.log.size() == n && r.fake.updates.size() == 2);
    }
    { // destroy with a pass open: references released, ReShade not called (its map is gone), finish is a no-op
        Rig r;
        r.Begin();
        r.fake.log.clear();
        r.binder.OnDestroyRuntime(kRt);
        for (const auto& l : r.fake.log)
            CHECK(l == "Release");
        CHECK(r.fake.Calls("Release") == 3);
        CHECK(r.fake.Balanced());
        CHECK(r.fake.DepthIs(kCopy, kCopy));
        r.fake.log.clear();
        r.Finish();
        CHECK(r.fake.log.empty());
    }
    { // destroy / reload with no pass open: nothing to do
        Rig r;
        r.binder.OnDestroyRuntime(kRt);
        r.binder.OnReloadedEffects(kRt);
        r.binder.OnDestroyRuntime(0x1234);
        CHECK(r.fake.log.empty());
    }
    { // nested begin: the second ends the first and binds again; the first finish closes the second pass, the next is a no-op
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.Begin() == Outcome::Bound);
        CHECK(r.fake.updates.size() == 3); // bind, put back, bind
        CHECK(r.fake.CoreRefs(kOrig) == 1 && r.fake.CoreRefs(kCopy) == 1);
        r.Finish();
        CHECK(r.fake.updates.size() == 4 && r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
        r.fake.log.clear();
        r.Finish();
        CHECK(r.fake.log.empty());
    }
    { // the capture is replaced and its owner frees the old copy during the pass: our reference keeps it alive
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        DepthCapture c = r.Capture();
        c.copyView = kCopy2;
        r.binder.PublishCapture(c);
        r.fake.OwnerRelease(kCopy);
        CHECK(!r.fake.IsFreed(kCopy) && r.fake.DepthIs(kCopy, kCopy));
        r.Finish();
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
        CHECK(r.fake.IsFreed(kCopy) && r.fake.violations == 0);
    }
}

void TwoRuntimes()
{
    Fake a, b; // two runtimes with their own bindings
    Router router(a, b);
    DepthBinder binder(router, &Hook);
    g_logged.clear();
    binder.SetEnabled(true);
    DepthCapture c;
    c.valid = true;
    c.outputW = 3840;
    c.outputH = 2160;
    c.renderW = 1920;
    c.renderH = 1080;
    c.frame = a.frame;
    c.tickMs = a.now;
    c.copyView = kCopy;
    c.device = kDevice;
    binder.PublishCapture(c);

    { // interleaved: begin(A), begin(B), finish(A), finish(B)
        CHECK(binder.OnBeginEffects(kRt) == Outcome::Bound);
        CHECK(binder.OnBeginEffects(kRt2) == Outcome::Bound);
        CHECK(a.DepthIs(kCopy, kCopy) && b.DepthIs(kCopy, kCopy));
        CHECK(a.CoreRefs(kCopy) == 2 && a.CoreRefs(kOrig) == 2);
        binder.OnFinishEffects(kRt);
        CHECK(a.DepthIs(kOrig, kOrigSrgb) && b.DepthIs(kCopy, kCopy));
        binder.OnFinishEffects(kRt2);
        CHECK(a.DepthIs(kOrig, kOrigSrgb) && b.DepthIs(kOrig, kOrigSrgb));
        CHECK(a.Balanced() && !a.HoldsCopy() && !b.HoldsCopy());
    }
    { // begin(A), begin(B), finish(B), finish(A)
        CHECK(binder.OnBeginEffects(kRt) == Outcome::Bound);
        CHECK(binder.OnBeginEffects(kRt2) == Outcome::Bound);
        binder.OnFinishEffects(kRt2);
        binder.OnFinishEffects(kRt);
        CHECK(a.Balanced() && !a.HoldsCopy() && !b.HoldsCopy());
    }
    { // status with two runtimes that disagree: the one that is not ours never writes it
        CHECK(binder.OnBeginEffects(kRt) == Outcome::Bound);
        binder.OnFinishEffects(kRt);
        const std::string status = binder.Status();
        CHECK(status == "Automatic scene depth: 1920x1080 -> 3840x2160");
        b.sameDevice = false;
        CHECK(binder.OnBeginEffects(kRt2) == Outcome::OtherDevice);
        CHECK(binder.Status() == status);
        b.sameDevice = true;
        b.d3d11 = false;
        CHECK(binder.OnBeginEffects(kRt2) == Outcome::NotD3D11);
        CHECK(binder.Status() == status);
        CHECK(binder.GetCounters().otherDevice == 2);
        // ... while one that is ours and has a problem does
        b.d3d11 = true;
        b.vars.clear();
        CHECK(binder.OnBeginEffects(kRt2) == Outcome::NoDepthInput);
        CHECK(binder.Status() == kNoInput);
    }
    // destroying one runtime leaves the other alone
    CHECK(binder.OnBeginEffects(kRt) == Outcome::Bound);
    binder.OnDestroyRuntime(kRt2);
    CHECK(a.DepthIs(kCopy, kCopy));
    binder.OnFinishEffects(kRt);
    CHECK(a.Balanced() && !a.HoldsCopy());
}

// ---- failures anywhere ----------------------------------------------------------------------------------------------------
std::vector<std::string> CleanSequence(int& beginCalls, int& totalCalls)
{
    Rig r;
    r.Begin();
    beginCalls = r.fake.calls;
    r.Finish();
    totalCalls = r.fake.calls;
    return r.fake.log;
}

// A fault anywhere in a begin or finish: nothing escapes, no dead view is touched, the pass is undone or its pending restore
// is covered by references, and one clean pass afterwards puts everything right. A destroy always ends in a balance.
void FaultInjection()
{
    int beginCalls = 0, totalCalls = 0;
    const std::vector<std::string> sequence = CleanSequence(beginCalls, totalCalls);
    CHECK(beginCalls > 10 && totalCalls > beginCalls);

    const char* const names[] = { "throws", "throws after its effect", "fails" };
    const Fault faults[] = { Fault::Throw, Fault::ThrowAfter, Fault::Fail };
    for (int n = 1; n <= totalCalls; ++n)
        for (int f = 0; f < 3; ++f)
        {
            Rig r;
            const std::string before = r.fake.StateKey();
            const std::string beforeNoDepth = r.fake.StateKeyNoDepth();
            r.fake.faultAt = n;
            r.fake.fault = faults[f];
            bool escaped = false, cleanAfterBegin = true;
            Outcome outcome = Outcome::Failed;
            try
            {
                outcome = r.Begin();
                cleanAfterBegin = r.Clean();
                r.Finish();
            }
            catch (...)
            {
                escaped = true;
            }
            const bool inBegin = n <= beginCalls;
            const bool clean = r.Clean();
            const bool pendingCovered = r.fake.BoundCopiesReferenced();
            // a begin that did not bind has already put everything back, without waiting for finish_effects
            const bool undone = outcome == Outcome::Bound || cleanAfterBegin;
            // A bind that was not verified yet when the fault hit is undone with an erase: the entry is then empty, never
            // anything else, and the rest of ReShade's state is as it was.
            const bool asBefore =
                r.fake.StateKey() == before || (r.fake.StateKeyNoDepth() == beforeNoDepth && !r.fake.hasDepth);
            const bool ok = !escaped && r.fake.violations == 0 && (clean || pendingCovered) && undone && (!inBegin || (clean && asBefore));
            if (!ok)
                std::printf("  fault: call #%d (%s) %s: escaped=%d violations=%d clean=%d covered=%d undone=%d\n", n,
                            sequence[n - 1].c_str(), names[f], escaped, int(r.fake.violations), clean, pendingCovered, undone);
            CHECK(ok);

            // one clean pass afterwards recovers everything (a latched "untrusted" ends with a reload)
            r.fake.faultAt = 0;
            r.fake.fault = Fault::None;
            bool bound = r.Pass();
            const bool latched = f == 2 && sequence[n - 1] == "IsDepthView";
            if (!bound && latched)
            {
                r.binder.OnReloadedEffects(kRt);
                bound = r.Pass();
            }
            const bool recovered = bound && r.Clean() && (r.fake.DepthIs(kOrig, kOrigSrgb) || !r.fake.hasDepth);
            if (!recovered)
                std::printf("  fault: call #%d (%s) %s: no recovery (bound=%d clean=%d)\n", n, sequence[n - 1].c_str(), names[f], bound,
                            r.Clean());
            CHECK(recovered);

            // a destroy after the same fault always balances
            Rig d;
            d.fake.faultAt = n;
            d.fake.fault = faults[f];
            try
            {
                d.Begin();
                d.Finish();
            }
            catch (...)
            {
                escaped = true;
            }
            d.fake.faultAt = 0;
            d.fake.fault = Fault::None;
            d.binder.OnDestroyRuntime(kRt);
            CHECK(!escaped);
            CHECK(d.fake.Balanced());
        }
    for (int release = 1; release <= 3; ++release) // a Release that throws must not keep the others from running
    {
        Rig d;
        d.Begin();
        d.fake.calls = 0;
        d.fake.faultAt = release;
        d.fake.fault = Fault::ThrowAfter;
        bool escaped = false;
        try
        {
            d.binder.OnDestroyRuntime(kRt);
        }
        catch (...)
        {
            escaped = true;
        }
        CHECK(!escaped);
        CHECK(d.fake.Balanced());
    }
}

// The first call of each kind in a begin fails (returns false / 0 / Error) or throws: the outcome says what happened and ReShade
// is left exactly as it was. The calls of a finish are covered next.
void BackendFailures()
{
    int beginCalls = 0, totalCalls = 0;
    const std::vector<std::string> sequence = CleanSequence(beginCalls, totalCalls);
    auto nthCall = [&](const std::string& method, int from, int nth = 1) // 1-based call number of the nth `method` call after `from`
    {
        for (size_t i = size_t(from); i < sequence.size(); ++i)
            if (sequence[i] == method && --nth == 0)
                return int(i) + 1;
        return 0;
    };
    struct Case
    {
        const char* method;
        int occurrence;
        Outcome whenItFails; // what a false / 0 / Error return leads to
        bool mayErase;       // the bind may have landed without being verified: the entry is then erased, not restored
    };
    const Case cases[] = {
        { "IsD3D11", 1, Outcome::NotD3D11, false },
        { "SameDevice", 1, Outcome::OtherDevice, false },
        { "Definition", 1, Outcome::Failed, false },
        { "Techniques", 1, Outcome::Failed, false },
        { "TextureVariables", 1, Outcome::Failed, false },
        { "TextureBinding", 1, Outcome::Failed, false }, // the first sight of the variable
        { "IsDepthView", 1, Outcome::NoDepthInput, false },
        { "BackBufferSize", 1, Outcome::Failed, false },
        { "NowMs", 1, Outcome::CaptureUnavailable, false },
        { "Frame", 1, Outcome::CaptureUnavailable, false },
        { "TextureBinding", 2, Outcome::Failed, false }, // the read before the bind
        { "UpdateTextureBindings", 1, Outcome::Failed, true },
        { "TextureBinding", 3, Outcome::Failed, true }, // the read after the bind: a failed read is no verdict
        { "UniformVariables", 1, Outcome::Failed, false },
        { "GetUniformBool", 1, Outcome::Failed, false },
        { "SetUniformBool", 1, Outcome::Failed, false },
    };
    for (const Case& c : cases)
        for (const Fault fault : { Fault::Fail, Fault::Throw })
        {
            const int n = nthCall(c.method, 0, c.occurrence);
            CHECK(n > 0 && n <= beginCalls);
            Rig r;
            const std::string before = r.fake.StateKey();
            const std::string beforeNoDepth = r.fake.StateKeyNoDepth();
            r.fake.faultAt = n;
            r.fake.fault = fault;
            const Outcome expected = fault == Fault::Fail ? c.whenItFails : Outcome::Failed;
            const Outcome got = r.Begin();
            if (got != expected)
                std::printf("  %s #%d %s: outcome %d, expected %d\n", c.method, c.occurrence, fault == Fault::Fail ? "fails" : "throws",
                            int(got), int(expected));
            CHECK(got == expected);
            // A bind that may have landed without a reader that showed the copy is undone with an erase; every other failure
            // leaves ReShade exactly as it was.
            const bool latched = std::string(c.method) == "IsDepthView";
            CHECK(r.Clean());
            CHECK(r.fake.StateKey() == before || (c.mayErase && r.fake.StateKeyNoDepth() == beforeNoDepth && !r.fake.hasDepth));
            CHECK(r.binder.GetCounters().passesBound == 0);
            r.Finish();
            CHECK(r.Clean());
            CHECK(r.fake.StateKey() == before || (c.mayErase && r.fake.StateKeyNoDepth() == beforeNoDepth && !r.fake.hasDepth));
            // a failed call is no verdict about the variable: the next pass binds (the IsDepthView answer "no" is one)
            if (!latched && expected == Outcome::Failed)
            {
                r.fake.faultAt = 0;
                CHECK(r.Pass() && r.Clean() && (r.fake.DepthIs(kOrig, kOrigSrgb) || (c.mayErase && !r.fake.hasDepth)));
            }
        }
    { // AddRef cannot fail, but a throwing one is survived
        Rig r;
        r.fake.faultAt = nthCall("AddRef", 0);
        r.fake.fault = Fault::Throw;
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.Clean());
    }
    { // the reference to the capture's copy is given back on every refusal, even when that Release throws
        struct Refusal
        {
            void (*breakIt)(Rig&);
        };
        const Refusal refusals[] = {
            { [](Rig& q) { q.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; } },
            { [](Rig& q) { q.fake.bbW = 100; } },
            { [](Rig& q) { q.fake.vars.clear(); } },
            { [](Rig& q) { q.fake.now += 300; } },
        };
        for (const Refusal& f : refusals)
        {
            int release = 0;
            {
                Rig q;
                f.breakIt(q);
                q.Begin();
                for (size_t i = 0; i < q.fake.log.size(); ++i)
                    if (q.fake.log[i] == "Release")
                        release = int(i) + 1;
            }
            CHECK(release > 0);
            Rig r;
            f.breakIt(r);
            r.fake.faultAt = release;
            r.fake.fault = Fault::ThrowAfter;
            CHECK(r.Begin() != Outcome::Bound); // noexcept: an escaping exception would end the program
            CHECK(r.Clean());
        }
    }
    { // a pass that never finished is rolled back at the next begin; looking up its uniforms may throw
        Rig r;
        CHECK(r.Begin() == Outcome::Bound);
        r.fake.calls = 0;
        r.fake.faultAt = 1; // the first call of the next begin is the rollback's uniform lookup
        r.fake.fault = Fault::Throw;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Bound); // the bools are skipped, the binding is still put back, the new pass binds
        r.Finish();
        CHECK(r.Clean() && r.fake.DepthIs(kOrig, kOrigSrgb));
    }
    { // a begin that fails before it bound anything has nothing to put back
        int firstRead = 0;
        {
            Rig q;
            CHECK(q.Pass());
            q.fake.log.clear();
            q.NextFrame();
            q.Begin();
            for (size_t i = 0; i < q.fake.log.size() && firstRead == 0; ++i)
                if (q.fake.log[i] == "TextureBinding") // the read of the binding before the bind
                    firstRead = int(i) + 1;
        }
        CHECK(firstRead > 0);
        Rig r;
        CHECK(r.Pass());
        r.fake.log.clear();
        r.fake.calls = 0;
        r.fake.faultAt = firstRead;
        r.fake.fault = Fault::Throw;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.fake.Calls("UpdateTextureBindings") == 0);
        CHECK(r.Clean() && r.fake.DepthIs(kOrig, kOrigSrgb));
    }
    { // the AddRef of the original throws after the bind landed and was verified: the original goes back, not an erase
        int secondAddRef = 0;
        {
            Rig q;
            CHECK(q.Pass());
            q.fake.log.clear();
            q.NextFrame();
            q.Begin();
            int seen = 0;
            for (size_t i = 0; i < q.fake.log.size(); ++i)
                if (q.fake.log[i] == "AddRef" && ++seen == 2) // the first is the capture's copy, the second the original srv
                    secondAddRef = int(i) + 1;
        }
        CHECK(secondAddRef > 0);
        Rig r;
        CHECK(r.Pass());
        r.fake.log.clear();
        r.fake.calls = 0;
        r.fake.faultAt = secondAddRef;
        r.fake.fault = Fault::Throw;
        r.NextFrame();
        CHECK(r.Begin() == Outcome::Failed);
        CHECK(r.fake.DepthIs(kOrig, kOrigSrgb) && r.Clean());
        CHECK(r.binder.GetCounters().failed == 1);
    }

    // The calls of a finish. A restore that cannot be verified or done leaves the runtime dirty with its references; the bools
    // are a separate step and never keep the binding from being put back.
    struct FinishCase
    {
        const char* method;
        bool dirty;
    };
    const FinishCase finishCases[] = { { "TextureBinding", true }, { "UpdateTextureBindings", true }, { "SetUniformBool", false } };
    for (const FinishCase& c : finishCases)
        for (const Fault fault : { Fault::Fail, Fault::Throw, Fault::ThrowAfter })
        {
            const int n = nthCall(c.method, beginCalls);
            CHECK(n > beginCalls);
            Rig r;
            r.fake.faultAt = n;
            r.fake.fault = fault;
            r.Begin();
            bool escaped = false;
            try
            {
                r.Finish();
            }
            catch (...)
            {
                escaped = true;
            }
            CHECK(!escaped && r.fake.violations == 0);
            // a throw after the restore went through is still an unverified restore
            const bool dirtyAfter = !r.Clean() && r.fake.BoundCopiesReferenced();
            CHECK(dirtyAfter == c.dirty);
            CHECK(r.binder.Status() == kFailed); // a failed restore step, of the binding or of a bool, is reported
            r.fake.faultAt = 0;
            r.fake.fault = Fault::None;
            // A restore that landed and then threw is pending; the retry finds no variable that shows the copy, so it cannot
            // tell that the original is already back and erases the entry instead.
            const bool landedAndThrew = fault == Fault::ThrowAfter && std::string(c.method) == "UpdateTextureBindings";
            CHECK(r.Pass() && r.Clean() && (r.fake.DepthIs(kOrig, kOrigSrgb) || (landedAndThrew && !r.fake.hasDepth)));
            Rig d;
            d.fake.faultAt = n;
            d.fake.fault = fault;
            d.Begin();
            d.Finish();
            d.binder.OnDestroyRuntime(kRt);
            CHECK(d.fake.Balanced());
        }
}

// ---- counters, log hook, allocation, threads -------------------------------------------------------------------------------
void LogHook()
{
    Rig r;
    r.fake.vars.clear();
    for (int i = 0; i < 2001; ++i)
    {
        r.NextFrame();
        r.Begin();
    }
    CHECK(r.binder.GetCounters().noDepthInput == 2001);
    CHECK(g_logged.size() == 3);
    if (g_logged.size() == 3)
        CHECK(g_logged[0] == std::make_pair(Counter::NoDepthInput, uint64_t(1)) &&
              g_logged[1] == std::make_pair(Counter::NoDepthInput, uint64_t(1000)) &&
              g_logged[2] == std::make_pair(Counter::NoDepthInput, uint64_t(2000)));

    { // the counters that tick every frame (while ReShade runs without an evaluate, for a runtime that is not ours, per pass) log
      // their first occurrence only; one that signals a real problem keeps its every 1000th line
        struct Chatty
        {
            Counter counter;
            size_t logged;
            void (*breakIt)(Rig&);
        };
        const Chatty chatty[] = {
            { Counter::StaleCaptures, 1, [](Rig& q) { q.fake.now += 300; } },
            { Counter::OtherDevice, 1, [](Rig& q) { q.fake.sameDevice = false; } },
            { Counter::Failed, 3, [](Rig& q) { q.fake.definitionError = true; } },
            { Counter::SizeMismatches, 3, [](Rig& q) { q.fake.bbW = 1; } },
        };
        for (const Chatty& c : chatty)
        {
            Rig q;
            c.breakIt(q);
            for (int i = 0; i < 2001; ++i)
                q.Begin();
            CHECK(g_logged.size() == c.logged);
            if (g_logged.size() >= 1)
                CHECK(g_logged[0].first == c.counter && g_logged[0].second == 1);
        }
        Rig b; // passesBound
        for (int i = 0; i < 2001; ++i)
        {
            b.NextFrame();
            b.Begin();
            b.Finish();
        }
        CHECK(b.binder.GetCounters().passesBound == 2001);
        CHECK(g_logged.size() == 1 && g_logged[0] == std::make_pair(Counter::PassesBound, uint64_t(1)));
    }

    // every counter reaches the hook on its first increment, with its own name
    struct Case
    {
        Counter counter;
        void (*breakIt)(Rig&);
    };
    const Case cases[] = {
        { Counter::SizeMismatches, [](Rig& q) { q.fake.bbW = 1; } },
        { Counter::ManualTransformStandDowns, [](Rig& q) { q.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2"; } },
        { Counter::StaleCaptures, [](Rig& q) { q.fake.now += 300; } },
        { Counter::OtherDevice, [](Rig& q) { q.fake.sameDevice = false; } },
        { Counter::NoDepthInput, [](Rig& q) { q.fake.vars.clear(); } },
        { Counter::Failed, [](Rig& q) { q.fake.definitionError = true; } },
    };
    for (const Case& c : cases)
    {
        Rig q;
        c.breakIt(q);
        q.Begin();
        CHECK(g_logged.size() == 1 && g_logged[0].first == c.counter && g_logged[0].second == 1);
    }

    Rig p; // overwrittenByOther and passesBound too
    p.Begin();
    p.fake.SetDepth(kOther, kOtherSrgb);
    p.Finish();
    CHECK(g_logged.size() == 2 && g_logged[0].first == Counter::PassesBound && g_logged[1].first == Counter::OverwrittenByOther);
}

void ThrowingHook()
{
    // a log hook that throws neither changes the outcome nor leaves a pass half done
    Fake fake;
    DepthBinder binder(fake, [](Counter, uint64_t) { throw std::runtime_error("log"); });
    binder.SetEnabled(true);
    DepthCapture c;
    c.valid = true;
    c.outputW = 3840;
    c.outputH = 2160;
    c.renderW = 1920;
    c.renderH = 1080;
    c.frame = fake.frame;
    c.tickMs = fake.now;
    c.copyView = kCopy;
    c.device = kDevice;
    binder.PublishCapture(c);
    const std::string before = fake.StateKey();
    CHECK(binder.OnBeginEffects(kRt) == Outcome::Bound); // passesBound reaches the hook
    binder.OnFinishEffects(kRt);
    CHECK(fake.StateKey() == before && fake.Balanced());
    fake.vars.clear();
    CHECK(binder.OnBeginEffects(kRt) == Outcome::NoDepthInput); // so does noDepthInput
}

void NoAllocationOnRefusals()
{
    // The disabled path and the refusals for runtimes that are not ours run on every frame of every ReShade user: no allocation
    // once the runtime is known.
    auto allocations = [](Rig& r)
    {
        r.fake.logCalls = false;
        r.Begin(); // the first begin creates the runtime's entry and the fake's bookkeeping
        r.Begin();
        g_allocs = 0;
        g_countAllocs = true;
        for (int i = 0; i < 100; ++i)
            r.Begin();
        g_countAllocs = false;
        return size_t(g_allocs);
    };
    Rig disabled(false);
    CHECK(allocations(disabled) == 0);
    Rig notD3D11;
    notD3D11.fake.d3d11 = false;
    CHECK(allocations(notD3D11) == 0);
    Rig otherDevice;
    otherDevice.fake.sameDevice = false;
    CHECK(allocations(otherDevice) == 0);
    g_allocs = 0; // switching the feature writes the status without building any text
    g_countAllocs = true;
    disabled.binder.SetEnabled(true);
    disabled.binder.SetEnabled(false);
    g_countAllocs = false;
    CHECK(g_allocs == 0);
}

void Threads()
{
    // The capture side publishes a fresh copy every time and frees the previous one right after PublishCapture returns, a reader
    // polls the status and the counters, and a switch toggles the feature, while the event thread runs thousands of real
    // passes. The core's reference to the copy is taken under the capture lock; without it the AddRef would hit a freed view.
    // Not verifiable here (no ThreadSanitizer on MSVC, and AddressSanitizer does not see data races): the guards in Status(),
    // SetStatus() and OnTechniquesChanged(). Taking them out is not caught by this test, and they stay.
    Fake fake;
    fake.logCalls = false;
    fake.slowAddRef = true;
    DepthBinder binder(fake, nullptr);
    binder.SetEnabled(true);
    std::atomic<bool> stop { false };
    std::atomic<int> torn { 0 };
    std::atomic<uint64_t> publishes { 0 };

    std::thread capture(
        [&]
        {
            uint64_t previous = 0;
            constexpr uint64_t kViews = 200000; // after that the last copy is published again (bounded memory)
            for (uint64_t i = 0; !stop.load(); ++i)
            {
                const bool fresh = i < kViews;
                const uint64_t view = fresh ? fake.NewView() : previous;
                DepthCapture c;
                c.valid = true;
                c.outputW = 3840;
                c.outputH = 2160;
                c.renderW = 1000 + uint32_t(i % 100);
                c.renderH = c.renderW - 500; // a status built from two different captures breaks this
                c.frame = fake.frame;
                c.tickMs = fake.now;
                c.copyView = view;
                c.device = kDevice;
                binder.PublishCapture(c);
                if (fresh && previous != 0)
                    fake.OwnerRelease(previous);
                previous = view;
                ++publishes;
            }
            if (previous != 0)
                fake.OwnerRelease(previous);
        });
    std::thread reader(
        [&]
        {
            while (!stop.load())
            {
                // A status is one of the fixed lines, or the sizes of ONE capture: render width = render height + 500, output
                // 3840x2160. Anything else (a kind from one write and sizes from another) is a torn read.
                const std::string s = binder.Status();
                if (s.rfind("Automatic scene depth", 0) == 0)
                {
                    unsigned w = 0, h = 0;
                    if (sscanf_s(s.c_str(), "Automatic scene depth: %ux%u -> 3840x2160", &w, &h) != 2 || h + 500 != w)
                        ++torn;
                }
                else if (s.empty())
                    ++torn;
                (void)binder.GetCounters();
                (void)binder.GetConsumer(fake.now).seen;
            }
        });
    std::thread toggler(
        [&]
        {
            while (!stop.load()) // writes the status as fast as it can: the feature is on about half of the time
            {
                binder.SetEnabled(false);
                binder.SetEnabled(true);
            }
        });

    // ReShade's own events from another thread (a reload or a preset change can arrive between a begin and its finish)
    std::thread events(
        [&]
        {
            while (!stop.load())
            {
                binder.OnTechniquesChanged(kRt);
                binder.OnReloadedEffects(kRt);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

    // The work is fixed, not the time: run until enough passes really bound and the capture side really published, however the
    // scheduler treats the threads. The deadline only keeps a broken run from hanging. The frame moves on with the capture side.
    constexpr uint64_t kBoundTarget = 20000, kPublishTarget = 1000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    uint64_t passes = 0, seenPublishes = 0;
    while ((binder.GetCounters().passesBound < kBoundTarget || publishes.load() < kPublishTarget) &&
           std::chrono::steady_clock::now() < deadline)
    {
        if (passes++ % 50 == 0 && publishes.load() != seenPublishes)
        {
            seenPublishes = publishes.load();
            ++fake.frame;
        }
        binder.OnBeginEffects(kRt);
        binder.OnFinishEffects(kRt);
    }
    stop = true;
    capture.join();
    reader.join();
    toggler.join();
    events.join();
    binder.SetEnabled(true);
    CHECK(fake.Balanced());
    CHECK(fake.violations == 0);
    CHECK(torn == 0);
    CHECK(binder.GetCounters().passesBound >= kBoundTarget);
    CHECK(publishes >= kPublishTarget);
    CHECK(!fake.HoldsCopy());
    std::printf("  threads: %llu passes bound of %llu, %llu captures published\n", (unsigned long long)binder.GetCounters().passesBound,
                (unsigned long long)passes, (unsigned long long)publishes.load());
}
} // namespace

int main()
{
    HappyPath();
    PassesInOneFrame();
    OriginalBindings();
    RestoreOnlyIfOurs();
    Gates();
    GateOrder();
    ClockAfterSnapshot();
    ConsumerInfoWindow();
    Untrusted();
    DecoyShapes();
    StuckCopyIsUnreachable();
    FailedCounter();
    ManualTransform();
    ManualTransformCache();
    DepthInputLookup();
    BufReadyDepth();
    EnableDisable();
    ReloadMidPass();
    ReloadCannotResolve();
    FailedRestoreRetry();
    RollbackResolvesAgain();
    PairingAndRuntimeLifetime();
    TwoRuntimes();
    FaultInjection();
    BackendFailures();
    LogHook();
    ThrowingHook();
    NoAllocationOnRefusals();
    Threads();
    if (g_fail)
    {
        std::printf("reshade_depth_core: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("reshade_depth_core: all checks passed\n");
    return 0;
}
