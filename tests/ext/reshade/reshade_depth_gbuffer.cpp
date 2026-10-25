// Automatic ReShade G-buffers, the binder side (ReShadeDepthCore.cpp, the slots of NORMALS, DECALS, DECALS_WATER, MOTIONVECTORS and
// MATERIAL) against the fake ReShade backend of reshade_depth_fake.h: the decision per buffer (remap or keep the add-on's binding),
// the request the backend gets, the bind and the restore at finish (only if the entry is still ours), the unverified undo, a restore
// that fails and is retried, reference balance, the rule that nothing is done without a bound depth, and zero extra backend calls
// when the effects declare no G-buffer.
#include "pch.h"

#include "reshade_depth_fake.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

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
    CHECK(g.fake.remapMakes == 2 && g.fake.draws == 2);
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
    CHECK(g.fake.remapMakes == 2 && g.fake.draws == 4); // the targets are the same (kept across frames), only drawn again
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
        // the status line must not claim a realignment that another add-on undid: normals cleared, motion (still ours) kept
        CHECK((g.binder.RealignedMask() & (1u << static_cast<uint32_t>(GBuffer::Normals))) == 0 && (g.binder.RealignedMask() & (1u << static_cast<uint32_t>(GBuffer::MotionVectors))) != 0);
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
    // the source IS the back buffer but the back buffer is not the output size: the clause "back buffer == output" decides
    CHECK(DecideRemap(Plain(1920, 1080, kRgba8), 1920, 1080, 960, 540, 3840, 2160) == RemapDecision::KeepSizeMismatch);
    CHECK(DecideRemap(Plain(1920, 1080, kRgba8), 1920, 1080, 960, 540, 1920, 1080) == RemapDecision::Remap); // ... and it remaps when it is
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

// ---- the fix round: ambiguity with empty readings, the subrect base, the motion scale, DLAA, and the gate-lane cases -------------------
std::vector<std::tuple<GBuffer, Counter, uint64_t>> g_slotLogged;
void SlotHook(GBuffer buffer, Counter counter, uint64_t count) { g_slotLogged.emplace_back(buffer, counter, count); }

// replaces the texNormals variables of the rig by `vars`, placed right after the depth variable
void SetNormalsVars(GRig& g, const std::vector<Fake::Var>& vars)
{
    auto& v = g.fake.vars;
    v.erase(std::remove_if(v.begin(), v.end(), [](const Fake::Var& x) { return x.name == "texNormals"; }), v.end());
    v.insert(v.begin() + 1, vars.begin(), vars.end());
}

// A same-named texture that is NOT a reader (an effect's own render target) next to real readers: the readers show nothing while nobody
// has bound the semantic, the decoy shows its own view. Comparing only the non-empty readings used to take the decoy's view as the source.
void AmbiguityCountsEmptyReadings()
{
    for (const bool decoyFirst : { false, true })
    {
        GRig g;
        g.fake.sem.erase("NORMALS");
        const Fake::Var decoy { kVarN2, "texNormals", "", 0xE1, 0xE2 }, reader { kVarN, "texNormals", "NORMALS", 0, 0 };
        g.fake.described[0xE1] = Plain(3840, 2160, kRgba8);
        for (uint64_t v : { 0xE1ull, 0xE2ull })
            g.fake.owner[v] = 1;
        SetNormalsVars(g, decoyFirst ? std::vector<Fake::Var> { decoy, reader } : std::vector<Fake::Var> { reader, decoy });
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        CHECK(!g.fake.notes.empty() && g.fake.notes[0].buffer == GBuffer::Normals && g.fake.notes[0].decision == RemapDecision::KeepAmbiguous);
        CHECK(g.fake.remaps.size() == 1 && g.fake.remaps[0].buffer == GBuffer::MotionVectors); // only the motion vectors were copied
        CHECK(g.fake.sem.count("NORMALS") == 0);                                             // nothing was bound to the semantic
        for (const auto& u : g.fake.updates)
            CHECK(u.semantic != "NORMALS");
        CHECK(g.binder.GetCounters(GBuffer::Normals).kept == 1 && g.binder.GetCounters(GBuffer::Normals).passesBound == 0);
        g.Finish();
        CHECK(g.fake.Balanced());
    }
    {   // several readers that all show the add-on's binding agree: copied
        GRig g;
        SetNormalsVars(g, { { kVarN, "texNormals", "NORMALS", 0, 0 }, { kVarN2, "texNormals", "NORMALS", 0, 0 } });
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        CHECK(g.binder.GetCounters(GBuffer::Normals).passesBound == 1 && g.fake.notes[0].decision == RemapDecision::Remap);
        g.Finish();
        CHECK(g.Both(kRestN, kRestMv) && g.fake.Balanced());
    }
    {   // several readers, nothing bound yet: nothing to copy
        GRig g;
        g.fake.sem.erase("NORMALS");
        SetNormalsVars(g, { { kVarN, "texNormals", "NORMALS", 0, 0 }, { kVarN2, "texNormals", "NORMALS", 0, 0 } });
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        CHECK(g.fake.notes[0].decision == RemapDecision::KeepNoSource && g.fake.remaps.size() == 1);
        g.Finish();
        CHECK(g.fake.Balanced());
    }
}

