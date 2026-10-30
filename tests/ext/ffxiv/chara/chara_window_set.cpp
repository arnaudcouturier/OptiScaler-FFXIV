// The processed set of preview windows (CharaWindowSet.h): pure state, so every rule is checked here without a device. The test drives the
// same calls the dispatcher makes (an accepted finisher that teaches the camera block binding and proves the window draws, a UI draw that
// samples an image, the binding asked at the first geometry draw of a pass, the finisher that proves whether the name was right) on scripted
// timelines: admission and hysteresis, the tie-breaks between candidates, the range of the count, the identity the pass gate is built on
// (learned at every finisher, ambiguous while two windows share a binding, cleared when one of them stops drawing), the real feed of the four
// cases a shared binding shows (an active and a sparse window, two windows drawing, an image outside the set, a window that takes a closed
// window's binding over), the hold a wrong name puts on the binding, the uneven render cadences the game shows, and the differential against
// the slot table with the set off.
#include "check.h"

#include "ext/ffxiv/chara/CharaSlots.h"
#include "ext/ffxiv/chara/CharaWindowSet.h"

#include <initializer_list>
#include <vector>

using namespace ext::ffxiv::chara;

namespace
{
constexpr uintptr_t kA = 0xA000;
constexpr uintptr_t kB = 0xB000;
constexpr uintptr_t kC = 0xC000;
constexpr uintptr_t kD = 0xD000;
constexpr uintptr_t kE = 0xE000;
constexpr uintptr_t kF = 0x5000;
constexpr uintptr_t kX = 0x9000; // a preview window the set does not process

// The camera constant buffer binding each preview image draws with: one buffer object per window, and the same binding for every render of
// that window. Only the binding is known to the identity (no content is read).
camera::BlockKey BlockOf(uintptr_t key)
{
    return camera::BlockKey { key + 0x10000, 0, 576, 960 };
}

// A binding two windows draw with (a shared per-frame camera buffer, or two windows at the same camera).
constexpr camera::BlockKey kShared { 0x77000, 0, 576, 960 };

// One render of `key` in the game: the accepted finisher teaches the binding and proves the window draws. `block` defaults to the window's
// own binding.
WindowSet::Learned Render(WindowSet& s, uintptr_t key, uint64_t now, camera::BlockKey block = {})
{
    const camera::BlockKey used = block.Valid() ? block : BlockOf(key);
    return s.NoteFinisher(key, used, now);
}

// One UI draw that samples the preview image of `key`.
void Sample(WindowSet& s, uintptr_t key, int entry, uint64_t now) { s.NoteDemand(key, entry, now); }

bool Named(WindowSet& s, uintptr_t key, uint64_t now) { return s.NamePass(BlockOf(key), now).name == WindowSet::Name::Named; }
WindowSet::Name NameOf(WindowSet& s, uintptr_t key, uint64_t now) { return s.NamePass(BlockOf(key), now).name; }

void TestOffChangesNothing()
{
    WindowSet s;
    s.Configure(false, 1);
    CHECK(!s.On());
    CHECK(s.Max() == 1);
    // off: every image is processed and every pass is followed, so the caller's gates are no-ops
    for (const uintptr_t key : { kA, kB, kC })
        CHECK(s.Processed(key));
    CHECK(s.FollowsPass(WindowSet::Name::Unknown) && s.FollowsPass(WindowSet::Name::Named));
    CHECK(s.NamePass(BlockOf(kA), 1000).name == WindowSet::Name::Named); // nothing is decided, so the pass is followed as before
    CHECK(s.KnownBlocks() == 0);
    // demand and finishers are not remembered at all while the set is off, so the per-draw call costs nothing and nothing is ever reset
    for (uintptr_t key = 0x1000; key < 0x1020; key += 0x10)
    {
        Sample(s, key, static_cast<int>(key & 7), 1000);
        CHECK(!Render(s, key, 1000).block);
    }
    CHECK(s.Count() == 0 && s.FreshCount(1000) == 0 && s.KnownBlocks() == 0);
    CHECK(s.KnownImages() == 0); // nothing is remembered, so the per-draw call costs nothing
    CHECK(!s.HasWaitingCandidate(1000));

    // switching it on starts from nothing, and switching it off forgets the set
    s.Configure(true, 2);
    Render(s, kA, 2000);
    Sample(s, kA, 0, 2000);
    CHECK(s.Processed(kA) && s.Count() == 1 && s.KnownBlocks() == 1 && Named(s, kA, 2000));
    CHECK(s.KnownImages() == 1); // the render remembered the image before the UI draw sampled it
    s.Configure(false, 2);
    CHECK(s.Count() == 0 && s.Processed(kA) && s.KnownBlocks() == 0);
}

void TestCapRange()
{
    WindowSet s;
    s.Configure(true, 0); // below the range: one
    CHECK(s.Max() == 1 && s.On());
    s.Configure(true, 99); // above the range: eight
    CHECK(s.Max() == 8);
    s.Configure(true, 3);
    CHECK(s.Max() == 3);
    // the count is clamped while members exist: the least recently sampled ones leave
    for (const uintptr_t key : { kA, kB, kC })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    CHECK(s.Count() == 3);
    s.Configure(true, 2);
    CHECK(s.Max() == 2 && s.Count() == 2);
    size_t members = 0;
    for (const uintptr_t key : { kA, kB, kC })
        members += s.Processed(key) ? 1 : 0;
    CHECK(members == 2);
    CHECK(!s.Processed(kA)); // the first and oldest member left
    CHECK(s.Processed(kB) && s.Processed(kC));
}

// A window takes a place only while it draws: a finisher within the demand window is the proof. A UI draw that samples an image the renderer no
// longer draws (a window that is open but paused, a stale output) does not take a place from a window that renders.
void TestRenderingIsRequired()
{
    WindowSet s;
    s.Configure(true, 1);
    Sample(s, kA, 0, 1000); // sampled, never seen drawing
    CHECK(!s.Processed(kA) && s.Count() == 0);
    Render(s, kA, 1000);
    Sample(s, kA, 0, 1000);
    CHECK(s.Processed(kA)); // the finisher proved it draws
    // a place is free: an image sampled but not drawing waits for its first finisher
    s.Configure(true, 2);
    Sample(s, kB, 1, 1000);
    CHECK(!s.Processed(kB) && s.Count() == 1);
    Render(s, kB, 1000);
    Sample(s, kB, 1, 1000);
    CHECK(s.Processed(kB) && s.Count() == 2);
    // the render is older than the demand window: the image is only sampled again, it is not drawn
    s.Configure(true, 1);
    Sample(s, kC, 2, 3000);
    CHECK(!s.Processed(kC));
    Render(s, kC, 3000);
    Sample(s, kC, 2, 3000);
    CHECK(s.Processed(kC) && !s.Processed(kA) && !s.Processed(kB));
    // a window that drew once and then stopped: its old finisher is no proof any more, so a UI draw that samples it does not take the place of
    // a member
    WindowSet stale;
    stale.Configure(true, 1);
    Render(stale, kA, 1000);
    Sample(stale, kA, 0, 1000);
    Render(stale, kB, 1000); // B drew once
    Sample(stale, kB, 1, 3000);
    CHECK(stale.Processed(kA) && !stale.Processed(kB));
    Render(stale, kB, 3100); // it draws again: now it takes the place
    Sample(stale, kB, 1, 3100);
    CHECK(stale.Processed(kB) && !stale.Processed(kA));
}

void TestAdmissionAndHysteresis()
{
    WindowSet s;
    s.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    CHECK(s.Count() == 2 && s.Processed(kA) && s.Processed(kB));
    // a third image while both members are live: it waits, and both keep their places (hysteresis)
    Render(s, kC, 1000);
    Sample(s, kC, 2, 1000);
    CHECK(!s.Processed(kC) && s.Processed(kA) && s.Processed(kB));
    Render(s, kA, 1100);
    Sample(s, kA, 0, 1100);
    Render(s, kC, 1100);
    Sample(s, kC, 2, 1100);
    CHECK(!s.Processed(kC));
    // the weakest member has been idle for more than 250 ms: it is replaced
    Render(s, kC, 1400);
    Sample(s, kC, 2, 1400);
    CHECK(s.Processed(kC) && !s.Processed(kB) && s.Processed(kA));
    // the evicted image comes back the same way: A has been idle for 900 ms, so B takes its place
    Render(s, kB, 2000);
    Sample(s, kB, 1, 2000);
    CHECK(s.Processed(kB) && !s.Processed(kA) && s.Processed(kC));
    Render(s, kB, 2100);
    Sample(s, kB, 1, 2100); // B keeps its place while it is sampled
    CHECK(s.Processed(kB) && s.Count() == 2);
    // the edge: a member idle for exactly 250 ms keeps its place; one millisecond more and it is replaced
    WindowSet edge;
    edge.Configure(true, 1);
    Render(edge, kA, 1000);
    Sample(edge, kA, 0, 1000);
    Render(edge, kB, 1000);
    Sample(edge, kB, 1, 1000); // one place: B waits
    Render(edge, kB, 1250);
    Sample(edge, kB, 1, 1250);
    CHECK(edge.Processed(kA) && !edge.Processed(kB));
    Render(edge, kB, 1251);
    Sample(edge, kB, 1, 1251);
    CHECK(edge.Processed(kB) && !edge.Processed(kA));
}

// A stall (a hitch, an alt-tab, a zone change) leaves every member idle at the same moment. The images that come back in one batch must not
// displace each other: a member loses its place only to an image that takes it, one exchange per pause.
void TestStallHysteresis()
{
    WindowSet s;
    s.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    CHECK(s.Processed(kA) && s.Processed(kB));
    // 1500 ms later the UI samples a third window, then the two members again, all at the same moment: the set is stale for all three
    Render(s, kC, 2500);
    Sample(s, kC, 2, 2500);
    CHECK(s.Processed(kC)); // the newcomer takes the place of the weakest member (A, sampled oldest)
    Render(s, kA, 2500);
    Sample(s, kA, 0, 2500);
    Render(s, kB, 2500);
    Sample(s, kB, 1, 2500);
    CHECK(s.Processed(kB)); // B was idle just as long, but no member is displaced twice in one pause
    CHECK(!s.Processed(kA));
    CHECK(s.Count() == 2);
    // after the pause another exchange is allowed: A replaces the weakest member again
    Render(s, kA, 3000);
    Sample(s, kA, 0, 3000);
    CHECK(s.Processed(kA) && s.Count() == 2);
    CHECK(s.Processed(kB) || s.Processed(kC));
}

// The residual of the stall rule: a non-member that is only sampled (a window whose output a UI draw shows while the renderer does not draw it)
// displaces nobody. Only an image that is actually drawing takes a place, so a stall cannot cost a member its slot and its NR model for a
// window that is not rendering anyway.
void TestStallNeedsARender()
{
    WindowSet s;
    s.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    // the stall is over, the members come back, and a third window is sampled without ever drawing
    Sample(s, kC, 2, 2500);
    Render(s, kA, 2500);
    Sample(s, kA, 0, 2500);
    Render(s, kB, 2500);
    Sample(s, kB, 1, 2500);
    CHECK(s.Processed(kA) && s.Processed(kB) && !s.Processed(kC)); // no member lost its place to a window that does not draw
    CHECK(s.Count() == 2);
    // it draws: now it may take a place (the pause has passed)
    Render(s, kC, 3000);
    Sample(s, kC, 2, 3000);
    CHECK(s.Processed(kC) && s.Count() == 2);
    CHECK(!s.Processed(kA) || !s.Processed(kB));
}

void TestTieBreaks()
{
    WindowSet s;
    s.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    // two candidates sampled at the same time while the members are live: the next admission picks one of them
    Render(s, kC, 1100);
    Sample(s, kC, 3, 1100);
    Render(s, kD, 1100);
    Sample(s, kD, 2, 1100);
    CHECK(!s.Processed(kC) && !s.Processed(kD));
    Render(s, kA, 1400); // refreshes A; B has been idle 400 ms, so one candidate takes its place
    Sample(s, kA, 0, 1400);
    CHECK(s.Processed(kD) && !s.Processed(kC) && !s.Processed(kB)); // the same sample time: the lower output entry wins
    // the same output entry on both candidates: the lower pointer wins
    Render(s, kD, 1440);
    Sample(s, kD, 2, 1440);
    for (const uintptr_t key : { kE, kF })
    {
        Render(s, key, 1450);
        Sample(s, key, 2, 1450);
    }
    CHECK(!s.Processed(kE) && !s.Processed(kF));
    Render(s, kD, 1700); // A has been idle 300 ms: replaced by the better candidate
    Sample(s, kD, 2, 1700);
    CHECK(s.Processed(kF) && !s.Processed(kE) && !s.Processed(kA)); // the same sample time and entry: the lower pointer wins
    // an image with no entry label sorts after the ones that have one
    WindowSet t;
    t.Configure(true, 2);
    Render(t, kA, 1000);
    Sample(t, kA, 0, 1000);
    Render(t, kD, 1000);
    Sample(t, kD, 3, 1000);
    Render(t, kB, 1100);
    Sample(t, kB, -1, 1100);
    Render(t, kC, 1100);
    Sample(t, kC, 7, 1100);
    Render(t, kA, 1400); // A is refreshed; D has been idle 400 ms, and C has an entry where B has none
    Sample(t, kA, 0, 1400);
    CHECK(t.Processed(kC) && !t.Processed(kB) && t.Processed(kA) && !t.Processed(kD));
}

// The gate is built on the image a pass is drawing, not on a guess about the order of the renders: the camera constant buffer binding asked at
// the first geometry draw names the image, and every accepted finisher teaches which binding belongs to which image.
void TestIdentity()
{
    WindowSet s;
    s.Configure(true, 1);
    Render(s, kA, 1000);
    Sample(s, kA, 0, 1000);
    // nothing is known before a finisher taught the binding
    CHECK(NameOf(s, kA, 1000) == WindowSet::Name::Named);
    CHECK(s.NamePass(camera::BlockKey {}, 1000).name == WindowSet::Name::Invalid); // no binding: nothing to name
    CHECK(!s.FollowsPass(WindowSet::Name::Unknown));
    // a finisher without a camera block (a pass whose geometry draw had none) teaches nothing, so the empty binding stays unnamed
    CHECK(!s.NoteFinisher(kD, camera::BlockKey {}, 1000).block);
    CHECK(s.NamePass(camera::BlockKey {}, 1000).name == WindowSet::Name::Invalid);
    CHECK(s.NamePass(BlockOf(kE), 1000).name == WindowSet::Name::Unknown); // a binding never seen at a finisher
    CHECK(!s.FollowsPass(s.NamePass(BlockOf(kE), 1000).name));
    // an image outside the set: its binding is learned, and its passes are not followed
    Render(s, kB, 1000);
    Sample(s, kB, 1, 1000);
    CHECK(!s.Processed(kB)); // one place: the second window waits
    CHECK(NameOf(s, kB, 1000) == WindowSet::Name::Outside);
    CHECK(!s.FollowsPass(WindowSet::Name::Outside));
    CHECK(s.Processed(kA) && !s.Processed(kB));
    // a binding two images draw with (two windows at the same camera) names neither, so neither pass is followed: following one would shift a
    // picture no upscaler runs on
    WindowSet shared;
    shared.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(shared, key, 1000);
        Sample(shared, key, static_cast<int>(key & 3), 1000);
    }
    CHECK(shared.NamePass(kShared, 1000).name == WindowSet::Name::Unknown); // not learned yet
    Render(shared, kA, 1000, kShared);
    CHECK(shared.NamePass(kShared, 1000).name == WindowSet::Name::Named);
    Render(shared, kB, 1000, kShared);
    CHECK(shared.NamePass(kShared, 1000).name == WindowSet::Name::Ambiguous);
    CHECK(!shared.FollowsPass(shared.NamePass(kShared, 1000).name));
    CHECK(shared.Processed(kA) && shared.Processed(kB));
    // with the set off nothing of this is consulted
    WindowSet off;
    off.Configure(false, 2);
    CHECK(off.NamePass(kShared, 1000).name == WindowSet::Name::Named && off.FollowsPass(WindowSet::Name::Named));
    Render(off, kA, 1000, kShared);
    CHECK(off.KnownBlocks() == 0);
    // the least recently learned binding is forgotten first: sixteen bindings are remembered
    WindowSet many;
    many.Configure(true, 1);
    Render(many, kA, 1000);
    Sample(many, kA, 0, 1000);
    for (uint64_t i = 0; i < kMaxCameraBlocks; ++i)
        Render(many, kA, 1000 + i, camera::BlockKey { 0x1000 + i, 0, 576, 960 });
    CHECK(many.KnownBlocks() == kMaxCameraBlocks);
    CHECK(many.NamePass(camera::BlockKey { 0x1000, 0, 576, 960 }, 1000).name == WindowSet::Name::Named);
    Render(many, kA, 2000, camera::BlockKey { 0x9999, 0, 576, 960 });
    CHECK(many.NamePass(camera::BlockKey { 0x1000, 0, 576, 960 }, 2000).name == WindowSet::Name::Unknown); // the oldest was reused
    CHECK(many.NamePass(camera::BlockKey { 0x9999, 0, 576, 960 }, 2000).name == WindowSet::Name::Named);
}

