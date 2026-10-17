#include "check.h"
#include "ext/ffxiv/chara/CharaMotionRules.h"

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

void TestDepth()
{
    CHECK(IsReversedZ(true, D3D11_COMPARISON_GREATER));
    CHECK(IsReversedZ(true, D3D11_COMPARISON_GREATER_EQUAL));
    CHECK(!IsReversedZ(true, D3D11_COMPARISON_LESS));
    CHECK(!IsReversedZ(true, D3D11_COMPARISON_LESS_EQUAL));
    CHECK(!IsReversedZ(true, D3D11_COMPARISON_ALWAYS));
    CHECK(!IsReversedZ(false, D3D11_COMPARISON_GREATER)); // no state bound: not reversed

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
}
} // namespace

int main()
{
    TestTagged();
    TestSkipConditions();
    TestUntaggedShape();
    TestDepth();
    TestDepthCache();
    TestQueries();
    return TEST_RESULT("chara_motion_rules");
}
