// Automatic ReShade G-buffers, the binder side (ReShadeDepthCore.cpp, the slots of NORMALS, DECALS, DECALS_WATER, MOTIONVECTORS and
// MATERIAL) against the fake ReShade backend of reshade_depth_fake.h: the decision per buffer (remap or keep the add-on's binding),
// the request the backend gets, the bind and the restore at finish (only if the entry is still ours), the unverified undo, a restore
// that fails and is retried, reference balance, the rule that nothing is done without a bound depth, and zero extra backend calls
// when the effects declare no G-buffer.
#include "pch.h"

#include "reshade_depth_fake.h"

#include <cstdio>
#include <string>

using namespace depthtest;

namespace
{
constexpr uint64_t kCtx = 0xC7C7;
constexpr uint64_t kRestN = 0xD1, kRestNs = 0xD2;   // the add-on's NORMALS binding
constexpr uint64_t kRestMv = 0xD3, kRestMvs = 0xD4; // ... and MOTIONVECTORS
constexpr uint64_t kVarN = 0x701, kVarMv = 0x702, kVarN2 = 0x703;
constexpr uint32_t kRgba8 = 28, kRg16f = 34;

ViewDesc Plain(uint32_t w, uint32_t h, uint32_t format)
{
    ViewDesc d;
    d.ok = d.plain2D = true;
    d.width = w;
    d.height = h;
    d.format = format;
    return d;
}

// A rig with the depth input, texNormals and texNativeMotionVectors in the effects, the add-on's bindings in place (output-sized, as
// REST's are) and a fresh capture of 1920x1080 inside 3840x2160 every frame.
struct GRig
{
    Rig r;
    DepthBinder& binder = r.binder;
    Fake& fake = r.fake;

    GRig()
    {
        fake.vars.push_back({ kVarN, "texNormals", "NORMALS", 0, 0 });
        fake.vars.push_back({ kVarMv, "texNativeMotionVectors", "MOTIONVECTORS", 0, 0 });
        for (uint64_t v : { kRestN, kRestNs, kRestMv, kRestMvs })
            fake.owner[v] = 1;
        fake.sem["NORMALS"] = { kRestN, kRestNs };
        fake.sem["MOTIONVECTORS"] = { kRestMv, kRestMvs };
        fake.described[kRestN] = Plain(3840, 2160, kRgba8);
        fake.described[kRestMv] = Plain(3840, 2160, kRg16f);
    }
    Outcome Begin() { return binder.OnBeginEffects(kRt, kCtx); }
    void Finish() { binder.OnFinishEffects(kRt); }
    bool Both(uint64_t n, uint64_t mv) const { return fake.sem.at("NORMALS").first == n && fake.sem.at("MOTIONVECTORS").first == mv; }
};

void Realigns()
{
    GRig g;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    // both buffers were remapped with the render window of the capture, the output size and the capture counter as the stamp
    CHECK(g.fake.remaps.size() == 2);
    if (g.fake.remaps.size() == 2)
    {
        const RemapRequest& n = g.fake.remaps[0];
        const RemapRequest& m = g.fake.remaps[1];
        CHECK(n.buffer == GBuffer::Normals && n.srv == kRestN && n.srvSrgb == kRestNs && n.context == kCtx);
        CHECK(n.renderW == 1920 && n.renderH == 1080 && n.outputW == 3840 && n.outputH == 2160 && n.stamp == g.fake.frame);
        CHECK(m.buffer == GBuffer::MotionVectors && m.srv == kRestMv && m.srvSrgb == kRestMvs && m.scaleMotion);
    }
    CHECK(g.fake.remapMakes == 2);
    // the semantics now hold the copies (not the add-on's views) until finish
    const auto n = g.fake.sem.at("NORMALS"), m = g.fake.sem.at("MOTIONVECTORS");
    CHECK(n.first != kRestN && g.fake.copyViews.count(n.first) && m.first != kRestMv && g.fake.copyViews.count(m.first));
    CHECK(g.fake.DepthIs(kCopy, kCopy)); // the depth is bound as before
    CHECK(g.binder.RealignedMask() == ((1u << static_cast<uint32_t>(GBuffer::Normals)) | (1u << static_cast<uint32_t>(GBuffer::MotionVectors))));
    CHECK(g.binder.GetCounters(GBuffer::Normals).passesBound == 1 && g.binder.GetCounters(GBuffer::MotionVectors).passesBound == 1);
    CHECK(g.binder.GetCounters(GBuffer::Decals).passesBound == 0 && g.binder.GetCounters().passesBound == 1);
    // the pass holds the copies (and the originals) for as long as they are bound
    CHECK(g.fake.CoreRefs(n.first) == 1 && g.fake.CoreRefs(m.first) == 1 && g.fake.CoreRefs(kRestN) == 1 && g.fake.CoreRefs(kRestNs) == 1);

    g.Finish();
    CHECK(g.Both(kRestN, kRestMv) && g.fake.sem.at("NORMALS").second == kRestNs && g.fake.sem.at("MOTIONVECTORS").second == kRestMvs);
    CHECK(!g.fake.HoldsCopy() && g.fake.Balanced());
    CHECK(g.fake.DepthIs(kOrig, kOrigSrgb));
    // the decision was told once, with the sizes
    CHECK(g.fake.notes.size() == 2 && g.fake.notes[0].decision == RemapDecision::Remap && g.fake.notes[0].status == RemapStatus::Ok);
    CHECK(g.fake.notes[0].source.width == 3840 && g.fake.notes[0].renderW == 1920 && g.fake.notes[0].outputW == 3840);

    // a second pass in the same real frame (frame generation): the backend is asked again with the same stamp (its cache answers), the
    // copy is the same, the pass is bound and restored again
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.fake.remaps.size() == 4 && g.fake.remaps[2].stamp == g.fake.remaps[0].stamp && g.fake.remapMakes == 2);
    g.Finish();
    CHECK(g.Both(kRestN, kRestMv) && g.fake.notes.size() == 2 && g.fake.Balanced());