// Every accepted finisher teaches its binding, so the identity is filled while the set is complete: a window that opens later (a K+1th) finds
// the members' bindings known and costs them no render. Only the newcomer waits for its own first finisher, which is the render it would keep
// native anyway.
void TestLearnsEveryFinisher()
{
    WindowSet s;
    s.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    CHECK(s.Processed(kA) && s.Processed(kB) && s.KnownBlocks() == 2);
    // three more renders of the two members, then a third window opens: A and B are named from their first pass on
    for (int frame = 0; frame < 3; ++frame)
    {
        for (const uintptr_t key : { kA, kB })
        {
            Render(s, key, 1010 + static_cast<uint64_t>(frame));
            Sample(s, key, static_cast<int>(key & 3), 1010 + static_cast<uint64_t>(frame));
        }
    }
    Render(s, kC, 1020);
    Sample(s, kC, 2, 1020);
    CHECK(Named(s, kA, 1020) && Named(s, kB, 1020)); // no member lost a render to an empty identity
    CHECK(NameOf(s, kC, 1020) == WindowSet::Name::Outside);
    CHECK(!s.Processed(kC));
    // the newcomer's own pass is not followed, and its next finisher names it
    Render(s, kC, 1021);
    CHECK(NameOf(s, kC, 1021) == WindowSet::Name::Outside);
    // every render of every window keeps its own answer: a pass is followed exactly when its image is processed
    size_t lostProcessed = 0, followedOutside = 0;
    for (const uintptr_t key : { kA, kC, kB, kC, kA, kC })
    {
        const bool follows = s.FollowsPass(NameOf(s, key, 1021));
        const bool isProcessed = s.Processed(key);
        lostProcessed += (isProcessed && !follows) ? 1 : 0;
        followedOutside += (!isProcessed && follows) ? 1 : 0;
        Render(s, key, 1021);
    }
    CHECK(lostProcessed == 0);
    CHECK(followedOutside == 0);
    CHECK(s.KnownBlocks() == 3);
}

