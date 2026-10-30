// The strict shapes of the jitter-source group, on committed bytes only:
//   1. the hook-site registry: kJitterHookSites (AnchorsJitter.h) is DATA that the shape check iterates; here it is checked structurally against the registry
//      and against the literal facts of resolve_jitter_shape_data.h: every shape seam has ONE guard anchor hanging from the Rva-bound location
//      seam, the values are equal, failClosed is set, the location is bound to an Rva field and the shape to the Guard field, no Guard is bound
//      to a location seam, and the seams with no GameBuild field are exactly the gate list;
//   2. the mask map: the wildcard positions of every shape are pinned as literals (a RIP displacement, a call target, the TLS slot), independent
//      of the pin text that `resolve_jitter_anchors --dump` regenerates;
//   3. EVERY non-wildcard byte of every shape, flipped one at a time on an UNKNOWN build (the resolver fills the Guard fields with live bytes
//      there, so the shape seam is the only check): the shape seam is not usable, its Guard field stays zero and the real VerifyAnchors refuses
//      through the block of that shape. The flips come from the literal facts, not from the registry, so a registry that masks a byte the facts say
//      is fixed (a consumer byte hidden behind a wildcard) is caught behaviourally as well as by the mask map.
#include "resolve_jitter_shape_data.h"

#include "ext/ffxiv/resolve/Entry.h"

#include <cstring>
#include <set>

using namespace jt;

// The mask-map comparison is a plain CHECK. (The mutation check of the fix compiles a copy of this file in which it is a no-op, to show that the
// flips of section 3 alone kill a mask mutant.)
#define MASKMAP_CHECK(c) CHECK(c)