    // a new real frame makes a new copy; the old one is released by the backend while the core's references are gone already
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.fake.remapMakes == 4);
    g.Finish();
    CHECK(g.Both(kRestN, kRestMv) && g.fake.Balanced() && g.fake.violations == 0);
}

void SrgbTwinAndRefs()
{
    GRig g;
    g.fake.remapTwin = true; // the copy has an sRGB twin: two views, two references
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    const auto n = g.fake.sem.at("NORMALS");
    CHECK(n.first != n.second && g.fake.CoreRefs(n.first) == 1 && g.fake.CoreRefs(n.second) == 1);
    g.Finish();
    CHECK(g.Both(kRestN, kRestMv) && !g.fake.HoldsCopy() && g.fake.Balanced());
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.Finish();
    CHECK(g.fake.Balanced() && g.fake.violations == 0);
}

// The size rule: only an output-sized source, with a render window smaller than the output, is remapped.
void Keeps()
{
    struct Row
    {
        const char* what;
        RemapDecision decision;
    };
    auto run = [](auto&& arrange, RemapDecision expected, const char* what, int expectRemaps)
    {
        GRig g;
        arrange(g);
        g.r.NextFrame();
        const Outcome o = g.Begin();
        if (o != Outcome::Bound)
            std::printf("  %s: outcome %d\n", what, int(o));
        CHECK(o == Outcome::Bound); // the depth is bound whatever the buffers do
        const auto last = g.fake.notes.empty() ? RemapDecision::Remap : g.fake.notes[0].decision;
        if (last != expected)
            std::printf("  %s: decision %d, expected %d\n", what, int(last), int(expected));
        CHECK(!g.fake.notes.empty() && last == expected);
        CHECK(int(g.fake.remaps.size()) == expectRemaps);
        // the add-on's binding was left alone: not written, not erased
        CHECK(g.fake.sem.count("NORMALS") == 0 || g.fake.sem.at("NORMALS").first == kRestN || expected == RemapDecision::KeepNoSource);
        for (const auto& u : g.fake.updates)
            CHECK(u.semantic == "DEPTH" || u.semantic == "MOTIONVECTORS");
        CHECK(g.binder.GetCounters(GBuffer::Normals).kept == 1 && g.binder.GetCounters(GBuffer::Normals).passesBound == 0);
        CHECK((g.binder.RealignedMask() & 1u) == 0);
        g.Finish();
        CHECK(g.fake.Balanced());
    };
    // render == output: nothing to realign
    {
        GRig g;
        DepthCapture c = g.r.Capture();
        c.renderW = 3840;
        c.renderH = 2160;
        g.binder.PublishCapture(c);
        CHECK(g.Begin() == Outcome::Bound);
        CHECK(g.fake.remaps.empty() && g.fake.notes.size() == 2 && g.fake.notes[0].decision == RemapDecision::KeepSameSize);
        CHECK(g.binder.GetCounters(GBuffer::Normals).kept == 1 && g.binder.GetCounters(GBuffer::MotionVectors).kept == 1);
        CHECK(g.Both(kRestN, kRestMv));
        g.Finish();
        CHECK(g.fake.Balanced());
    }
    // the source is already render-sized
    run([](GRig& g) { g.fake.described[kRestN] = Plain(1920, 1080, kRgba8); }, RemapDecision::KeepRenderSized, "render-sized", 1);
    // any other size
    run([](GRig& g) { g.fake.described[kRestN] = Plain(1000, 700, kRgba8); }, RemapDecision::KeepSizeMismatch, "other size", 1);
    // not a plain 2D view / unreadable
    run([](GRig& g) { ViewDesc d = Plain(3840, 2160, kRgba8); d.plain2D = false; g.fake.described[kRestN] = d; }, RemapDecision::KeepUnreadable, "not plain", 1);
    run([](GRig& g) { g.fake.described.erase(kRestN); }, RemapDecision::KeepUnreadable, "unreadable", 1);
    // nothing bound yet
    run([](GRig& g) { g.fake.sem.erase("NORMALS"); }, RemapDecision::KeepNoSource, "nothing bound", 1);
    // two variables of that name that show different bindings: some are not readers of the semantic
    run([](GRig& g) { g.fake.vars.push_back({ kVarN2, "texNormals", "", 0xE1, 0xE1 }); }, RemapDecision::KeepAmbiguous, "ambiguous", 1);
    // the backend does not remap this format: kept, counted as kept (not as a failure)
    run([](GRig& g) { g.fake.remapStatus = RemapStatus::UnsupportedFormat; }, RemapDecision::Remap, "unsupported", 2);
}