// Two windows that draw with one camera block: the block names neither while both draw, and it names the survivor once the other one stops
// drawing (its window was closed), instead of keeping the survivor native until its camera moves or the limit is switched off.
void TestAmbiguityClears()
{
    WindowSet s;
    s.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    Render(s, kA, 1000, kShared);
    Render(s, kB, 1000, kShared);
    CHECK(s.NamePass(kShared, 1000).name == WindowSet::Name::Ambiguous);
    // both keep drawing: still ambiguous
    Render(s, kA, 1500, kShared);
    Render(s, kB, 1500, kShared);
    CHECK(s.NamePass(kShared, 1500).name == WindowSet::Name::Ambiguous);
    // B's window is closed: after the demand window the binding names A again, and A's passes are followed
    Render(s, kA, 2600, kShared);
    const WindowSet::PassName name = s.NamePass(kShared, 2600);
    CHECK(name.name == WindowSet::Name::Named && name.image == kA);
    CHECK(name.cleared); // the ambiguity ended here, and the diagnostics count it
    CHECK(s.FollowsPass(name.name));
    CHECK(s.NamePass(kShared, 2600).name == WindowSet::Name::Named); // cleared once, not once per lookup
    // both windows stop drawing: the binding names neither until one of them draws again
    WindowSet idle;
    idle.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(idle, key, 1000);
        Sample(idle, key, static_cast<int>(key & 3), 1000);
    }
    Render(idle, kA, 1000, kShared);
    Render(idle, kB, 1000, kShared);
    CHECK(idle.NamePass(kShared, 2500).name == WindowSet::Name::Ambiguous); // both stale: neither is proven
    Render(idle, kB, 2500, kShared); // B draws again: it takes the binding over, the ambiguity ends at that finisher
    CHECK(idle.NamePass(kShared, 2500).name == WindowSet::Name::Named);
    CHECK(idle.NamePass(kShared, 2500).image == kB);
}