namespace
{
using ext::ffxiv::resolve::kJitterGateSeams;
using ext::ffxiv::resolve::kJitterGlobals;
using ext::ffxiv::resolve::kJitterHookSites;

const ext::ffxiv::resolve::JitterHookSite* SiteOfShape(const std::string& shape)
{
    for (const auto& s : kJitterHookSites)
    {
        if (shape == s.shape)
            return &s;
    }
    return nullptr;
}

const Binding* BindingOfKey(const Registry& reg, const std::string& key)
{
    const Binding* found = nullptr;
    std::size_t n = 0;
    for (const Binding& b : reg.bindings)
    {
        if (b.key == key)
        {
            found = &b;
            ++n;
        }
    }
    return n == 1 ? found : nullptr; // one binding per key
}

// ---- 1. the hook-site registry -------------------------------------------------------------------------------------------------

void TestHookSiteRegistry()
{
    const Registry reg = JitterRegistry();
    const auto img = MakeJitterImage(0x11111111);
    REQUIRE(img != nullptr);
    const ResolvedBuild pure = ResolveImage(*img, reg);

    const std::size_t sites = sizeof(kJitterHookSites) / sizeof(kJitterHookSites[0]);
    CHECK(sites == 7 && sites == Shapes().size());

    // the table agrees with the literal facts, entry by entry and in VerifyAnchors' order
    const char* order[] = { "generator", "cameraGetter", "producerCall", "cameraHead", "phases", "strength", "deviceSize" };
    for (std::size_t i = 0; i < sites; ++i)
        CHECK(std::strcmp(kJitterHookSites[i].block, order[i]) == 0);
    for (const ShapeFacts& f : Shapes())
    {
        const auto* site = SiteOfShape(f.shape);
        REQUIRE(site != nullptr);
        CHECK(std::strcmp(site->location, f.location) == 0 && std::strcmp(site->guardKey, f.guardKey) == 0 && std::strcmp(site->block, f.block) == 0);
    }

    std::set<std::string> shapes, locations;
    for (const auto& site : kJitterHookSites)
    {
        const SeamSpec* loc = FindSeam(reg, site.location);
        const SeamSpec* shp = FindSeam(reg, site.shape);
        REQUIRE(loc != nullptr && shp != nullptr);
        CHECK(shapes.insert(site.shape).second && locations.insert(site.location).second); // no site listed twice

        // the Rva field is bound to the LOCATION seam, the Guard field to the SHAPE seam
        const FieldRow* rvaRow = FindField(site.rvaKey);
        const FieldRow* guardRow = FindField(site.guardKey);
        REQUIRE(rvaRow != nullptr && guardRow != nullptr);
        CHECK(rvaRow->kind == FieldKind::Rva && guardRow->kind == FieldKind::Guard);
        const Binding* rb = BindingOfKey(reg, site.rvaKey);
        const Binding* gb = BindingOfKey(reg, site.guardKey);
        REQUIRE(rb != nullptr && gb != nullptr);
        CHECK(rb->seam == site.location && gb->seam == site.shape && gb->guardOffset == 0);

        // the shape never table-fills, on any build
        if (!shp->failClosed)
            std::printf("FAIL: %s is not failClosed\n", site.shape);
        CHECK(shp->failClosed);

        if (std::string(site.location) == site.shape)
        {
            // deviceSize: location and shape are one seam; its only anchor is scanned (the strict 20 bytes, two template copies)
            REQUIRE(shp->anchors.size() == 1);
            CHECK(shp->anchors[0].fromSeam.empty() && shp->anchors[0].expectedHits == 2);
            continue;
        }
        CHECK(loc->kind == SeamKind::Function || loc->kind == SeamKind::CodeSite);
        CHECK(!loc->failClosed); // a location seam keeps the table fallback / EntryPatched rules
        // exactly ONE anchor, a guard at the location seam (a second anchor would let one failing piece read FoundSingleAnchor = usable)
        REQUIRE(shp->kind == SeamKind::CodeSite && shp->anchors.size() == 1);
        const AnchorSpec& a = shp->anchors[0];
        CHECK(a.fromSeam == site.location);
        CHECK(a.valueFrom == ValueFrom::Match && a.fromOffset + a.valueOffset == 0); // shape value = location value
        // ... and on the committed layout the two values are equal
        const SeamResult& ls = Get(pure, site.location);
        const SeamResult& ss = Get(pure, site.shape);
        CHECK(ls.status == SeamStatus::Found && ss.status == SeamStatus::Found && ls.rva == ss.rva);
    }

    // every failClosed seam of the group is a listed shape, and every Guard binding of the group lands on one (never on a location seam)
    for (const SeamSpec& s : reg.seams)
        CHECK(!s.failClosed || shapes.count(s.id) == 1);
    for (const Binding& b : reg.bindings)
    {
        const FieldRow* row = FindField(b.key);
        REQUIRE(row != nullptr);
        if (row->kind == FieldKind::Guard)
        {
            if (shapes.count(b.seam) != 1)
                std::printf("FAIL: the Guard field %s is bound to %s, which is not a hook-site shape\n", b.key.c_str(), b.seam.c_str());
            CHECK(shapes.count(b.seam) == 1);
        }
    }

    // the three globals the hooks read memory through: bound to their Rva field (P3 refuses a zero one, see the header), Global seams
    std::set<std::string> globals;
    for (const auto& g : kJitterGlobals)
    {
        const SeamSpec* s = FindSeam(reg, g.seam);
        REQUIRE(s != nullptr && s->kind == SeamKind::Global);
        const FieldRow* row = FindField(g.rvaKey);
        REQUIRE(row != nullptr);
        CHECK(row->kind == FieldKind::Rva);
        const Binding* b = BindingOfKey(reg, g.rvaKey);
        REQUIRE(b != nullptr);
        CHECK(b->seam == g.seam && b->valueOffset == 0);
        globals.insert(g.seam);
    }
    CHECK(globals.size() == 3 && sizeof(kJitterGlobals) / sizeof(kJitterGlobals[0]) == 3);

    // the seams with NO GameBuild field are exactly the gate list (the gate checks them until fields exist)
    std::set<std::string> unbound, gate;
    for (const SeamSpec& s : reg.seams)
    {
        const bool bound = std::any_of(reg.bindings.begin(), reg.bindings.end(), [&](const Binding& b) { return b.seam == s.id; });
        if (!bound)
            unbound.insert(s.id);
    }
    for (const char* id : kJitterGateSeams)
        gate.insert(id);
    CHECK(unbound == gate && gate.size() == 3);
    CHECK(gate.count("jitterSource.strengthFloor") == 1 && gate.count("jitterSource.ndcSignMask") == 1 && gate.count("jitterSource.cameraGlobal") == 1);
}

// ---- 2. the literal mask map -------------------------------------------------------------------------------------------------

void TestMaskMap()
{
    const Registry reg = JitterRegistry();
    std::size_t runs = 0;
    for (const ShapeFacts& f : Shapes())
    {
        const SeamSpec* s = FindSeam(reg, f.shape);
        REQUIRE(s != nullptr && s->anchors.size() == 1);
        Pattern p;
        REQUIRE(ParsePattern(s->anchors[0].pattern, p));
        MASKMAP_CHECK(p.size() == f.size);
        for (std::size_t i = 0; i < p.size() && i < f.size; ++i)
        {
            // a byte is fully fixed or fully wild (no nibble wildcards), and wild exactly where the facts say
            const bool wild = p.mask[i] == 0x00;
            if (p.mask[i] != 0x00 && p.mask[i] != 0xff)
                std::printf("FAIL: %s +0x%zx is a nibble wildcard\n", f.shape, i);
            CHECK(p.mask[i] == 0x00 || p.mask[i] == 0xff);
            if (wild != IsMaskedAt(f, i))
                std::printf("FAIL: %s +0x%zx is %s in the registry, the facts say %s\n", f.shape, i, wild ? "a wildcard" : "fixed", wild ? "fixed" : "a wildcard");
            MASKMAP_CHECK(wild == IsMaskedAt(f, i));
        }
        MASKMAP_CHECK(p.fixedBytes() == FixedBytes(f));
        runs += f.masks.size();
        // every run lies inside the pattern
        for (const MaskRun& m : f.masks)
            CHECK(m.at + m.len <= f.size);
    }
    CHECK(runs == 5 + 0 + 3 + 5 + 9 + 5 + 1);
}

// ---- 3. every non-wildcard byte, on an unknown build ---------------------------------------------------------------------------

// Bytes whose flip also breaks the LOCATION seam, because both location anchors contain them (shared by two anchors), so that the shape is
// unreachable and cannot be exercised on its own: the byte is still refused (through the dead location). Everything else keeps the location
// usable and is refused by the shape alone.
struct SharedByte
{
    const char* shape;
    std::uint32_t at, len; // [at, at + len) inside the shape pattern
    const char* why;
};
const SharedByte kSharedWithLocation[] = {
    { "jitterSource.producerCallShape", 0x54, 1, "the E8 of the call: producerCall and producerPre both contain it" },
    { "jitterSource.strengthShape", 0x00, 1, "the E8 of the generator call: strengthCall and strengthArgs both contain it" },
    { "jitterSource.strengthShape", 0x05, 3, "48 8B 0D after the E8: strengthCall and strengthArgs both contain them" },
    { "jitterSource.strengthShape", 0x0c, 4, "4C 8D 45 D0 after the renderer load: strengthCall and strengthArgs both contain them" },
};

bool IsSharedWithLocation(const ShapeFacts& f, std::size_t at)
{
    for (const SharedByte& b : kSharedWithLocation)
    {
        if (std::strcmp(b.shape, f.shape) == 0 && at >= b.at && at < b.at + b.len)
            return true;
    }
    return false;
}

void TestEveryFixedByte()
{
    const Registry reg = JitterRegistry();
    auto img = MakeJitterImage(0x11111111);
    REQUIRE(img != nullptr);

    // unmodified: every shape found, every Guard filled with the live bytes, the hooks' compare passes
    {
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(!rb.knownBuild && rb.Count(SeamStatus::Found) == reg.seams.size());
        for (const ShapeFacts& f : Shapes())
            CHECK(rb.Usable(f.shape) && !GuardIsZero(rb, f.guardKey));
        CHECK(ext::ffxiv::jitter::source::VerifyAnchors(ReaderOf(*img), rb.build.jitterSource, kBase).ok);
    }

    std::size_t flips = 0, viaShapeAlone = 0, viaDeadLocation = 0;
    for (const ShapeFacts& f : Shapes())
    {
        std::size_t shapeFlips = 0;
        for (std::size_t at = 0; at < f.size; ++at)
        {
            if (IsMaskedAt(f, at))
                continue;
            const std::uint32_t rva = f.site + static_cast<std::uint32_t>(at);
            const FlipRun run = FlipAndResolve(*img, reg, rva, false);
            const bool shared = IsSharedWithLocation(f, at);
            const bool refused = !run.verifyOk && std::strcmp(run.failedBlock, f.block) == 0;
            const bool shapeDead = !run.rb.Usable(f.shape) && GuardIsZero(run.rb, f.guardKey);
            const bool sameSeam = std::strcmp(f.location, f.shape) == 0; // deviceSize: location and shape are one seam
            const bool locationAlive = sameSeam ? false : run.rb.Usable(f.location);
            // not shared: the location survives (one anchor left, or none of its anchors contains the byte) and the SHAPE refuses on its own
            // shared: the location is dead too (both of its anchors contain the byte)
            const bool ok = refused && shapeDead && (sameSeam || (shared ? !locationAlive : locationAlive));
            if (!ok)
                std::printf("FAIL: flipping %s +0x%zx (rva 0x%x): refused=%d block=%s shapeDead=%d location=%s expected %s\n", f.shape, at, rva, refused ? 1 : 0,
                            run.failedBlock, shapeDead ? 1 : 0, locationAlive ? "usable" : "dead", shared ? "dead" : "usable");
            CHECK(ok);
            ++shapeFlips;
            (shared ? viaDeadLocation : viaShapeAlone)++;
        }
        CHECK(shapeFlips == FixedBytes(f));
        flips += shapeFlips;
    }
    std::printf("    unknown build: %zu fixed bytes flipped: %zu refused by the shape alone (location alive), %zu through a dead location too\n", flips, viaShapeAlone,
                viaDeadLocation);
    CHECK(flips == 286 + 8 + 69 + 100 + 168 + 91 + 16);
    CHECK(viaDeadLocation == 1 + 1 + 3 + 4); // exactly the allow-list above (fixed bytes only), no more
}
} // namespace

int main()
{
    TestHookSiteRegistry();
    TestMaskMap();
    TestEveryFixedByte();
    return TEST_RESULT("resolve_jitter_shapes");
}