void BackendRefuses()
{
    for (const RemapStatus status : { RemapStatus::UnsupportedFormat, RemapStatus::NotPlain2D, RemapStatus::NoContext, RemapStatus::Refused, RemapStatus::Failed })
    {
        GRig g;
        g.fake.remapStatus = status;
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        const Counters c = g.binder.GetCounters(GBuffer::Normals);
        CHECK(status == RemapStatus::Failed ? (c.failed == 1 && c.kept == 0) : (c.kept == 1 && c.failed == 0));
        CHECK(g.fake.sem.at("NORMALS").first == kRestN && (g.binder.RealignedMask() & 1u) == 0);
        CHECK(g.fake.notes.size() == 2 && g.fake.notes[0].status == status);
        g.Finish();
        CHECK(g.fake.Balanced() && g.binder.GetCounters().failed == 0);
    }
}

// No depth bound, no G-buffer work: a manual transform, no capture, a stale capture.
void NothingWithoutDepth()
{
    {
        GRig g;
        g.fake.globalDefs["RESHADE_DEPTH_INPUT_X_SCALE"] = "2";
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::ManualTransform);
        CHECK(g.fake.remaps.empty() && g.fake.Calls("DescribeView") == 0 && g.binder.RealignedMask() == 0);
        CHECK(g.Both(kRestN, kRestMv) && g.fake.updates.empty());
    }
    {
        GRig g; // the capture side has nothing yet
        g.binder.PublishCapture(DepthCapture {});
        CHECK(g.Begin() == Outcome::NoCapture);
        CHECK(g.fake.remaps.empty() && g.binder.RealignedMask() == 0);
    }
    {
        GRig g;
        g.r.NextFrame();
        g.fake.now += 1000; // the capture is stale
        CHECK(g.Begin() == Outcome::CaptureUnavailable);
        CHECK(g.fake.remaps.empty() && g.Both(kRestN, kRestMv));
    }
}

// Effects that declare no G-buffer: not one backend call more than before.
void NoGBufferNoCalls()
{
    Rig r;
    r.NextFrame();
    CHECK(r.Begin() == Outcome::Bound);
    r.Finish();
    int more = 0;
    for (const char* what : { "DescribeView", "Remap", "ReleaseRemaps" })
        more += r.fake.Calls(what);
    CHECK(more == 0 && r.fake.notes.empty() && r.fake.remaps.empty());
    CHECK(r.fake.bindingReads.size() == 1 && r.fake.bindingReads.count(kDepthVar) == 1); // only the depth variable was ever read
    CHECK(r.fake.textureEnums == 1);
    CHECK(r.binder.GetCounters(GBuffer::Normals).kept == 0 && r.binder.RealignedMask() == 0);
}