// The cadences the game shows, all of them within one game frame (the clock does not move between the renders of the loop): the pass that
// follows a render belongs to that render, so the gate's answer is exactly "is this image processed". No render of a window outside the
// set is ever followed, and no render of a processed window is lost.
void TestCadenceByIdentity(const char* name, uint32_t k, std::initializer_list<uintptr_t> cadence, size_t repeats)
{
    (void) name;
    WindowSet s;
    s.Configure(true, k);
    const uint64_t now = 1000;
    const std::vector<uintptr_t> images(cadence);
    // One render per image first, then the UI draw that samples it: the finisher teaches the binding and proves the window draws.
    for (size_t i = 0; i < images.size(); ++i)
    {
        Render(s, images[i], now);
        Sample(s, images[i], static_cast<int>(i), now);
    }

    size_t processed = 0, followedProcessed = 0, followedOutside = 0, lostProcessed = 0;
    for (size_t r = 0; r < repeats; ++r)
    {
        for (const uintptr_t image : images)
        {
            const bool follows = s.FollowsPass(NameOf(s, image, now)); // the decision for the render that follows
            const bool isProcessed = s.Processed(image);               // what that render really is
            if (isProcessed)
            {
                ++processed;
                followedProcessed += follows ? 1 : 0;
                lostProcessed += follows ? 0 : 1;
            }
            else
                followedOutside += follows ? 1 : 0;
            Render(s, image, now); // the finisher names the image of the binding
        }
    }
    CHECK(lostProcessed == 0);
    CHECK(followedOutside == 0);
    CHECK(followedProcessed == processed);
}

void TestCadences()
{
    TestCadenceByIdentity("A A B, one place", 1, { kA, kA, kB }, 6);
    TestCadenceByIdentity("A A B A A C, two places", 2, { kA, kA, kB, kA, kA, kC }, 6);
    TestCadenceByIdentity("A A A B, one place", 1, { kA, kA, kA, kB }, 5);
    TestCadenceByIdentity("A B A B C, two places", 2, { kA, kB, kA, kB, kC }, 6);
    TestCadenceByIdentity("A B A, one place", 1, { kA, kB, kA }, 8);
    // three windows, two places: the third image's renders are never followed and the two processed ones never lose one
    TestCadenceByIdentity("A B C rotating, two places", 2, { kA, kB, kC, kB, kA }, 6);
}

// The menu line counts the windows that are really on screen: a member that was closed is only dropped when another image takes its place.
void TestFreshCount()
{
    WindowSet s;
    s.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    CHECK(s.Count() == 2 && s.FreshCount(1000) == 2);
    CHECK(s.FreshCount(1200) == 2); // still within the demand window
    CHECK(s.FreshCount(3000) == 0); // both windows closed, no image took their place: Count stays 2, nothing is on screen
    CHECK(s.Count() == 2);
    Render(s, kA, 3100);
    Sample(s, kA, 0, 3100);
    CHECK(s.FreshCount(3100) == 1);
    // with the set off nothing is counted
    WindowSet off;
    off.Configure(false, 2);
    Sample(off, kA, 0, 1000);
    CHECK(off.FreshCount(1000) == 0);
}

// Nothing that can hold the camera block is bound at the draw's own stage: the pass is not named, and the diagnostics count it apart from a
// binding that no finisher has taught yet (a different problem with a different fix).
void TestInvalidBinding()
{
    WindowSet s;
    s.Configure(true, 1);
    const camera::BlockKey none {}; // the context had nothing bound that can hold the camera block
    CHECK(s.NamePass(none, 1000).name == WindowSet::Name::Invalid);
    CHECK(!s.FollowsPass(WindowSet::Name::Invalid));
    Render(s, kA, 1000);
    Sample(s, kA, 0, 1000);
    CHECK(s.NamePass(none, 1000).name == WindowSet::Name::Invalid); // still nothing that could name the pass
    CHECK(s.NamePass(camera::BlockKey { 0x99000, 0, 576, 960 }, 1000).name == WindowSet::Name::Unknown); // a binding no finisher taught
    CHECK(s.NamePass(BlockOf(kA), 1000).name == WindowSet::Name::Named);
    // with the set off a pass is followed as before, whatever is bound
    WindowSet off;
    off.Configure(false, 1);
    CHECK(off.NamePass(none, 1000).name == WindowSet::Name::Named);
}

