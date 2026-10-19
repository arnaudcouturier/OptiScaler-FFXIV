#include "check.h"
#include "ext/ffxiv/jitter/FfxivJitterProbe.h"
#include "ext/upscaler/RenderGeometry.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;

namespace
{
constexpr uintptr_t kBase = 0x7FF600000000ull;
constexpr uint64_t kGlobal = 0x1000000000ull, kCamera = 0x2000000000ull, kObject = 0x3000000000ull;

bool Near(float a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol; }
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// A fake process: regions of bytes keyed by start address. A read succeeds only when it lies inside one region.
struct FakeMemory
{
    std::map<uint64_t, std::vector<unsigned char>> regions;
    int reads = 0;
    std::function<void(FakeMemory&)> beforeRead; // lets a test change memory between reads

    void Put(uint64_t address, const void* data, size_t size)
    {
        auto& r = regions[address];
        r.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
    }
    void PutU64(uint64_t address, uint64_t v) { Put(address, &v, sizeof(v)); }
    bool Read(uint64_t address, void* out, size_t size)
    {
        ++reads;
        if (beforeRead)
            beforeRead(*this);
        for (auto& [start, bytes] : regions)
            if (address >= start && address + size <= start + bytes.size())
            {
                std::memcpy(out, bytes.data() + (address - start), size);
                return true;
            }
        return false;
    }
    auto Reader()
    {
        return [this](uintptr_t a, void* out, size_t n) { return Read(a, out, n); };
    }
};

void PutObject(FakeMemory& m, const RawObject& o)
{
    unsigned char bytes[20];
    std::memcpy(bytes + 0, &o.jx, 4);
    std::memcpy(bytes + 4, &o.jy, 4);
    std::memcpy(bytes + 8, &o.ndcX, 4);
    std::memcpy(bytes + 12, &o.ndcY, 4);
    std::memcpy(bytes + 16, &o.counter, 4);
    m.Put(kObject, bytes, sizeof(bytes));
}

// The object of a game that applies jitter scaled by s: ndcX = s * jx / W, ndcY = -s * jy / H.
RawObject ObjectFor(float jx, float jy, float s, float w, float h, int counter = 100)
{
    return { jx, jy, s * jx / w, -s * jy / h, counter };
}

FakeMemory Image(const RawObject& o)
{
    const JitterBuild& b = kCurrentBuild.jitter;
    FakeMemory m;
    m.PutU64(kBase + b.cameraGlobalRva, kGlobal);
    m.PutU64(kGlobal + b.cameraOffset, kCamera);
    m.PutU64(kCamera + b.jitterPointerOffset, kObject);
    PutObject(m, o);
    return m;
}

Guard Read(FakeMemory& m, RawObject& out, const GameBuild* build = &kCurrentBuild)
{
    return ReadChain(m.Reader(), build, kBase, out);
}

void TestConstants()
{
    const JitterBuild& b = kCurrentBuild.jitter;
    CHECK(b.cameraGlobalRva == 0x28f8ba0 && b.cameraOffset == 0x3028 && b.jitterPointerOffset == 0x278);
    CHECK(b.jitterXOffset == 0 && b.jitterYOffset == 4 && b.ndcXOffset == 8 && b.ndcYOffset == 12 && b.counterOffset == 16 && b.objectBytes == 20);
}

void TestReadChain()
{
    const JitterBuild& b = kCurrentBuild.jitter;
    RawObject out;

    // a valid object is read field by field
    auto m = Image({ 0.25f, -0.125f, 0.0005f, 0.0003f, 777 });
    CHECK(Read(m, out) == Guard::None);
    CHECK(out.jx == 0.25f && out.jy == -0.125f && out.ndcX == 0.0005f && out.ndcY == 0.0003f && out.counter == 777);
    CHECK(m.reads == 5); // three pointers and the object twice (the second read confirms the first)

    // build mismatch: nothing is read at all
    auto none = Image({ 0.25f, 0.25f, 0, 0, 1 });
    CHECK(Read(none, out, nullptr) == Guard::BuildMismatch && none.reads == 0);

    // a null pointer at each level stops the chain there (no later read)
    {
        auto g = Image({ 0.1f, 0.1f, 0, 0, 1 });
        g.PutU64(kBase + b.cameraGlobalRva, 0);
        CHECK(Read(g, out) == Guard::GlobalInvalid && g.reads == 1);
        auto c = Image({ 0.1f, 0.1f, 0, 0, 1 });
        c.PutU64(kGlobal + b.cameraOffset, 0);
        CHECK(Read(c, out) == Guard::CameraInvalid && c.reads == 2);
        auto o = Image({ 0.1f, 0.1f, 0, 0, 1 });
        o.PutU64(kCamera + b.jitterPointerOffset, 0);
        CHECK(Read(o, out) == Guard::ObjectPointerInvalid && o.reads == 3);
    }
    // pointers that are not user-mode addresses (too low, kernel range) are invalid too
    for (uint64_t bad : { uint64_t(5), uint64_t(0xFFFF), uint64_t(0xFFFF800000000000ull), ~uint64_t(0) })
    {
        auto g = Image({ 0.1f, 0.1f, 0, 0, 1 });
        g.PutU64(kBase + b.cameraGlobalRva, bad);
        CHECK(Read(g, out) == Guard::GlobalInvalid);
        auto o = Image({ 0.1f, 0.1f, 0, 0, 1 });
        o.PutU64(kCamera + b.jitterPointerOffset, bad);
        CHECK(Read(o, out) == Guard::ObjectPointerInvalid);
    }
    // an address that is not mapped
    {
        FakeMemory empty;
        CHECK(Read(empty, out) == Guard::GlobalUnreadable);
        auto c = Image({ 0.1f, 0.1f, 0, 0, 1 });
        c.PutU64(kBase + b.cameraGlobalRva, 0x5000000000ull); // a valid-looking pointer to nothing
        CHECK(Read(c, out) == Guard::CameraUnreadable);
        auto o = Image({ 0.1f, 0.1f, 0, 0, 1 });
        o.PutU64(kGlobal + b.cameraOffset, 0x5000000000ull);
        CHECK(Read(o, out) == Guard::ObjectPointerUnreadable);
        auto p = Image({ 0.1f, 0.1f, 0, 0, 1 });
        p.PutU64(kCamera + b.jitterPointerOffset, 0x5000000000ull);
        CHECK(Read(p, out) == Guard::ObjectUnreadable);
        auto t = Image({ 0.1f, 0.1f, 0, 0, 1 });
        t.regions[kObject].resize(19); // truncated object
        CHECK(Read(t, out) == Guard::ObjectUnreadable);
    }
    // value guards
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (float bad : { 0.5001f, -0.75f, 3.0f, nan, inf, -inf })
    {
        auto x = Image({ bad, 0.1f, 0.001f, 0.001f, 1 });
        CHECK(Read(x, out) == Guard::JitterOutOfRange);
        auto y = Image({ 0.1f, bad, 0.001f, 0.001f, 1 });
        CHECK(Read(y, out) == Guard::JitterOutOfRange);
    }
    for (float bad : { 1.0001f, -2.0f, nan, inf })
    {
        auto x = Image({ 0.1f, 0.1f, bad, 0.001f, 1 });
        CHECK(Read(x, out) == Guard::NdcOutOfRange);
        auto y = Image({ 0.1f, 0.1f, 0.001f, bad, 1 });
        CHECK(Read(y, out) == Guard::NdcOutOfRange);
    }
    auto limits = Image({ 0.5f, -0.5f, 1.0f, -1.0f, 0 }); // the limits themselves are fine
    CHECK(Read(limits, out) == Guard::None);

    // Torn reads. `beforeRead` runs BEFORE read number k (counted from 0) copies its bytes: reads 0-2 are the three pointers, read 3 is
    // the first look at the object, reads 4 and 5 are the two confirmations ReadChain may make (at most 6 reads in all).
    {
        // memory that changes on every read is refused
        auto t = Image({ 0.1f, 0.1f, 0.001f, 0.001f, 1 });
        int n = 0;
        t.beforeRead = [&](FakeMemory& mem) { PutObject(mem, { 0.1f, 0.1f, 0.001f, 0.001f, 1000 + n++ }); };
        CHECK(Read(t, out) == Guard::ObjectUnstable);

        // a change before the first look is not a torn read: both looks see the new object and agree at once (5 reads, no retry)
        auto early = Image({ 0.1f, 0.1f, 0.001f, 0.001f, 1 });
        int e = 0;
        early.beforeRead = [&](FakeMemory& mem)
        {
            if (e == 3)
                PutObject(mem, { 0.2f, 0.2f, 0.001f, 0.001f, 2 });
            ++e;
        };
        CHECK(Read(early, out) == Guard::None && Near(out.jx, 0.2) && early.reads == 5);

        // one change between the first look (old) and the first confirmation (new): the first comparison differs, the retry reads the
        // new object again and the second comparison agrees -> accepted, with the NEW values, after all 6 reads
        auto s = Image({ 0.1f, 0.1f, 0.001f, 0.001f, 1 });
        int k = 0;
        s.beforeRead = [&](FakeMemory& mem)
        {
            if (k == 4)
                PutObject(mem, { 0.2f, 0.2f, 0.001f, 0.001f, 2 });
            ++k;
        };
        out = RawObject {};
        CHECK(Read(s, out) == Guard::None);
        CHECK(out.jx == 0.2f && out.jy == 0.2f && out.counter == 2);
        CHECK(s.reads == 6);

        // two different changes (before read 4 and before read 5): both comparisons differ, the retry budget is spent -> refused
        auto two = Image({ 0.1f, 0.1f, 0.001f, 0.001f, 1 });
        int j = 0;
        two.beforeRead = [&](FakeMemory& mem)
        {
            if (j == 4)
                PutObject(mem, { 0.2f, 0.2f, 0.001f, 0.001f, 2 });
            if (j == 5)
                PutObject(mem, { 0.3f, 0.3f, 0.001f, 0.001f, 3 });
            ++j;
        };
        CHECK(Read(two, out) == Guard::ObjectUnstable && two.reads == 6);
    }
}

void TestCounterTracker()
{
    CounterTracker c;
    CHECK(c.Check(10) == Guard::None); // first sample: nothing to compare with
    CHECK(c.Check(11) == Guard::None);
    CHECK(c.Check(11) == Guard::CounterNotIncreasing); // stalled
    CHECK(c.Check(12) == Guard::None);                  // re-based on the failed sample: not latched
    CHECK(c.Check(3) == Guard::CounterNotIncreasing);   // camera re-created, counter restarted
    CHECK(c.Check(4) == Guard::None);
}

void TestRenderSize()
{
    auto s = ResolveRenderSize(3840, 2160, 1920, 1080); // the game submits a display-size subrect
    CHECK(s.trusted && s.width == 1920 && s.height == 1080);
    s = ResolveRenderSize(1280, 720, 1920, 1080); // subrect smaller than the parameters: the subrect wins per axis
    CHECK(s.trusted && s.width == 1280 && s.height == 720);
    s = ResolveRenderSize(1920, 2160, 3840, 1080); // per axis
    CHECK(s.width == 1920 && s.height == 1080);
    s = ResolveRenderSize(0, 0, 1920, 1080);
    CHECK(s.trusted && s.width == 1920 && s.height == 1080);
    s = ResolveRenderSize(3840, 2160, 0, 0); // only a subrect: used for the log, never trusted
    CHECK(!s.trusted && s.width == 3840 && s.height == 2160);
    s = ResolveRenderSize(0, 0, 0, 0);
    CHECK(!s.trusted && s.width == 0 && s.height == 0);
    s = ResolveRenderSize(3840, 2160, 1920, 0); // half-known parameters are not used
    CHECK(!s.trusted);

    // The size is the one the shared rule ext::upscaler::ResolveRenderExtentValue gives (per-axis minimum of the subrect and the
    // Width/Height parameters), over a grid that has every ordering of the two numbers on each axis, equal values included. The two
    // functions intentionally differ only when the subrect is absent (0): this one then trusts Width/Height, the shared rule starts from
    // a size of 0 and changes nothing; so the grid holds non-zero values only.
    const uint32_t values[] = { 1, 640, 720, 1080, 1270, 1280, 1920, 2160, 2258, 3840 };
    uint32_t cells = 0;
    for (uint32_t subW : values)
        for (uint32_t subH : values)
            for (uint32_t paramW : values)
                for (uint32_t paramH : values)
                {
                    unsigned int w = subW, h = subH;
                    ext::upscaler::ResolveRenderExtentValue(true, paramW, paramH, w, h);
                    const RenderSize got = ResolveRenderSize(subW, subH, paramW, paramH);
                    CHECK(got.trusted && got.width == w && got.height == h);
                    ++cells;
                }
    CHECK(cells == 10000);
    // an absent subrect: the shared rule has no size to correct, this one takes the parameters (and says so)
    {
        unsigned int w = 0, h = 0;
        CHECK(!ext::upscaler::ResolveRenderExtentValue(true, 1920, 1080, w, h) && w == 0 && h == 0);
        CHECK(ResolveRenderSize(0, 0, 1920, 1080).width == 1920 && ResolveRenderSize(0, 0, 1920, 1080).trusted);
    }
}

const RenderSize kHd { 1920, 1080, true, "test" };

void TestAnalysis()
{
    // a game that applies exactly what it reports (s = 2): ratios 1, consistent, applied == reported
    auto a = Analyse(ObjectFor(0.25f, -0.125f, 2.0f, 1920, 1080), kHd);
    CHECK(a.haveApplied && Near(a.ax, 0.25) && Near(a.ay, -0.125));
    CHECK(a.haveRx && a.haveRy && Near(a.rx, 1.0) && Near(a.ry, 1.0) && Near(a.ImpliedScaleX(), 2.0) && Near(a.ImpliedScaleY(), 2.0));
    CHECK(!a.signMismatchX && !a.signMismatchY && a.verdict == Verdict::Consistent);

    // a scaled object (the suspected case): s = 1 applies half of what is reported; s = 0.5 a quarter; s = 3 one and a half
    for (float s : { 1.0f, 0.5f, 3.0f, 1.2f })
    {
        const auto b = Analyse(ObjectFor(0.3f, 0.4f, s, 1920, 1080), kHd);
        CHECK(Near(b.rx, s / 2) && Near(b.ry, s / 2) && Near(b.ImpliedScaleX(), s) && b.verdict == Verdict::Differ);
        CHECK(Near(b.ax, 0.3 * s / 2) && Near(b.ay, 0.4 * s / 2));
    }
    // the ratio tolerance on both sides of 1 (implied s within 2 +- 0.04 is consistent)
    CHECK(Analyse(ObjectFor(0.3f, 0.3f, 2.0f * 1.019f, 1920, 1080), kHd).verdict == Verdict::Consistent);
    CHECK(Analyse(ObjectFor(0.3f, 0.3f, 2.0f * 0.981f, 1920, 1080), kHd).verdict == Verdict::Consistent);
    CHECK(Analyse(ObjectFor(0.3f, 0.3f, 2.0f * 1.03f, 1920, 1080), kHd).verdict == Verdict::Differ);
    CHECK(Analyse(ObjectFor(0.3f, 0.3f, 2.0f * 0.97f, 1920, 1080), kHd).verdict == Verdict::Differ);
    // one axis off is enough
    {
        RawObject o = ObjectFor(0.3f, 0.3f, 2.0f, 1920, 1080);
        o.ndcY *= 0.5f;
        CHECK(Analyse(o, kHd).verdict == Verdict::Differ);
    }

    // a sign-flipped Y (the matrix does not negate): ry = -1, flagged
    {
        RawObject o = ObjectFor(0.3f, 0.3f, 2.0f, 1920, 1080);
        o.ndcY = -o.ndcY;
        const auto f = Analyse(o, kHd);
        CHECK(!f.signMismatchX && f.signMismatchY && Near(f.ry, -1.0) && f.verdict == Verdict::Differ);
        o = ObjectFor(0.3f, 0.3f, 2.0f, 1920, 1080);
        o.ndcX = -o.ndcX;
        CHECK(Analyse(o, kHd).signMismatchX);
    }

    // An anchor that does not use ObjectFor (which is the algebraic inverse of Analyse): the literal numbers of the FSR/XeSS convention
    // (include/fsr2/ffx_fsr2.h: ndcX = 2 * jx / width, ndcY = -2 * jy / height). jx = 0.25 at 1920 -> 2 * 0.25 / 1920 = 2.604167e-4;
    // jy = 0.25 at 1080 -> -2 * 0.25 / 1080 = -4.62963e-4. A game that follows it applies exactly what it reports: both ratios 1, and the
    // width belongs to X and the height to Y (a swap would give 0.5625 and 1.7778).
    {
        const auto fsr = Analyse({ 0.25f, 0.25f, 2.604167e-4f, -4.62963e-4f, 1 }, kHd);
        CHECK(fsr.haveRx && fsr.haveRy && Near(fsr.rx, 1.0, 1e-5) && Near(fsr.ry, 1.0, 1e-5));
        CHECK(Near(fsr.ax, 0.25, 1e-5) && Near(fsr.ay, 0.25, 1e-5));
        CHECK(!fsr.signMismatchX && !fsr.signMismatchY && fsr.verdict == Verdict::Consistent);
    }

    // division guard: a jitter of 0 (or tiny) has no ratio, and nothing is NaN or infinite
    {
        const auto z = Analyse({ 0.0f, 0.0f, 0.0f, 0.0f, 5 }, kHd);
        CHECK(!z.haveRx && !z.haveRy && z.verdict == Verdict::Undetermined && std::isfinite(z.ax) && std::isfinite(z.ay));
        const auto x0 = Analyse(ObjectFor(0.0f, 0.3f, 1.0f, 1920, 1080), kHd); // jx = 0 (Halton n = 1): Y alone decides
        CHECK(!x0.haveRx && x0.haveRy && x0.verdict == Verdict::Differ);
        const auto tiny = Analyse(ObjectFor(0.01f, 0.3f, 2.0f, 1920, 1080), kHd);
        CHECK(!tiny.haveRx && tiny.haveRy && tiny.verdict == Verdict::Consistent);
        const auto unknown = Analyse(ObjectFor(0.3f, 0.3f, 2.0f, 1920, 1080), RenderSize {});
        CHECK(!unknown.haveApplied && unknown.verdict == Verdict::Undetermined);
    }
}

void TestNgxMatch()
{
    const RawObject o { 0.25f, -0.125f, 0, 0, 1 };
    CHECK(CompareNgx(o, 0.25f, -0.125f) == NgxMatch::Same);
    CHECK(CompareNgx(o, 0.2500005f, -0.125f) == NgxMatch::Same);
    CHECK(CompareNgx(o, 0.26f, -0.125f) == NgxMatch::Different);
    CHECK(CompareNgx(o, 0.25f, 0.125f) == NgxMatch::Different); // a sign flip on the way
    CHECK(CompareNgx(o, std::nullopt, -0.125f) == NgxMatch::Unavailable);
    CHECK(CompareNgx(o, 0.25f, std::nullopt) == NgxMatch::Unavailable);
    CHECK(CompareNgx(o, std::numeric_limits<float>::quiet_NaN(), 0.0f) == NgxMatch::Unavailable);
}

void TestLimiterAndEpisodes()
{
    SampleLimiter l;
    CHECK(l.Allow(1000, 1)); // first
    CHECK(!l.Allow(1001, 1) && !l.Allow(9999, 1) && !l.Allow(10999, 1));
    CHECK(l.Allow(11000, 1));  // 10 s later
    CHECK(!l.Allow(11500, 2)); // configuration changed but only 0.5 s after the last sample
    CHECK(l.Allow(13000, 2));  // changed, 2 s since the last sample
    CHECK(!l.Allow(13500, 2) && !l.Allow(22999, 2));
    CHECK(l.Allow(23000, 2));
    CHECK(!l.Allow(22000, 9)); // the clock going backwards never opens the gate

    // a configuration that changes at every opportunity: at most kMaxChangeSamples early samples, then only the 10 s cadence
    SampleLimiter flood;
    CHECK(flood.Allow(0, 0));
    uint64_t lastTaken = 0;
    uint32_t early = 0, total = 0;
    for (uint64_t k = 1; k <= 200; ++k)
    {
        const uint64_t t = k * 2000; // 400 s, a new configuration every 2 s
        if (flood.Allow(t, k))
        {
            ++total;
            early += (t - lastTaken < SampleLimiter::kIntervalMs) ? 1 : 0;
            lastTaken = t;
        }
    }
    CHECK(early == SampleLimiter::kMaxChangeSamples);
    CHECK(total <= 40 + SampleLimiter::kMaxChangeSamples); // 400 s / 10 s plus the early ones

    EpisodeLog e;
    CHECK(!e.ShouldLog(Guard::None));
    CHECK(e.ShouldLog(Guard::CameraInvalid)); // first failure of the episode
    CHECK(!e.ShouldLog(Guard::CameraInvalid));
    CHECK(!e.ShouldLog(Guard::ObjectUnreadable)); // another guard in the same episode: still one line per episode
    CHECK(!e.ShouldLog(Guard::None));             // recovery
    CHECK(e.ShouldLog(Guard::ObjectUnreadable));  // a new episode logs again

    // a state that flickers every call cannot flood the log: at most kMaxLines lines in a session, the rest counted
    EpisodeLog flicker;
    uint32_t lines = 0;
    for (int i = 0; i < 1000; ++i)
    {
        lines += flicker.ShouldLog(Guard::ObjectNotThisEvaluate) ? 1 : 0;
        flicker.ShouldLog(Guard::None);
    }
    CHECK(lines == EpisodeLog::kMaxLines && flicker.Suppressed() == 1000 - EpisodeLog::kMaxLines);
    EpisodeLog replaceEpisodes; // the replace episodes of the glue use the same type
    lines = 0;
    for (int i = 0; i < 1000; ++i)
    {
        lines += replaceEpisodes.Event(true) ? 1 : 0;
        replaceEpisodes.Event(false);
    }
    CHECK(lines == EpisodeLog::kMaxLines);
}

void TestCorrection()
{
    auto decide = [](RawObject o, RenderSize size, NgxMatch ngx, Guard g = Guard::None) { return DecideCorrection(g, Analyse(o, size), ngx); };
    const RawObject scaled = ObjectFor(0.3f, -0.2f, 1.0f, 1920, 1080); // s = 1: half is applied
    // a matching, scaled object is replaced by the applied values
    auto d = decide(scaled, kHd, NgxMatch::Same);
    CHECK(d.replace && d.guard == Guard::None && !d.consistent && Near(d.ax, 0.15) && Near(d.ay, -0.1));
    // s = 3 replaces too (the applied jitter is larger)
    d = decide(ObjectFor(0.3f, 0.2f, 3.0f, 1920, 1080), kHd, NgxMatch::Same);
    CHECK(d.replace && Near(d.ax, 0.45) && Near(d.ay, 0.3));

    // every failed guard leaves the game's value alone
    for (Guard g : { Guard::BuildMismatch, Guard::GlobalUnreadable, Guard::GlobalInvalid, Guard::CameraUnreadable, Guard::CameraInvalid,
                     Guard::ObjectPointerUnreadable, Guard::ObjectPointerInvalid, Guard::ObjectUnreadable, Guard::ObjectUnstable,
                     Guard::JitterOutOfRange, Guard::NdcOutOfRange })
    {
        d = decide(scaled, kHd, NgxMatch::Same, g);
        CHECK(!d.replace && d.guard == g);
    }
    d = decide(scaled, RenderSize { 1920, 1080, false, "subrect" }, NgxMatch::Same);
    CHECK(!d.replace && d.guard == Guard::RenderSizeUnknown);
    d = decide(scaled, RenderSize {}, NgxMatch::Same);
    CHECK(!d.replace && d.guard == Guard::RenderSizeUnknown);
    d = decide(scaled, kHd, NgxMatch::Unavailable);
    CHECK(!d.replace && d.guard == Guard::NgxJitterUnavailable);
    d = decide(scaled, kHd, NgxMatch::Different);
    CHECK(!d.replace && d.guard == Guard::ObjectNotThisEvaluate);
    // an implausibly large applied jitter (a wrong render size, for instance)
    d = decide(ObjectFor(0.4f, 0.4f, 8.0f, 1920, 1080), kHd, NgxMatch::Same);
    CHECK(!d.replace && d.guard == Guard::AppliedImplausible);
    d = decide(scaled, RenderSize { 100000, 1080, true, "absurd" }, NgxMatch::Same);
    CHECK(!d.replace && d.guard == Guard::AppliedImplausible);

    // a consistent game (s = 2) is never touched, whatever the float noise
    for (float s : { 2.0f, 2.0f * 1.001f, 2.0f * 0.999f })
    {
        d = decide(ObjectFor(0.3f, -0.2f, s, 1920, 1080), kHd, NgxMatch::Same);
        CHECK(!d.replace && d.consistent && d.guard == Guard::None);
    }
    // ONE criterion for the probe and the correction (the ratio within 2% of 1), whatever the size of the jitter: a game the probe calls
    // consistent is never rewritten, a game it calls different is rewritten on every frame that can tell (no mix of corrected and
    // uncorrected frames chosen by the Halton value)
    for (float jitter : { 0.45f, 0.3f, 0.05f, 0.02f })
    {
        for (float factor : { 1.019f, 0.981f, 1.0f })
        {
            const RawObject o = ObjectFor(jitter, jitter, 2.0f * factor, 1920, 1080);
            d = decide(o, kHd, NgxMatch::Same);
            CHECK(Analyse(o, kHd).verdict == Verdict::Consistent && !d.replace && d.consistent);
        }
        for (float factor : { 1.03f, 0.97f, 0.95f, 0.5f })
        {
            const RawObject o = ObjectFor(jitter, jitter, 2.0f * factor, 1920, 1080);
            d = decide(o, kHd, NgxMatch::Same);
            CHECK(Analyse(o, kHd).verdict == Verdict::Differ && d.replace);
            CHECK(Near(d.ax, jitter * factor, 1e-4) && Near(d.ay, jitter * factor, 1e-4)); // both axes carry the applied values
        }
    }
    // the cases that used to disagree: s = 2 * 0.981 at a large jitter, s = 1.9 at a small one
    d = decide(ObjectFor(0.45f, 0.45f, 2.0f * 0.981f, 1920, 1080), kHd, NgxMatch::Same);
    CHECK(!d.replace && d.consistent);
    d = decide(ObjectFor(0.05f, 0.06f, 1.9f, 1920, 1080), kHd, NgxMatch::Same);
    CHECK(d.replace);
    // a frame whose jitter is too small on both axes cannot tell: values left alone, flagged so the glue changes no episode
    d = decide(ObjectFor(0.01f, -0.01f, 1.0f, 1920, 1080), kHd, NgxMatch::Same);
    CHECK(!d.replace && !d.consistent && d.undetermined && d.guard == Guard::None);
    d = decide(ObjectFor(0.0f, 0.0f, 1.0f, 1920, 1080), kHd, NgxMatch::Same);
    CHECK(!d.replace && d.undetermined);

    // sign handling: a flipped axis is not corrected (the convention is not understood), on either axis
    RawObject flipY = ObjectFor(0.3f, 0.3f, 1.0f, 1920, 1080);
    flipY.ndcY = -flipY.ndcY;
    d = decide(flipY, kHd, NgxMatch::Same);
    CHECK(!d.replace && d.guard == Guard::SignMismatch);
    RawObject flipX = ObjectFor(0.3f, 0.3f, 1.0f, 1920, 1080);
    flipX.ndcX = -flipX.ndcX;
    d = decide(flipX, kHd, NgxMatch::Same);
    CHECK(!d.replace && d.guard == Guard::SignMismatch);
    // an axis with too little jitter to tell is not a mismatch; the other axis decides
    d = decide(ObjectFor(0.0f, 0.3f, 1.0f, 1920, 1080), kHd, NgxMatch::Same);
    CHECK(d.replace && Near(d.ay, 0.15) && Near(d.ax, 0.0));

    // property: a replacement happens only when every precondition holds
    for (int readFailed = 0; readFailed < 2; ++readFailed)
        for (int ngx = 0; ngx < 3; ++ngx)
            for (int trusted = 0; trusted < 2; ++trusted)
            {
                const auto r = decide(scaled, RenderSize { 1920, 1080, trusted != 0, "t" }, static_cast<NgxMatch>(ngx),
                                      readFailed ? Guard::ObjectUnreadable : Guard::None);
                const bool allGood = readFailed == 0 && ngx == static_cast<int>(NgxMatch::Same) && trusted == 1;
                CHECK(r.replace == allGood);
            }
}

void TestText()
{
    Context ctx;
    ctx.subrectW = 3840;
    ctx.subrectH = 2160;
    ctx.widthParam = 1920;
    ctx.heightParam = 1080;
    ctx.outW = 3840;
    ctx.outH = 2160;
    ctx.perfQuality = 2;
    ctx.forcedQuality = 1;
    const RawObject o = ObjectFor(0.25f, -0.125f, 2.0f, 1920, 1080, 4242);
    const auto size = ResolveRenderSize(3840, 2160, 1920, 1080);

    const auto line = FormatSample(Analyse(o, size), NgxMatch::Same, 0.25f, -0.125f, ctx);
    CHECK(Has(line, "Game jitter: counter 4242, reported jx/jy 0.2500/-0.1250"));
    CHECK(Has(line, "render 1920x1080 (min of subrect and Width/Height)"));
    CHECK(Has(line, "ratio rx/ry 1.000/1.000, signs ok, implied s = 2.000/2.000 (consistent), NGX saw the same values"));
    CHECK(Has(line, "NGX subrect 3840x2160, Width/Height 1920x1080, OutWidth/OutHeight 3840x2160, PerfQualityValue 2"));
    CHECK(Has(line, "quality 1"));

    const auto scaledLine =
        FormatSample(Analyse(ObjectFor(0.25f, -0.125f, 1.0f, 1920, 1080), size), NgxMatch::Unavailable, std::nullopt, std::nullopt, Context {});
    CHECK(Has(scaledLine, "implied s = 1.000/1.000 (reported and applied jitter differ)"));
    CHECK(Has(scaledLine, "NGX jitter not available at this seam"));
    CHECK(Has(scaledLine, "PerfQualityValue n/a") && Has(scaledLine, "Width/Height n/a"));
    CHECK(Has(FormatSample(Analyse(o, size), NgxMatch::Different, 0.3f, 0.1f, ctx), "NGX saw different values (0.3000/0.1000"));
    CHECK(Has(FormatSample(Analyse(RawObject {}, size), NgxMatch::Same, 0.0f, 0.0f, ctx), "undetermined: jitter too small this sample"));
    CHECK(Has(FormatSample(Analyse(o, RenderSize {}), NgxMatch::Same, 0.25f, -0.125f, ctx), "undetermined: render size unknown"));
    {
        RawObject f = o;
        f.ndcY = -f.ndcY;
        CHECK(Has(FormatSample(Analyse(f, size), NgxMatch::Same, 0.25f, -0.125f, ctx), "signs MISMATCH"));
    }

    CHECK(FormatProbeFailure(Guard::CameraInvalid) == "Game jitter: not read, guard failed: camera object pointer null or not a user address");
    const auto a = Analyse(ObjectFor(0.3f, -0.2f, 1.0f, 1920, 1080), size);
    const auto d = DecideCorrection(Guard::None, a, NgxMatch::Same);
    CHECK(FormatCorrection(a, d) == "Jitter correction: reported (0.3000,-0.2000) replaced by applied (0.1500,-0.1000), ratio 0.500/0.500");
    CHECK(Has(FormatPassThrough(Guard::ObjectNotThisEvaluate), "passed through unchanged, guard failed: jitter object does not match this evaluate"));

    // the configuration key follows every field that matters: each one, changed alone, changes the key
    Context c2 = ctx;
    CHECK(ConfigurationKey(ctx) == ConfigurationKey(c2));
    const std::function<void(Context&)> changes[] = {
        [](Context& c) { c.subrectW = 1280; },
        [](Context& c) { c.subrectH = 720; },
        [](Context& c) { c.widthParam = 1280; },
        [](Context& c) { c.heightParam = 720; },
        [](Context& c) { c.outW = 1920; },
        [](Context& c) { c.outH = 1080; },
        [](Context& c) { c.perfQuality = 0; },
        [](Context& c) { c.perfQuality.reset(); },
        [](Context& c) { c.forcedQuality = 3; },
        [](Context& c) { c.forcedQuality.reset(); },
    };
    for (const auto& change : changes)
    {
        c2 = ctx;
        change(c2);
        CHECK(ConfigurationKey(ctx) != ConfigurationKey(c2));
    }
    // and the key ignores what is only informational (the sizes of the last answer): a plain log difference is no new configuration
    c2 = ctx;
    c2.answeredRenderW = 123;
    c2.answeredRenderH = 456;
    c2.answeredDisplayW = 789;
    c2.answeredDisplayH = 12;
    CHECK(ConfigurationKey(ctx) == ConfigurationKey(c2));
}
} // namespace

int main()
{
    TestConstants();
    TestReadChain();
    TestCounterTracker();
    TestRenderSize();
    TestAnalysis();
    TestNgxMatch();
    TestLimiterAndEpisodes();
    TestCorrection();
    TestText();
    return TEST_RESULT("jitter_probe");
}