// The remap assumes the render window starts at 0,0; the depth honours Depth.Subrect.Base. A base that is not 0,0 keeps the add-on's bindings.
void SubrectBaseAndMotionScale()
{
    for (const auto& base : { std::pair<uint32_t, uint32_t> { 3, 0 }, { 0, 5 }, { 7, 9 } })
    {
        GRig g;
        DepthCapture c = g.r.Capture();
        c.baseX = base.first;
        c.baseY = base.second;
        g.binder.PublishCapture(c);
        CHECK(g.Begin() == Outcome::Bound); // the depth is bound all the same: it honours the base
        CHECK(g.fake.remaps.empty() && g.fake.notes.size() == 2);
        CHECK(g.fake.notes[0].decision == RemapDecision::KeepSubrectBase && g.fake.notes[0].baseX == base.first && g.fake.notes[0].baseY == base.second);
        CHECK(g.fake.Calls("DescribeView") == 0 && g.fake.bindingReads.count(kVarN) == 0); // decided from the capture alone
        CHECK(g.Both(kRestN, kRestMv));
        CHECK(g.binder.GetCounters(GBuffer::Normals).kept == 1 && g.binder.GetCounters(GBuffer::MotionVectors).kept == 1 && g.binder.RealignedMask() == 0);
        g.Finish();
        CHECK(g.fake.Balanced());
    }
    {   // the reported motion vector scale travels with the request (0 = not reported)
        GRig g;
        DepthCapture c = g.r.Capture();
        c.mvScaleX = 3840.0f;
        c.mvScaleY = 2160.0f;
        g.binder.PublishCapture(c);
        CHECK(g.Begin() == Outcome::Bound && g.fake.remaps.size() == 2);
        CHECK(g.fake.remaps[1].buffer == GBuffer::MotionVectors && g.fake.remaps[1].motionScaleX == 3840.0f && g.fake.remaps[1].motionScaleY == 2160.0f);
        g.Finish();
        GRig h;
        CHECK(h.Begin() == Outcome::Bound && h.fake.remaps.size() == 2 && h.fake.remaps[1].motionScaleX == 0.0f && h.fake.remaps[1].motionScaleY == 0.0f);
        h.Finish();
    }
}

// DLAA (render == output): nothing to realign, so the add-on's bindings are not even read, one log line per change, no rate line for `kept`.
void DlaaEarlyOut()
{
    GRig g;
    g.binder.SetSlotLog(&SlotHook);
    g_slotLogged.clear();
    for (int i = 0; i < 6; ++i)
    {
        g.r.NextFrame();
        DepthCapture c = g.r.Capture();
        c.renderW = 3840;
        c.renderH = 2160;
        g.binder.PublishCapture(c);
        CHECK(g.Begin() == Outcome::Bound);
        g.Finish();
    }
    CHECK(g.fake.remaps.empty() && g.fake.Calls("DescribeView") == 0);
    CHECK(g.fake.bindingReads.count(kVarN) == 0 && g.fake.bindingReads.count(kVarMv) == 0);
    CHECK(g.fake.notes.size() == 2 && g.fake.notes[0].decision == RemapDecision::KeepSameSize && g.fake.notes[1].decision == RemapDecision::KeepSameSize);
    CHECK(g.binder.GetCounters(GBuffer::Normals).kept == 6 && g.binder.GetCounters(GBuffer::MotionVectors).kept == 6);
    CHECK(g.binder.GetCounters(GBuffer::Normals).failed == 0 && g.binder.RealignedMask() == 0);
    for (const auto& e : g_slotLogged)
        CHECK(std::get<1>(e) != Counter::Kept); // counted, never logged as a rate line
    // the render scale changes: the copy is made and told again; the other counters still log
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.fake.remaps.size() == 2 && g.fake.notes.size() == 4 && g.fake.notes[2].decision == RemapDecision::Remap);
    bool bound = false;
    for (const auto& e : g_slotLogged)
        bound = bound || std::get<1>(e) == Counter::PassesBound;
    CHECK(bound);
    g.Finish();
    CHECK(g.fake.Balanced());
}