// Three images that draw with one binding: the binding keeps the two that drew last, so the third one displaces the oldest instead of being
// forgotten, and a pass drawn with the binding is named for whichever of the two is still drawing.
void TestThirdImage()
{
    WindowSet s;
    s.Configure(true, 3);
    for (const uintptr_t key : { kA, kB, kC })
    {
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(key & 3), 1000);
    }
    CHECK(s.Processed(kA) && s.Processed(kB) && s.Processed(kC));
    Render(s, kA, 1000, kShared);
    Render(s, kB, 1500, kShared);
    Render(s, kC, 1900, kShared); // the third image: it keeps the place of the oldest one, A
    CHECK(s.NamePass(kShared, 1900).name == WindowSet::Name::Ambiguous); // the two it kept both drew lately
    // A has not drawn with the binding for the demand window: the binding names the third image, which is the one that still draws
    const WindowSet::PassName name = s.NamePass(kShared, 2400);
    CHECK(name.name == WindowSet::Name::Named && name.image == kC);
    CHECK(name.cleared);
    // B was dropped from the two the binding keeps: it draws again and becomes the second image, so the binding names neither
    Render(s, kB, 2400, kShared);
    CHECK(s.NamePass(kShared, 2400).name == WindowSet::Name::Ambiguous);
    // C stops drawing: the one that still draws is named
    Render(s, kB, 3600, kShared);
    const WindowSet::PassName later = s.NamePass(kShared, 3600);
    CHECK(later.name == WindowSet::Name::Named && later.image == kB);
}

// One pass of the real feed, in the order the dispatcher and the DLSS path run it: the binding asked at the pass's first geometry draw (the
// name), the accepted finisher that teaches the binding, and the finisher's proof of the name (after the teach, as `Finish` does). A UI draw of
// the same frame samples every window that is on screen (Samples), which is the demand the set is built on.
struct FeedPass
{
    WindowSet::Name name = WindowSet::Name::Unknown;
    uintptr_t named = 0;   // the image the binding named (0: none)
    bool followed = false; // the pass was followed (upscaled)
    bool mismatch = false; // the finisher was another image than the name said
};

// What a run of the feed showed, for the assertions below.
struct Feed
{
    size_t misnamed = 0;        // passes named for another image than the one that drew (the newcomer's first render, then one per window)
    size_t namedWrong = 0;      // passes named for an image that is not the one that drew them
    size_t followedOutside = 0; // passes of an image the set does not process that were followed anyway
    std::vector<uint64_t> misnames; // when those passes finished
};

struct TimedPass
{
    uint64_t now = 0;
    bool processed = false; // the image was a member of the set when the pass started
    FeedPass pass;
};

// The UI draws of one frame sample every preview window that is on screen.
void Samples(WindowSet& s, uint64_t now, const std::vector<uintptr_t>& visible)
{
    int entry = 0;
    for (const uintptr_t key : visible)
        s.NoteDemand(key, entry++, now);
}

FeedPass RunFeedPass(WindowSet& s, uintptr_t key, uint64_t now, bool processed, Feed& f)
{
    const WindowSet::PassName name = s.NamePass(kShared, now);
    s.NoteFinisher(key, kShared, now);
    FeedPass p;
    p.name = name.name;
    p.named = name.image;
    p.followed = s.FollowsPass(name.name);
    p.mismatch = s.NoteMismatch(kShared, name.image, key, now);
    if (p.mismatch)
    {
        ++f.misnamed;
        f.misnames.push_back(now);
    }
    if (p.named != 0 && p.named != key)
        ++f.namedWrong;
    if (p.followed && !processed)
        ++f.followedOutside;
    return p;
}

// Whether `now` is inside a window a mismatch opened: the binding names nobody for kWindowMismatchMs after each.
bool InsideHold(const std::vector<uint64_t>& misnames, uint64_t now)
{
    for (const uint64_t at : misnames)
    {
        if (now >= at && now < at + kWindowMismatchMs)
            return true;
    }
    return false;
}