// The restore at finish: only if the entry is still ours.
void RestoreRules()
{
    {
        GRig g; // another add-on binds NORMALS after us: its choice stays and the counter says it
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        g.fake.sem["NORMALS"] = { 0xF1, 0xF2 };
        g.fake.owner[0xF1] = g.fake.owner[0xF2] = 1;
        g.Finish();
        CHECK(g.fake.sem.at("NORMALS").first == 0xF1 && g.fake.sem.at("MOTIONVECTORS").first == kRestMv);
        CHECK(g.binder.GetCounters(GBuffer::Normals).overwrittenByOther == 1 && g.binder.GetCounters(GBuffer::MotionVectors).overwrittenByOther == 0);
        CHECK(g.fake.Balanced());
    }
    {
        GRig g; // a restore that fails keeps the references and is retried first by the next begin
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        g.fake.failRestore = true;
        g.Finish();
        CHECK(g.binder.GetCounters(GBuffer::Normals).failed == 1 && g.binder.GetCounters(GBuffer::MotionVectors).failed == 1);
        CHECK(g.fake.HoldsCopy() && g.fake.BoundCopiesReferenced()); // still bound, still referenced
        g.fake.failRestore = false;
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound); // the retry restored, then the pass bound again
        g.Finish();
        CHECK(g.Both(kRestN, kRestMv) && !g.fake.HoldsCopy() && g.fake.Balanced() && g.fake.violations == 0);
    }
    {
        GRig g; // the effects reload while bound: the variables are looked up again and the original goes back
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        g.binder.OnReloadedEffects(kRt);
        CHECK(g.Both(kRestN, kRestMv) && !g.fake.HoldsCopy() && g.fake.Balanced());
        g.Finish();
        CHECK(g.fake.Balanced());
    }
}

// No variable of that name reads the semantic: the bind is erased (never what a decoy showed), the slot stops until a reload.
void Unverified()
{
    GRig g;
    g.fake.vars.clear();
    g.fake.vars.push_back({ kDepthVar, "DepthBufferTex", "DEPTH", 0, 0 });
    g.fake.vars.push_back({ kVarN, "texNormals", "", 0xE1, 0xE2 }); // a decoy: right name, no semantic, its own texture
    g.fake.owner[0xE2] = 1;
    g.fake.described[0xE1] = Plain(3840, 2160, kRgba8);
    g.fake.sem.clear();
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.fake.remaps.size() == 1);
    const Counters c = g.binder.GetCounters(GBuffer::Normals);
    CHECK(c.erasedUnverified == 1 && c.noDepthInput == 1 && c.passesBound == 0);
    CHECK(g.fake.sem.count("NORMALS") == 0); // erased
    for (const auto& u : g.fake.updates)
        CHECK(!(u.semantic == "NORMALS" && (u.srv == 0xE1 || u.srgb == 0xE2))); // never what the decoy showed
    CHECK(g.fake.vars.back().srv == 0xE1 && g.fake.vars.back().srgb == 0xE2);
    g.Finish();
    CHECK(g.fake.Balanced() && !g.fake.HoldsCopy());
    // not tried again until the effects reload
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.fake.remaps.size() == 1 && g.binder.GetCounters(GBuffer::Normals).noDepthInput == 2);
    g.Finish();
    g.binder.OnReloadedEffects(kRt);
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.fake.remaps.size() == 2);
    g.Finish();
    CHECK(g.fake.Balanced() && g.binder.GetCounters().failed == 0);
}

// A slot that fails never fails the depth.
void SlotFaultsStayInTheSlot()
{
    GRig g;
    g.fake.remapStatus = RemapStatus::Failed;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound && g.binder.GetCounters().failed == 0 && g.binder.GetCounters().passesBound == 1);
    g.Finish();
    // a throw inside the slot (the backend threw while binding): rolled back, counted in the slot, the depth stays bound
    GRig h;
    h.r.NextFrame();
    int seen = 0;
    h.fake.afterCall = [&](const char* name)
    {
        if (std::string(name) == "Remap" && ++seen == 1)
            throw std::runtime_error("after remap");
    };
    CHECK(h.Begin() == Outcome::Bound);
    CHECK(h.binder.GetCounters(GBuffer::Normals).failed == 1 && h.binder.GetCounters().failed == 0);
    CHECK(h.fake.DepthIs(kCopy, kCopy));
    h.fake.afterCall = nullptr;
    h.Finish();
    CHECK(h.fake.DepthIs(kOrig, kOrigSrgb) && h.fake.Balanced());
}

// The option goes off: the next begin drops the remap targets once.
void Disabled()
{
    GRig g;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.Finish();
    g.binder.SetEnabled(false);
    CHECK(g.Begin() == Outcome::Disabled && g.fake.releaseRemaps == 1);
    CHECK(g.Begin() == Outcome::Disabled && g.fake.releaseRemaps == 1);
    CHECK(g.Both(kRestN, kRestMv));
    // never remapped: nothing to release
    GRig h;
    h.binder.SetEnabled(false);
    CHECK(h.Begin() == Outcome::Disabled && h.fake.releaseRemaps == 0);
}

