#include "check.h"
#include "ext/ffxiv/chara/CharaMotionRules.h"

#include <memory>
#include <string>

using namespace ext::ffxiv::chara;
using namespace ext::ffxiv::chara::motion;

namespace
{
// A G-buffer draw that has every property the replay needs, with a tagged shader.
ReplayFacts Tagged()
{
    ReplayFacts f;
    f.rtvBound = { true, true, true, true, true };
    f.rtvEntry = { 8, 9, 10, 11, 12 };
    f.dsvBound = true;
    f.psGeometryTag = true;
    return f;
}

// The same draw with an untagged shader that has the G-buffer shape.
ReplayFacts Untagged()
{
    ReplayFacts f = Tagged();
    f.psGeometryTag = false;
    f.vsCb0Bytes = 0x3b0;
    f.psCb2Bytes = 0x3b0;
    return f;
}

void TestTagged()
{
    ReplayDecision d = DecideReplay(Tagged());
    CHECK(d.replay && !d.tagShader && d.skip == Skip::None);

    // a tagged shader is replayed without looking at the constant buffers
    ReplayFacts f = Tagged();
    f.vsCb0Bytes = 1;
    f.psCb2Bytes = 2;
    CHECK(DecideReplay(f).replay);

    // ... and without the other targets being entries 9..12
    f = Tagged();
    f.rtvEntry = { 8, -1, -1, -1, -1 };
    f.rtvBound = { true, false, false, false, true };
    CHECK(DecideReplay(f).replay);
}

void TestSkipConditions()
{
    ReplayFacts f = Tagged();
    f.ownDraw = true;
    CHECK(DecideReplay(f).skip == Skip::OwnDraw);

    f = Tagged();
    f.immediate = false;
    CHECK(DecideReplay(f).skip == Skip::NotImmediate);

    f = Tagged();
    f.openQueries = 1;
    CHECK(DecideReplay(f).skip == Skip::OpenQuery);
    f = Tagged();
    f.replayAllowedByQueries = false;
    CHECK(DecideReplay(f).skip == Skip::OpenQuery);

    f = Tagged();
    f.rtvBound[0] = false;
    CHECK(DecideReplay(f).skip == Skip::MissingTargets);
    f = Tagged();
    f.rtvBound[4] = false;
    CHECK(DecideReplay(f).skip == Skip::MissingTargets);
    f = Tagged();
    f.dsvBound = false;
    CHECK(DecideReplay(f).skip == Skip::MissingTargets);

    // only the opaque G-buffer is replayed (the transparent one, entry 13, is not)
    f = Tagged();
    f.rtvEntry[0] = 13;
    CHECK(DecideReplay(f).skip == Skip::NotOpaqueGBuffer);
    f = Tagged();
    f.rtvEntry[0] = kEntryNone;
    CHECK(DecideReplay(f).skip == Skip::NotOpaqueGBuffer);

    f = Tagged();
    f.rtv0Samples = 4;
    CHECK(DecideReplay(f).skip == Skip::MultiSample);

    // unordered access views and stream-out targets: the draw only runs natively
    f = Tagged();
    f.anyUav = true;
    CHECK(DecideReplay(f).skip == Skip::UnorderedAccess);
    f = Tagged();
    f.anyStreamOut = true;
    CHECK(DecideReplay(f).skip == Skip::StreamOut);

    // an open query beats everything else that would allow the replay
    f = Untagged();
    f.openQueries = 2;
    CHECK(!DecideReplay(f).replay);
}

void TestUntaggedShape()
{
    ReplayDecision d = DecideReplay(Untagged());
    CHECK(d.replay && d.tagShader); // tagged on the spot

    ReplayFacts f = Untagged();
    f.vsCb0Bytes = 0x3b4;
    CHECK(DecideReplay(f).skip == Skip::UnknownShader);
    f = Untagged();
    f.psCb2Bytes = 0x3af;
    CHECK(DecideReplay(f).skip == Skip::UnknownShader);
    f = Untagged();
    f.rtvEntry[3] = 10; // targets 0..4 must be entries 8..12 in order
    CHECK(DecideReplay(f).skip == Skip::UnknownShader);
    f = Untagged();
    f.rtvBound[2] = false;
    CHECK(DecideReplay(f).skip == Skip::UnknownShader);
}

// The wider shape rule (PreviewReplayWideShape): the camera block may be a view into a larger buffer, so any size the camera snapshot
// accepts is enough, for both buffers; the render targets must still be entries 8..12 in order and the shader is still untagged.
void TestWideShape()
{
    // the exact shape is accepted with the rule off and on
    for (const bool wide : { false, true })
    {
        ReplayFacts f = Untagged();
        f.wideShape = wide;
        ReplayDecision d = DecideReplay(f);
        CHECK(d.replay && d.tagShader && d.skip == Skip::None);
    }
    // a 1024-byte camera block: refused exactly, accepted wide (the sizes this fork's game build reports for the G-buffer pixel shader)
    {
        ReplayFacts f = Untagged();
        f.psCb2Bytes = 1024;
        CHECK(DecideReplay(f).skip == Skip::UnknownShader);
        f.wideShape = true;
        CHECK(DecideReplay(f).replay && DecideReplay(f).skip == Skip::None);
        // only the exact block proves that the shader reads the camera block, so a wider one is replayed without tagging it: a tag taken
        // from a buffer that merely fits would follow that shader for every draw it makes, for the rest of the session
        CHECK(!DecideReplay(f).tagShader);
    }
    // both buffers have to pass: a wide pixel buffer with an exact-size vertex buffer is accepted, an out-of-range one is not
    {
        ReplayFacts f = Untagged();
        f.wideShape = true;
        f.vsCb0Bytes = 0x3b0;
        f.psCb2Bytes = 0x3b0 + 0xFC50; // the top of the range
        CHECK(DecideReplay(f).replay);
        f.psCb2Bytes = 0x3b0 + 0xFC50 + 1; // one byte above it
        CHECK(DecideReplay(f).skip == Skip::UnknownShader);
        f.psCb2Bytes = 0x3b0;
        f.vsCb0Bytes = 0x3af; // one byte below the minimum
        CHECK(DecideReplay(f).skip == Skip::UnknownShader);
        f.vsCb0Bytes = 0x3b4; // not a whole number of vectors: inside the range, so accepted wide
        CHECK(DecideReplay(f).replay);
        f.vsCb0Bytes = 0; // no constant buffer bound
        CHECK(DecideReplay(f).skip == Skip::UnknownShader);
        f.vsCb0Bytes = 0x3b0;
        f.psCb2Bytes = 0;
        CHECK(DecideReplay(f).skip == Skip::UnknownShader);
    }
    // the rule changes the size test only: the render target requirement and the other skips stay
    {
        ReplayFacts f = Untagged();
        f.wideShape = true;
        f.psCb2Bytes = 1024;
        f.rtvEntry[3] = 10;
        CHECK(DecideReplay(f).skip == Skip::UnknownShader);
        f = Untagged();
        f.wideShape = true;
        f.psCb2Bytes = 1024;
        f.anyUav = true;
        CHECK(DecideReplay(f).skip == Skip::UnorderedAccess);
        f = Untagged();
        f.wideShape = true;
        f.psCb2Bytes = 1024;
        f.psGeometryTag = true; // a tagged shader is replayed without looking at the buffers
        CHECK(DecideReplay(f).replay && !DecideReplay(f).tagShader);
    }
    // the camera rule itself: the range Accept uses, without the bound range
    CHECK(camera::SizeAccepted(0x3b0) && camera::SizeAccepted(1024) && camera::SizeAccepted(0x3b0 + 0xFC50));
    CHECK(!camera::SizeAccepted(0x3af) && !camera::SizeAccepted(0) && !camera::SizeAccepted(0x3b0 + 0xFC50 + 1));
}

// The guides of a pass come from a G-buffer draw whose shader carries the geometry role, or from a draw the replay accepted. The wider shape is
// replayed without being tagged, so without the second half a pass whose opaque draws are all of the wider shape would never have a camera and
// could not finish.
void TestGuideCapture()
{
    CHECK(CapturesGuides(true, false));  // a shader already known as a G-buffer shader
    CHECK(CapturesGuides(true, true));
    CHECK(CapturesGuides(false, true));  // a wider-shape shader the replay accepted (and did not tag)
    CHECK(!CapturesGuides(false, false)); // an opaque draw the replay refused: nothing proves it reads the camera block
}

void TestDepth()
{
    CHECK(IsReversedZ(true, D3D11_COMPARISON_GREATER));
    CHECK(IsReversedZ(true, D3D11_COMPARISON_GREATER_EQUAL));
    CHECK(!IsReversedZ(true, D3D11_COMPARISON_LESS));
    CHECK(!IsReversedZ(true, D3D11_COMPARISON_LESS_EQUAL));
    CHECK(!IsReversedZ(true, D3D11_COMPARISON_ALWAYS));
    CHECK(!IsReversedZ(false, D3D11_COMPARISON_GREATER)); // no state bound: not reversed

    // What one bound state says about the convention, and the memo that keeps the last decisive answer
    CHECK(ConventionOf(true, true, D3D11_COMPARISON_GREATER) == DepthConvention::Reversed);
    CHECK(ConventionOf(true, true, D3D11_COMPARISON_GREATER_EQUAL) == DepthConvention::Reversed);
    CHECK(ConventionOf(true, true, D3D11_COMPARISON_LESS) == DepthConvention::Normal);
    CHECK(ConventionOf(true, true, D3D11_COMPARISON_LESS_EQUAL) == DepthConvention::Normal);
    CHECK(ConventionOf(false, false, D3D11_COMPARISON_LESS) == DepthConvention::Normal); // no state = the default state
    // draws that say nothing: depth test off, or a function that does not order depths
    CHECK(ConventionOf(true, false, D3D11_COMPARISON_GREATER) == DepthConvention::Unknown);
    CHECK(ConventionOf(true, false, D3D11_COMPARISON_LESS) == DepthConvention::Unknown);
    for (const auto f : { D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_NEVER, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_NOT_EQUAL })
        CHECK(ConventionOf(true, true, f) == DepthConvention::Unknown);
    {
        DepthConventionMemo m;
        CHECK(!m.Known() && !m.Reversed());
        m.Observe(ConventionOf(true, true, D3D11_COMPARISON_ALWAYS)); // a masking draw first: says nothing
        CHECK(!m.Known() && !m.Reversed());
        m.Observe(ConventionOf(true, true, D3D11_COMPARISON_GREATER_EQUAL));
        CHECK(m.Known() && m.Reversed());
        m.Observe(ConventionOf(true, false, D3D11_COMPARISON_LESS)); // a later draw with the test off does not undo it
        m.Observe(ConventionOf(true, true, D3D11_COMPARISON_ALWAYS));
        CHECK(m.Reversed());
        m.Observe(ConventionOf(true, true, D3D11_COMPARISON_LESS)); // a decisive answer replaces it
        CHECK(m.Known() && !m.Reversed());
        m.Reset();
        CHECK(!m.Known());
    }

    D3D11_DEPTH_STENCIL_DESC src {};
    src.DepthEnable = TRUE;
    src.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    src.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    src.StencilEnable = TRUE;
    src.StencilReadMask = 0x0F;
    src.StencilWriteMask = 0xF0;
    D3D11_DEPTH_STENCIL_DESC d = ReplayDepthDesc(true, src);
    CHECK(d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
    CHECK(d.StencilWriteMask == 0);
    CHECK(d.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL); // same function as the game state
    CHECK(d.StencilReadMask == 0x0F);
    CHECK(d.DepthEnable == TRUE && d.StencilEnable == TRUE);

    d = ReplayDepthDesc(false, src);
    CHECK(d.DepthEnable == TRUE && d.DepthFunc == D3D11_COMPARISON_LESS);
    CHECK(d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
    CHECK(d.StencilReadMask == 0xFF && d.StencilWriteMask == 0);
    CHECK(d.StencilEnable == FALSE);
}

void TestDepthCache()
{
    // A reference-counted value (the real cache holds ComPtr) survives the insert.
    {
        DepthStateCache<std::shared_ptr<int>, 2> shared;
        D3D11_DEPTH_STENCIL_DESC d {};
        std::shared_ptr<int> original = std::make_shared<int>(7);
        std::shared_ptr<int>* stored = shared.Insert(0x40, d, original);
        CHECK(stored != nullptr && *stored == original && original.use_count() == 2);
        CHECK(shared.Find(0x40, d) != nullptr && *shared.Find(0x40, d) == original);
    }

    // ComPtr overloads operator& (it releases the pointer): the cache must never take the address of a value with it.
    {
        struct Guarded
        {
            int* released = nullptr;
            int payload = 0;
            Guarded* operator&()
            {
                if (released != nullptr)
                    ++*released;
                return this;
            }
        };
        int releases = 0;
        DepthStateCache<Guarded, 2> guarded;
        D3D11_DEPTH_STENCIL_DESC d {};
        Guarded g;
        g.released = &releases;
        g.payload = 5;
        Guarded* stored = guarded.Insert(0x50, d, g);
        CHECK(stored != nullptr && stored->payload == 5);
        CHECK(guarded.Find(0x50, d) != nullptr);
        CHECK(releases == 0);
    }

    DepthStateCache<int, 2> cache;
    D3D11_DEPTH_STENCIL_DESC a {};
    a.DepthEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC b = a;
    b.DepthFunc = D3D11_COMPARISON_GREATER;

    CHECK(cache.Find(0x10, a) == nullptr);
    CHECK(cache.Insert(0x10, a, 1) != nullptr);
    CHECK(cache.Find(0x10, a) != nullptr && *cache.Find(0x10, a) == 1);

    // the address is reused by a different state: the cached copy is not served
    CHECK(cache.Find(0x10, b) == nullptr);
    CHECK(*cache.Insert(0x10, b, 2) == 2); // replaces the stale entry in place
    CHECK(cache.Size() == 1);

    CHECK(cache.Insert(0x20, a, 3) != nullptr);
    CHECK(cache.Insert(0x30, a, 4) == nullptr); // full: the caller leaves the draw native
    CHECK(cache.Size() == 2);
    cache.Clear();
    CHECK(cache.Size() == 0);
}

void TestQueries()
{
    CHECK(QueryTypeCounts(D3D11_QUERY_OCCLUSION));
    CHECK(QueryTypeCounts(D3D11_QUERY_PIPELINE_STATISTICS));
    CHECK(QueryTypeCounts(D3D11_QUERY_OCCLUSION_PREDICATE));
    CHECK(QueryTypeCounts(D3D11_QUERY_SO_STATISTICS));
    CHECK(QueryTypeCounts(D3D11_QUERY_SO_OVERFLOW_PREDICATE));
    CHECK(!QueryTypeCounts(D3D11_QUERY_EVENT));
    CHECK(!QueryTypeCounts(D3D11_QUERY_TIMESTAMP));
    CHECK(!QueryTypeCounts(D3D11_QUERY_TIMESTAMP_DISJOINT));

    OpenQuerySet s;
    CHECK(s.Blind() && !s.ReplayAllowed()); // queries opened before the module started are unknown
    s.OnFrameEnd();
    CHECK(s.ReplayAllowed());

    s.Begin(0x100, true);
    CHECK(s.Count() == 1 && !s.ReplayAllowed());
    s.Begin(0x100, true); // a second Begin of the same query does not count twice
    CHECK(s.Count() == 1);
    s.End(0x100);
    CHECK(s.Count() == 0 && s.ReplayAllowed());

    s.Begin(0x200, false); // timestamp-like queries are ignored
    CHECK(s.Count() == 0 && s.ReplayAllowed());
    s.End(0x300); // End without Begin
    CHECK(s.Count() == 0 && s.ReplayAllowed());
    s.Begin(0, true); // an unidentifiable null is ignored
    CHECK(s.Count() == 0);

    s.Begin(0x400, true);
    s.Begin(0x500, true);
    s.End(0x400);
    CHECK(s.Count() == 1 && !s.ReplayAllowed());
    s.Clear(); // the module restarted
    CHECK(s.Count() == 0 && s.Blind() && !s.ReplayAllowed());

    // Without a frame callback the gate lifts after 100 ms of maintenance calls.
    s.Maintain(5000); // starts the clock
    CHECK(s.Blind());
    s.Maintain(5099);
    CHECK(s.Blind());
    s.Maintain(5100);
    CHECK(!s.Blind() && s.ReplayAllowed());
    s.Begin(0x600, true);
    s.MarkBlind(); // the module restarted: the clock starts again
    s.Maintain(9000);
    s.Maintain(9050);
    CHECK(s.Blind());
    s.OnFrameEnd(); // a frame callback lifts it at once
    CHECK(!s.Blind());
    // The query that was open when the hooks stopped being fed never sees its End: marking the set blind forgets it, so the replay is
    // not refused for the rest of the session.
    CHECK(s.Count() == 0 && s.ReplayAllowed());

    // the same when the path simply goes inactive and active again with a query straddling the change
    s.Begin(0x700, true);
    CHECK(!s.ReplayAllowed());
    s.MarkBlind(); // the path became inactive (or active) with 0x700 open
    s.OnFrameEnd();
    CHECK(s.Count() == 0 && s.ReplayAllowed());
}
} // namespace

int main()
{
    TestTagged();
    TestSkipConditions();
    TestUntaggedShape();
    TestWideShape();
    TestGuideCapture();
    TestDepth();
    TestDepthCache();
    TestQueries();
    return TEST_RESULT("chara_motion_rules");
}