// Case 1: an active window that draws every frame and a sparse one that opens 3 s later and then draws every 3 s, on one binding. The sparse
// window's first render is the only one that can be named for the active one (nothing knows it draws before its finisher); the mismatch then
// holds the binding for 30 s, and every pass drawn with it stays native. The sparse window is therefore mis-named at most once per window
// instead of on every render, and no pass is ever named for another window's image, so no pass composes or clears another window's picture.
// The active window is followed whenever the binding is not held, which is the stated cost of failing open.
void TestFeedActiveAndSparse()
{
    WindowSet s;
    s.Configure(true, 2);
    const uint64_t start = 10000;
    const std::vector<uintptr_t> onlyA { kA };
    const std::vector<uintptr_t> both { kA, kB };
    Feed f;
    std::vector<TimedPass> aPasses, bPasses;
    // A opens on the binding: its first render names nothing (no finisher taught the binding yet) and stays native.
    Samples(s, start, onlyA);
    const FeedPass first = RunFeedPass(s, kA, start, false, f);
    CHECK(first.name == WindowSet::Name::Unknown && !first.followed);
    uint64_t nextA = start + 16;
    uint64_t nextB = start + 3010; // the sparse window opens 3 s later, 10 ms off A's frames
    bool bOpen = false;
    while (nextA < start + 60000 || nextB < start + 60000)
    {
        const bool aDue = nextA <= nextB;
        const uint64_t now = aDue ? nextA : nextB;
        if (!aDue)
            bOpen = true; // the window is on screen from its first render on
        Samples(s, now, bOpen ? both : onlyA);
        if (aDue)
        {
            const bool processed = s.Processed(kA);
            aPasses.push_back({ now, processed, RunFeedPass(s, kA, now, processed, f) });
            nextA += 16;
        }
        else
        {
            const bool processed = s.Processed(kB);
            bPasses.push_back({ now, processed, RunFeedPass(s, kB, now, processed, f) });
            nextB += 3000;
        }
    }
    CHECK(s.Processed(kA) && s.Processed(kB));
    // The sparse window is mis-named once when it opens and then once per 30 s window (the binding is held in between), two of its nineteen
    // renders in the minute, never on every render.
    CHECK(bPasses.size() == 19);
    CHECK(f.misnamed == 2);
    for (size_t i = 1; i < f.misnames.size(); ++i)
        CHECK(f.misnames[i] - f.misnames[i - 1] >= kWindowMismatchMs);
    // A pass is named for another window only on the render that proves the binding is shared (one per hold window); every other pass is named
    // for its own image or for nobody, so no pass of one window ever composes or clears the other window's picture.
    CHECK(f.namedWrong == f.misnamed);
    CHECK(f.followedOutside == 1); // only the newcomer's very first render, before a UI draw admitted it
    // Inside a hold the sparse window's passes are named for nobody and stay native; the active window is followed exactly outside the holds.
    size_t bNamedInsideHold = 0, aFollowedInsideHold = 0, aNativeOutsideHold = 0;
    for (const TimedPass& b : bPasses)
        bNamedInsideHold += (InsideHold(f.misnames, b.now) && b.pass.named != 0 && !b.pass.mismatch) ? 1 : 0;
    for (const TimedPass& a : aPasses)
    {
        if (InsideHold(f.misnames, a.now))
            aFollowedInsideHold += a.pass.followed ? 1 : 0;
        else
            aNativeOutsideHold += a.pass.followed ? 0 : 1;
    }
    CHECK(bNamedInsideHold == 0);
    CHECK(aFollowedInsideHold == 0);
    CHECK(aNativeOutsideHold == 0);
}

// Case 2: two windows that both draw on one binding, the second one opening next to the running first one (its first pass is the transient
// mis-name). From that finisher on the binding names nobody: every pass of both windows is ambiguous and stays native for the rest of the
// minute, so there is no composed/native alternation and no pass is named for the other window.
void TestFeedBothDrawing()
{
    WindowSet s;
    s.Configure(true, 2);
    const uint64_t start = 10000;
    const std::vector<uintptr_t> onlyA { kA };
    const std::vector<uintptr_t> both { kA, kB };
    Feed f;
    std::vector<TimedPass> passes;
    Samples(s, start, onlyA);
    const FeedPass first = RunFeedPass(s, kA, start, false, f);
    CHECK(first.name == WindowSet::Name::Unknown && !first.followed);
    uint64_t nextA = start + 16;
    uint64_t nextB = start + 8; // the second window opens 8 ms later
    while (nextA < start + 60000 || nextB < start + 60000)
    {
        const bool aDue = nextA <= nextB;
        const uint64_t now = aDue ? nextA : nextB;
        Samples(s, now, both);
        if (aDue)
        {
            const bool processed = s.Processed(kA);
            passes.push_back({ now, processed, RunFeedPass(s, kA, now, processed, f) });
            nextA += 16;
        }
        else
        {
            const bool processed = s.Processed(kB);
            passes.push_back({ now, processed, RunFeedPass(s, kB, now, processed, f) });
            nextB += 16;
        }
    }
    CHECK(f.misnamed == 1);   // the second window's first render only: nothing could tell the two apart before its finisher
    CHECK(f.namedWrong == 1);
    CHECK(f.followedOutside == 1); // that first render of the not-yet-member window
    // After the mismatch no pass is named at all, so neither window is composed from the other's picture and neither is cleared by the other's
    // pass: both keep the game's own picture (fail open) while both draw with the binding.
    size_t namedAfter = 0, followedAfter = 0;
    for (const TimedPass& p : passes)
    {
        if (p.now <= f.misnames.front())
            continue;
        namedAfter += p.pass.named != 0 ? 1 : 0;
        followedAfter += p.pass.followed ? 1 : 0;
    }
    CHECK(namedAfter == 0);
    CHECK(followedAfter == 0);
}

// Case 3: an image the set does not process (one place, taken by the active window) draws every 3 s with the member's binding. Its first
// render is named for the member, its finisher proves the binding cannot tell them apart, and from then on the binding is held: the outside
// image's renders are native (never followed, never named for the member) and the member's passes are followed only outside the holds.
void TestFeedOutsideTheSet()
{
    WindowSet s;
    s.Configure(true, 1); // one place: A is processed, X never is
    const uint64_t start = 10000;
    const std::vector<uintptr_t> onlyA { kA };
    const std::vector<uintptr_t> both { kA, kX };
    Feed f;
    std::vector<TimedPass> aPasses, xPasses;
    Samples(s, start, onlyA);
    const FeedPass first = RunFeedPass(s, kA, start, false, f);
    CHECK(first.name == WindowSet::Name::Unknown && !first.followed);
    uint64_t nextA = start + 16;
    uint64_t nextX = start + 3010;
    bool xOpen = false;
    while (nextA < start + 60000 || nextX < start + 60000)
    {
        const bool aDue = nextA <= nextX;
        const uint64_t now = aDue ? nextA : nextX;
        if (!aDue)
            xOpen = true;
        Samples(s, now, xOpen ? both : onlyA);
        if (aDue)
        {
            const bool processed = s.Processed(kA);
            aPasses.push_back({ now, processed, RunFeedPass(s, kA, now, processed, f) });
            nextA += 16;
        }
        else
        {
            const bool processed = s.Processed(kX);
            xPasses.push_back({ now, processed, RunFeedPass(s, kX, now, processed, f) });
            nextX += 3000;
        }
    }
    CHECK(!s.Processed(kX));
    // X is never processed (one place holds A), its renders are followed only on those that prove the binding is shared (one per hold
    // window), and it is mis-named once per window instead of on every render.
    CHECK(xPasses.size() == 19);
    size_t xFollowed = 0, xProcessed = 0;
    for (const TimedPass& x : xPasses)
    {
        xFollowed += x.pass.followed ? 1 : 0;
        xProcessed += x.processed ? 1 : 0;
    }
    CHECK(xFollowed == f.misnamed);
    CHECK(xProcessed == 0);
    CHECK(f.followedOutside == f.misnamed);
    CHECK(f.namedWrong == f.misnamed);
    CHECK(f.misnamed == 2);
    for (size_t i = 1; i < f.misnames.size(); ++i)
        CHECK(f.misnames[i] - f.misnames[i - 1] >= kWindowMismatchMs);
    // Inside a hold the outside image's passes are named for nobody; the member's passes are followed exactly outside the holds.
    size_t xNamedInsideHold = 0, aFollowedInsideHold = 0, aNativeOutsideHold = 0;
    for (const TimedPass& x : xPasses)
        xNamedInsideHold += (InsideHold(f.misnames, x.now) && x.pass.named != 0 && !x.pass.mismatch) ? 1 : 0;
    for (const TimedPass& a : aPasses)
    {
        if (InsideHold(f.misnames, a.now))
            aFollowedInsideHold += a.pass.followed ? 1 : 0;
        else
            aNativeOutsideHold += a.pass.followed ? 0 : 1;
    }
    CHECK(xNamedInsideHold == 0);
    CHECK(aFollowedInsideHold == 0);
    CHECK(aNativeOutsideHold == 0);
}