// Whether the add-on rebinds between frames is told once.
void RebindIsObserved()
{
    GRig g;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.Finish();
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound); // the same view is there again
    g.Finish();
    CHECK(g.fake.observed.empty());
    g.fake.sem["NORMALS"] = { 0xD5, 0xD6 }; // a new view between frames
    g.fake.owner[0xD5] = g.fake.owner[0xD6] = 1;
    g.fake.described[0xD5] = Plain(3840, 2160, kRgba8);
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.Finish();
    CHECK(g.fake.observed.size() == 1 && g.fake.observed[0].first == GBuffer::Normals && g.fake.observed[0].second == GBufferEvent::ViewChanged);
    g.fake.sem["NORMALS"] = { 0xD7, 0xD8 }; // and again: said once
    g.fake.owner[0xD7] = g.fake.owner[0xD8] = 1;
    g.fake.described[0xD7] = Plain(3840, 2160, kRgba8);
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.Finish();
    CHECK(g.fake.observed.size() == 1);
}

void Helpers()
{
    // the decision table
    const ViewDesc full = Plain(3840, 2160, kRgba8), quarter = Plain(1920, 1080, kRgba8);
    CHECK(DecideRemap(full, 3840, 2160, 1920, 1080, 3840, 2160) == RemapDecision::Remap);
    CHECK(DecideRemap(full, 3840, 2160, 2560, 1440, 3840, 2160) == RemapDecision::Remap);
    CHECK(DecideRemap(full, 3840, 2160, 3840, 1080, 3840, 2160) == RemapDecision::Remap); // smaller in one axis only
    CHECK(DecideRemap(full, 3840, 2160, 3840, 2160, 3840, 2160) == RemapDecision::KeepSameSize);
    CHECK(DecideRemap(quarter, 3840, 2160, 1920, 1080, 3840, 2160) == RemapDecision::KeepRenderSized);
    CHECK(DecideRemap(Plain(2000, 1100, kRgba8), 3840, 2160, 1920, 1080, 3840, 2160) == RemapDecision::KeepSizeMismatch);
    CHECK(DecideRemap(full, 3840, 2160, 4000, 2160, 3840, 2160) == RemapDecision::KeepSizeMismatch); // render larger than the output
    CHECK(DecideRemap(full, 3840, 2160, 0, 1080, 3840, 2160) == RemapDecision::KeepSizeMismatch);
    CHECK(DecideRemap(full, 1920, 1080, 960, 540, 3840, 2160) == RemapDecision::KeepSizeMismatch); // the back buffer is not the output
    ViewDesc bad = full;
    bad.plain2D = false;
    CHECK(DecideRemap(bad, 3840, 2160, 1920, 1080, 3840, 2160) == RemapDecision::KeepUnreadable);
    bad = full;
    bad.ok = false;
    CHECK(DecideRemap(bad, 3840, 2160, 1920, 1080, 3840, 2160) == RemapDecision::KeepUnreadable);
    // names
    CHECK(std::string(GBufferSemantic(GBuffer::Normals)) == "NORMALS" && std::string(GBufferVariable(GBuffer::Normals)) == "texNormals");
    CHECK(std::string(GBufferSemantic(GBuffer::Decals)) == "DECALS" && std::string(GBufferVariable(GBuffer::Decals)) == "texDecalNormals");
    CHECK(std::string(GBufferSemantic(GBuffer::DecalsWater)) == "DECALS_WATER" && std::string(GBufferVariable(GBuffer::DecalsWater)) == "texDecalWaterNormals");
    CHECK(std::string(GBufferSemantic(GBuffer::MotionVectors)) == "MOTIONVECTORS" && std::string(GBufferVariable(GBuffer::MotionVectors)) == "texNativeMotionVectors");
    CHECK(std::string(GBufferSemantic(GBuffer::Material)) == "MATERIAL" && std::string(GBufferVariable(GBuffer::Material)) == "texMaterial");
    CHECK(std::string(GBufferLabel(GBuffer::MotionVectors)) == "motion" && std::string(GBufferLabel(GBuffer::Normals)) == "normals");
    CHECK(std::string(GBufferSemantic(GBuffer::Count)) == "?");
}
} // namespace

int main()
{
    Helpers();
    Realigns();
    SrgbTwinAndRefs();
    Keeps();
    BackendRefuses();
    NothingWithoutDepth();
    NoGBufferNoCalls();
    RestoreRules();
    Unverified();
    SlotFaultsStayInTheSlot();
    Disabled();
    RebindIsObserved();
    if (g_fail != 0)
    {
        std::printf("reshade_depth_gbuffer: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("reshade_depth_gbuffer: all checks passed\n");
    return 0;
}