// A slot whose restore keeps failing stays dirty (and referenced) while the depth and the other slot are clean.
void DirtySlotCleanDepth()
{
    GRig g;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.fake.failRestore = true;
    g.fake.failSemantic = "NORMALS";
    g.Finish();
    CHECK(g.fake.DepthIs(kOrig, kOrigSrgb) && g.fake.sem.at("MOTIONVECTORS").first == kRestMv);
    const uint64_t stuck = g.fake.sem.at("NORMALS").first;
    CHECK(stuck != kRestN && g.fake.copyViews.count(stuck) && g.fake.CoreRefs(stuck) == 1); // still ours, still referenced
    CHECK(g.binder.GetCounters(GBuffer::Normals).failed == 1 && g.binder.GetCounters().failed == 0 && g.binder.GetCounters(GBuffer::MotionVectors).failed == 0);
    // the next begin retries first; it fails again; the depth and the motion vectors bind as usual, normals are skipped
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.binder.GetCounters(GBuffer::Normals).failed >= 2 && g.binder.GetCounters(GBuffer::Normals).passesBound == 1);
    CHECK(g.binder.GetCounters(GBuffer::MotionVectors).passesBound == 2 && (g.binder.RealignedMask() & 1u) == 0);
    CHECK(g.fake.sem.at("NORMALS").first == stuck && g.fake.CoreRefs(stuck) == 1);
    g.Finish();
    // restorable again: the retry puts the add-on's binding back, then the pass binds again
    g.fake.failRestore = false;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound && g.binder.GetCounters(GBuffer::Normals).passesBound == 2);
    g.Finish();
    CHECK(g.Both(kRestN, kRestMv) && !g.fake.HoldsCopy() && g.fake.Balanced() && g.fake.violations == 0);
}

// Shutdown with passes open (depth and slots): every reference goes back, no ReShade call is made, and nothing binds afterwards.
void ShutdownWithOpenSlots()
{
    GRig g;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    const size_t updates = g.fake.updates.size();
    CHECK(g.fake.CopyRefs() > 0);
    g.binder.Shutdown();
    CHECK(g.fake.updates.size() == updates && g.fake.CopyRefs() == 0 && g.fake.Balanced());
    CHECK(g.Begin() == Outcome::Disabled);
}

// A runtime destroyed while it holds copies is kept (orphaned) without a ReShade call. If the same pointer comes back with its map (a reset),
// the next begin restores first; if it comes back with an empty map (a new object), no variable shows the copy: the entries are erased
// (counted) and the add-on's fresh binding of that frame is lost, the next frame is fine.
void DestroyOrphanReturns()
{
    {
        GRig g;
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        const size_t updates = g.fake.updates.size();
        g.binder.OnDestroyRuntime(kRt);
        CHECK(g.fake.updates.size() == updates && g.fake.CopyRefs() > 0);
        g.fake.RuntimeDestroyed(Fake::DestroyWorld::Reset); // the same object, its map survives
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound); // restored first, then bound again
        g.Finish();
        CHECK(g.Both(kRestN, kRestMv) && g.fake.DepthIs(kOrig, kOrigSrgb) && !g.fake.HoldsCopy() && g.fake.Balanced());
        CHECK(g.binder.GetCounters(GBuffer::Normals).erasedUnverified == 0 && g.binder.GetCounters().erasedUnverified == 0);
    }
    {
        GRig g;
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        g.binder.OnDestroyRuntime(kRt);
        g.fake.RuntimeDestroyed(Fake::DestroyWorld::MapDies); // a new object: empty maps, then the add-on binds again
        g.fake.sem["NORMALS"] = { kRestN, kRestNs };
        g.fake.sem["MOTIONVECTORS"] = { kRestMv, kRestMvs };
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        CHECK(g.binder.GetCounters(GBuffer::Normals).erasedUnverified == 1 && g.binder.GetCounters(GBuffer::MotionVectors).erasedUnverified == 1);
        CHECK(g.binder.GetCounters().erasedUnverified == 1);
        CHECK(g.fake.notes.back().decision == RemapDecision::KeepNoSource); // this frame's binding was erased: nothing to copy
        g.Finish();
        g.fake.sem["NORMALS"] = { kRestN, kRestNs }; // the add-on binds again at its next begin
        g.fake.sem["MOTIONVECTORS"] = { kRestMv, kRestMvs };
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound && g.binder.GetCounters(GBuffer::Normals).passesBound == 2);
        g.Finish();
        CHECK(g.Both(kRestN, kRestMv) && g.fake.Balanced());
    }
}