// Case 4: one window closes and another one takes its binding over. A draws every frame for 2 s and its window closes; B opens 2 s later on the
// same binding. At B's first draw the binding still names A, the image it learned last and which stopped drawing, so that one render is
// mis-named; its finisher replaces A on the binding and only B draws with it now, so the binding names B from the next render on instead of
// holding B native for 30 s. Every render of B after the first one is followed.
void TestFeedWindowTakesTheBindingOver()
{
    WindowSet s;
    s.Configure(true, 2);
    const uint64_t start = 10000;
    const uint64_t aStop = start + 2000; // A's window closes after 2 s
    const uint64_t bOpen = aStop + 2000; // the next window opens 2 s after it closed
    const std::vector<uintptr_t> onlyA { kA };
    const std::vector<uintptr_t> onlyB { kB };
    Feed f;
    Samples(s, start, onlyA);
    const FeedPass first = RunFeedPass(s, kA, start, false, f);
    CHECK(first.name == WindowSet::Name::Unknown && !first.followed);
    for (uint64_t now = start + 16; now < aStop; now += 16)
    {
        Samples(s, now, onlyA);
        const bool processed = s.Processed(kA);
        RunFeedPass(s, kA, now, processed, f);
    }
    size_t bNative = 0, bFollowed = 0, bNamedOther = 0;
    for (uint64_t now = bOpen; now < bOpen + 60000; now += 16)
    {
        Samples(s, now, onlyB);
        const bool processed = s.Processed(kB);
        const FeedPass p = RunFeedPass(s, kB, now, processed, f);
        if (now == bOpen)
            continue; // the render that proves the binding moved to B
        bNative += p.followed ? 0 : 1;
        bFollowed += p.followed ? 1 : 0;
        bNamedOther += (p.named != 0 && p.named != kB) ? 1 : 0;
    }
    CHECK(s.Processed(kB));
    // One mis-name only, on B's first render: the binding never held it, so no second one follows 30 s later.
    CHECK(f.misnamed == 1);
    CHECK(f.namedWrong == 1);
    CHECK(bNamedOther == 0);
    CHECK(bNative == 0);
    CHECK(bFollowed == 3749); // every render of the minute but the first, which proved the binding moved
}

// The hold itself: a binding that named one image for a pass that finished into another one names nobody for 30 s, whatever the freshness
// says (the image that is still drawing is not named from it), and the hold belongs to that binding only. A mismatch inside the hold stretches
// its end by at most one more window from the first one, so it is never renewed endlessly; once it ends the naming rule applies again. Both
// images must still draw with the binding for the hold to start: a mismatch of a binding only one image is left on holds nothing.
void TestHoldEndsAndIsNotRenewed()
{
    WindowSet s;
    s.Configure(true, 2);
    const uint64_t t0 = 10000;
    for (const uintptr_t key : { kA, kB })
    {
        Render(s, key, t0, kShared);
        Sample(s, key, static_cast<int>(key & 3), t0);
    }
    Render(s, kA, t0, BlockOf(kA)); // A's own binding, which the mismatch on the shared one must not touch
    const uint64_t namesA = t0 + kWindowDemandMs + 1;
    Render(s, kA, namesA, kShared); // A still draws with the shared binding, B does not: it names A
    CHECK(s.NamePass(kShared, namesA).image == kA);
    const uint64_t first = t0 + 3000;
    // Both windows draw with the binding right up to the mismatch, which is how a pass of one of them is named for the other one in the game.
    Render(s, kA, first, kShared);
    Render(s, kB, first, kShared);
    CHECK(s.NoteMismatch(kShared, kA, kB, first)); // B's finisher: it drew B, not A
    // For the whole window the binding names nobody, though A's finishers keep coming (the freshness rule alone would name A again after a
    // second), and the window's own binding still names A: the hold belongs to the binding that proved it cannot tell the two apart.
    Render(s, kA, first + kWindowDemandMs + 1, kShared);
    CHECK(s.NamePass(kShared, first + kWindowDemandMs + 1).name == WindowSet::Name::Ambiguous);
    CHECK(!s.FollowsPass(s.NamePass(kShared, first + kWindowDemandMs + 1).name));
    CHECK(s.NamePass(BlockOf(kA), first + kWindowDemandMs + 1).name == WindowSet::Name::Named);
    CHECK(s.NamePass(kShared, first + kWindowMismatchMs - 1).name == WindowSet::Name::Ambiguous);
    Render(s, kA, first + kWindowMismatchMs - 1, kShared);
    const WindowSet::PassName after = s.NamePass(kShared, first + kWindowMismatchMs);
    CHECK(after.name == WindowSet::Name::Named && after.image == kA);

    // A mismatch inside the hold stretches its end, but not past one more window from the first mismatch. Both windows keep drawing with the
    // binding at each of them: only a binding that still has two live images can mis-name a pass, which is what every mismatch below proves.
    WindowSet h;
    h.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(h, key, t0, kShared);
        Sample(h, key, static_cast<int>(key & 3), t0);
    }
    Render(h, kA, t0 + kWindowDemandMs + 1, kShared);
    const uint64_t start = 20000;
    Render(h, kA, start, kShared);
    Render(h, kB, start, kShared);
    CHECK(h.NoteMismatch(kShared, kA, kB, start));                // opens the hold until start + 30 s
    Render(h, kA, start + 20000, kShared);
    Render(h, kB, start + 20000, kShared);
    CHECK(h.NoteMismatch(kShared, kA, kB, start + 20000));        // inside it: until start + 50 s
    Render(h, kA, start + 45000, kShared);
    Render(h, kB, start + 45000, kShared);
    CHECK(h.NoteMismatch(kShared, kA, kB, start + 45000));        // until start + 60 s: one more window from the first at most
    Render(h, kA, start + 55000, kShared);
    Render(h, kB, start + 55000, kShared);
    CHECK(h.NoteMismatch(kShared, kA, kB, start + 55000));        // no further
    Render(h, kA, start + 59000, kShared);                        // A never stopped drawing with the binding
    CHECK(h.NamePass(kShared, start + 60000 - 1).name == WindowSet::Name::Ambiguous);
    const WindowSet::PassName ended = h.NamePass(kShared, start + 60000);
    CHECK(ended.name == WindowSet::Name::Named && ended.image == kA); // the hold ended, and the survivor is named again

    // One image left on the binding: B stopped drawing with it, A still does, so a mismatch does not hold anything and A is named at once.
    WindowSet one;
    one.Configure(true, 2);
    for (const uintptr_t key : { kA, kB })
    {
        Render(one, key, t0, kShared);
        Sample(one, key, static_cast<int>(key & 3), t0);
    }
    Render(one, kA, t0 + 2000, kShared); // B has not drawn with the binding for longer than the demand window
    CHECK(one.NoteMismatch(kShared, kB, kA, t0 + 2000));
    const WindowSet::PassName onlyImage = one.NamePass(kShared, t0 + 2000);
    CHECK(onlyImage.name == WindowSet::Name::Named && onlyImage.image == kA);
}

// Only a finisher of another image than the one the binding named is a mismatch: a name that was right, a pass that was never named, and the
// set switched off teach nothing.
void TestMismatchNeedsTwoImages()
{
    WindowSet s;
    s.Configure(true, 1);
    Render(s, kA, 1000);
    Sample(s, kA, 0, 1000); // one place: A is processed
    CHECK(!s.NoteMismatch(BlockOf(kA), kA, kA, 1000)); // the finisher is the image the binding named
    CHECK(!s.NoteMismatch(BlockOf(kA), 0, kB, 1000));  // the pass was not named at all
    CHECK(s.NamePass(BlockOf(kA), 1000).name == WindowSet::Name::Named); // nothing was held by either
    WindowSet off;
    off.Configure(false, 1);
    CHECK(!off.NoteMismatch(BlockOf(kA), kA, kB, 1000)); // with the set off nothing is named or held
}

void TestDifferentialAgainstSlots()
{
    // With the set off, today's slot table: sixteen preview images claim slots, the seventeenth is refused.
    SlotTable table;
    for (uintptr_t i = 0; i < kMaxSlots; ++i)
    {
        const uintptr_t key = 0x1000 + i;
        table.NoteRecent(key, 1000, 1);
        CHECK(table.AcquireForConsumer(key, 1000, 576, 960) == static_cast<int>(i));
    }
    table.NoteRecent(0x9999, 1000, 1);
    CHECK(table.AcquireForConsumer(0x9999, 1000, 576, 960) == kNoSlot);

    // With the budget on and two places, only the images the set processes claim a slot: the same six renders and samples, two slots.
    WindowSet s;
    s.Configure(true, 2);
    SlotTable gated;
    int acquired = 0;
    for (uintptr_t i = 0; i < 6; ++i)
    {
        const uintptr_t key = 0x2000 + i;
        gated.NoteRecent(key, 1000, 1);
        Render(s, key, 1000);
        Sample(s, key, static_cast<int>(i), 1000);
        if (!s.Processed(key))
            continue;
        CHECK(gated.AcquireForConsumer(key, 1000, 576, 960) != kNoSlot);
        ++acquired;
    }
    CHECK(acquired == 2);

    // The same six with the set off: all six claim a slot, which is the behaviour the budget replaces.
    WindowSet off;
    off.Configure(false, 2);
    SlotTable open;
    int all = 0;
    for (uintptr_t i = 0; i < 6; ++i)
    {
        const uintptr_t key = 0x2000 + i;
        open.NoteRecent(key, 1000, 1);
        Sample(off, key, static_cast<int>(i), 1000);
        if (!off.Processed(key))
            continue;
        CHECK(open.AcquireForConsumer(key, 1000, 576, 960) != kNoSlot);
        ++all;
    }
    CHECK(all == 6);
}

void TestWaitingCandidate()
{
    WindowSet s;
    s.Configure(true, 1);
    CHECK(!s.HasWaitingCandidate(1000));
    Render(s, kA, 1000);
    Sample(s, kA, 0, 1000);
    CHECK(!s.HasWaitingCandidate(1100)); // nothing else wants a place
    Sample(s, kB, 1, 1100);             // A is live, so B waits
    CHECK(s.HasWaitingCandidate(1100));
    CHECK(s.HasWaitingCandidate(2000)); // B's demand is still within the 1000 ms window
    CHECK(!s.HasWaitingCandidate(2101));
    CHECK(s.Processed(kA));
    // with a free place there is no pressure
    WindowSet roomy;
    roomy.Configure(true, 2);
    Render(roomy, kA, 1000);
    Sample(roomy, kA, 0, 1000);
    CHECK(!roomy.HasWaitingCandidate(1000));
}
} // namespace

int main()
{
    TestOffChangesNothing();
    TestCapRange();
    TestRenderingIsRequired();
    TestAdmissionAndHysteresis();
    TestStallHysteresis();
    TestStallNeedsARender();
    TestTieBreaks();
    TestIdentity();
    TestLearnsEveryFinisher();
    TestAmbiguityClears();
    TestInvalidBinding();
    TestThirdImage();
    TestFeedActiveAndSparse();
    TestFeedBothDrawing();
    TestFeedOutsideTheSet();
    TestFeedWindowTakesTheBindingOver();
    TestHoldEndsAndIsNotRenewed();
    TestMismatchNeedsTwoImages();
    TestCadences();
    TestFreshCount();
    TestDifferentialAgainstSlots();
    TestWaitingCandidate();
    return TEST_RESULT("chara_window_set");
}