// The option goes off between begin and finish: finish still puts everything back; the targets are dropped by the next begin.
void SwitchedOffMidPass()
{
    GRig g;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.binder.SetEnabled(false);
    g.Finish();
    CHECK(g.Both(kRestN, kRestMv) && g.fake.DepthIs(kOrig, kOrigSrgb) && !g.fake.HoldsCopy() && g.fake.Balanced());
    CHECK(g.fake.releaseRemaps == 0);
    CHECK(g.Begin() == Outcome::Disabled && g.fake.releaseRemaps == 1);
}

// Two runtimes with the same buffers, passes overlapping: each gets its own copy bound and its own binding back.
void TwoRuntimes()
{
    Fake a, b;
    Router router(a, b);
    DepthBinder binder(router, nullptr);
    binder.SetEnabled(true);
    constexpr uint64_t kRestN2 = 0xD5, kRestN2s = 0xD6;
    for (Fake* f : { &a, &b })
    {
        f->vars.push_back({ kVarN, "texNormals", "NORMALS", 0, 0 });
        f->sem["NORMALS"] = f == &a ? std::make_pair(kRestN, kRestNs) : std::make_pair(kRestN2, kRestN2s);
    }
    for (uint64_t v : { kRestN, kRestNs, kRestN2, kRestN2s })
        a.owner[v] = 1;
    a.described[kRestN] = Plain(3840, 2160, kRgba8);
    a.described[kRestN2] = Plain(3840, 2160, kRgba8);
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
    CHECK(binder.OnBeginEffects(kRt, kCtx) == Outcome::Bound);
    CHECK(binder.OnBeginEffects(kRt2, kCtx) == Outcome::Bound);
    CHECK(a.sem.at("NORMALS").first != kRestN && b.sem.at("NORMALS").first != kRestN2);
    CHECK(binder.GetCounters(GBuffer::Normals).passesBound == 2);
    binder.OnFinishEffects(kRt);
    CHECK(a.sem.at("NORMALS").first == kRestN && b.sem.at("NORMALS").first != kRestN2); // the other runtime is still in its pass
    binder.OnFinishEffects(kRt2);
    CHECK(b.sem.at("NORMALS").first == kRestN2 && b.sem.at("NORMALS").second == kRestN2s);
    CHECK(a.Balanced() && b.Balanced() && !a.HoldsCopy() && !b.HoldsCopy());
}

// The engine keeps its target (and so its views) across frames: a stale bind of our own copy is no original and is refused.
void StaleBindOfOurCopy()
{
    GRig g;
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    g.Finish();
    const uint64_t copy = g.fake.held.at(GBuffer::Normals).a;
    g.fake.sem["NORMALS"] = { copy, copy };
    g.fake.described[copy] = Plain(3840, 2160, kRgba8);
    g.r.NextFrame();
    CHECK(g.Begin() == Outcome::Bound);
    CHECK(g.binder.GetCounters(GBuffer::Normals).failed == 1 && g.binder.GetCounters(GBuffer::Normals).passesBound == 1); // refused this time
    CHECK(g.fake.sem.at("NORMALS").first == copy);                                                                          // untouched
    CHECK(g.binder.GetCounters(GBuffer::MotionVectors).passesBound == 2);
    g.Finish();
    CHECK(g.fake.Balanced());
}

// The add-on's view stays the one we put back for kStableLogAfter begins: told once per buffer.
void ViewStableIsObserved()
{
    GRig g;
    for (uint32_t i = 0; i < kStableLogAfter + 4; ++i)
    {
        g.r.NextFrame();
        CHECK(g.Begin() == Outcome::Bound);
        g.Finish();
    }
    size_t stable = 0, changed = 0;
    for (const auto& e : g.fake.observed)
    {
        stable += e.second == GBufferEvent::ViewStable ? 1 : 0;
        changed += e.second == GBufferEvent::ViewChanged ? 1 : 0;
    }
    CHECK(stable == 2 && changed == 0); // normals and motion, once each
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
    AmbiguityCountsEmptyReadings();
    SubrectBaseAndMotionScale();
    DlaaEarlyOut();
    DirtySlotCleanDepth();
    ShutdownWithOpenSlots();
    DestroyOrphanReturns();
    SwitchedOffMidPass();
    TwoRuntimes();
    StaleBindOfOurCopy();
    ViewStableIsObserved();
    if (g_fail != 0)
    {
        std::printf("reshade_depth_gbuffer: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("reshade_depth_gbuffer: all checks passed\n");
    return 0;
}
