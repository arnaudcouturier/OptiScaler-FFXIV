// P2a (MEDIUM-2 of the independent review): what the quality anchors pin and what they mask, derived from the registry itself and checked against
// the committed bytes of build C (no executable needed).
//
// Why this test exists. resolve_quality_pins compares every pattern with an independent copy of its text. A future editor who loosens a pattern
// updates both, and the hand-written flips of resolve_quality_controls only prove the bytes somebody thought of. A fixed-byte sweep cannot find an
// over-masked byte either: the byte is no longer fixed, so nothing visits it. Three generated checks close that:
//   1. OVER-MASKING. Every byte that ANY anchor masks and NO anchor decodes must be in a short, explicit allow-list (kMaskedOnPurpose) with the
//      reason it is free: a RIP displacement or call target that only forwards control, an import call, a constant pool load. The set is computed
//      per anchor at the anchor's own site, pinned-by-another-anchor bytes are NOT excluded (an anchor that masks what a sibling pins would weaken
//      the signature the moment the sibling is lost). Set EQUALITY: an unexplained masked byte fails, and so does an allow-list entry nobody masks.
//   2. EVERY FIXED BYTE IS ENFORCED. Each fixed byte of each anchor is flipped in the planted image (one image, flipped and restored): every anchor
//      that fixes it stops being Unique, its seam is not plain Found unless two other anchors still agree, a seam whose every anchor fixes it is
//      NotFound; for a SHAPE seam the Guard field is ZERO afterwards (HIGH-1, for every byte of the three strict patterns).
//   3. THE PATCH REGION. The first 14 bytes of the entry of every hooked function (the two Detour targets and the direct-call target) are fixed in
//      the entry pattern and in the shape pattern, with no wildcard: the length of the patch the vendored Detours writes (5 bytes, or a 14-byte
//      absolute jump) was not verified, so any byte it may relocate is treated as pinned.
#include "resolve_quality_util.h"

#include <map>
#include <set>

using namespace qt;

namespace
{
struct Span
{
    std::uint32_t rva;
    std::uint32_t length;
    const char* why;
};

// Every byte of build C that some anchor masks and no anchor decodes, as RVA ranges. Each entry says why nobody needs the byte.
// (15 ranges, 54 bytes: every one is a RIP displacement or a branch/call distance. No register, no immediate, no struct offset, no opcode is here.)
const Span kMaskedOnPurpose[] = {
    // resizeCallbacks entry: the two indirect calls `call [rip+..]` (the lock and the unlock of the callbacks array; imports, nobody reads where)
    { 0x2369c0, 4, "FF 15 disp32: indirect call through the import table (the lock)" },
    { 0x236a0c, 4, "FF 15 disp32: indirect call through the import table (the unlock)" },
    // rendererUpdate caller: `mov rcx,[renderer] ; movaps ; call rendererUpdate ; mov rcx,[..] ; test rcx,rcx ; je +5 ; call ..`
    { 0x2b9e6c, 4, "mov rcx,[rip+..]: the renderer global (the hook receives the renderer as its argument; on build C it equals chara.managerPointer, pinned)" },
    { 0x2b9e7c, 4, "mov rcx,[rip+..] after the call: another global of the caller, unrelated to the hooked call" },
    { 0x2b9e86, 4, "E8 rel32: the second call of the caller (after test rcx,rcx), unrelated to the hooked call" },
    // rendererUpdate prologue / body
    { 0x2da5be, 4, "mov rax,[rip+..]: the stack cookie (__security_cookie), the consumer never reads it" },
    { 0x2da5d9, 4, "mulss xmm0,[rip+..]: a float constant of the update, not read" },
    // manager constructor `mov [rdi+CONTEXT],rax ; lea rcx,[querySize] ; mov rax,[renderer global] ; mov [rax+0x730],rcx ; mov rcx,rdi ; call ; mov rcx,rdi ; call`
    { 0x3537d9, 4, "mov rax,[rip+..]: the renderer global (only the store of the lea into it, +0x730, is pinned)" },
    { 0x3537e8, 4, "E8 rel32: first constructor call, forwards control only" },
    { 0x3537f0, 4, "E8 rel32: second constructor call, forwards control only" },
    // camera producer window: three global loads, only the first is decoded (the camera global)
    { 0x3740d3, 4, "mov rdi,[rip+..]: the second global of the producer, the camera is read through the first" },
    { 0x3740da, 4, "mov rbx,[rip+..]: the third global of the producer" },
    // the jne behind each flags test: the DISTANCE over the slow path (the opcode, the test and the 0x30 mask are fixed)
    { 0x3745a6, 1, "75 rel8: distance of the jne over contextResolver's slow path (MEDIUM-4: an edit of that path must not kill the anchor)" },
    { 0x3747ab, 1, "75 rel8: distance of the jne behind test al,0x30 (same reason)" },
    // cacheCompare: the final `je` to the cache-hit path
    { 0x374f38, 4, "0F 84 rel32: the branch target of the final compare" },
};

struct Site
{
    std::size_t seam, anchor;
    std::uint32_t match;
    Pattern pattern;
};

std::vector<Site> Sites(const Registry& reg, const ResolvedBuild& base)
{
    std::vector<Site> sites;
    for (std::size_t si = 0; si < reg.seams.size(); ++si)
    {
        for (std::size_t ai = 0; ai < reg.seams[si].anchors.size(); ++ai)
        {
            Site s;
            s.seam = si;
            s.anchor = ai;
            const AnchorResult& r = base.seams[si].anchors[ai];
            if (!r.hasValue)
            {
                std::printf("FAIL: %s/%s has no baseline match\n", reg.seams[si].id.c_str(), reg.seams[si].anchors[ai].name.c_str());
                ++g_failures;
                continue;
            }
            s.match = r.match;
            if (!ParsePattern(reg.seams[si].anchors[ai].pattern, s.pattern))
            {
                std::printf("FAIL: %s/%s does not parse\n", reg.seams[si].id.c_str(), reg.seams[si].anchors[ai].name.c_str());
                ++g_failures;
                continue;
            }
            sites.push_back(std::move(s));
        }
    }
    return sites;
}

// The bytes that an anchor's decode reads (a displacement, a rel32, a field).
void DecodedBytes(const AnchorSpec& a, std::uint32_t match, std::set<std::uint32_t>& out)
{
    const std::uint32_t insn = match + static_cast<std::uint32_t>(a.decode.at);
    switch (a.decode.kind)
    {
    case DecodeKind::RipDisp32:
        for (std::uint32_t k = 0; k < 4; ++k)
            out.insert(insn + static_cast<std::uint32_t>(a.decode.dispAt) + k);
        break;
    case DecodeKind::Rel32:
        for (std::uint32_t k = 1; k < 5; ++k)
            out.insert(insn + k);
        break;
    case DecodeKind::Field:
        for (int k = 0; k < a.decode.width; ++k)
            out.insert(insn + static_cast<std::uint32_t>(k));
        break;
    default:
        break;
    }
}

std::string Runs(const std::set<std::uint32_t>& rvas)
{
    std::string text;
    for (auto it = rvas.begin(); it != rvas.end();)
    {
        std::uint32_t first = *it, last = *it;
        for (++it; it != rvas.end() && *it == last + 1; ++it)
            last = *it;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s{ 0x%x, %u }", text.empty() ? "" : ", ", first, last - first + 1);
        text += buf;
    }
    return text;
}

// 1. over-masking ---------------------------------------------------------------------------------------------------------------------------
void TestMaskedBytesAreAllowListed()
{
    const Registry reg = QualityRegistry();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    const ResolvedBuild base = ResolveImage(*img, reg);
    const std::vector<Site> sites = Sites(reg, base);

    std::set<std::uint32_t> masked, decoded;
    for (const Site& s : sites)
    {
        for (std::size_t k = 0; k < s.pattern.size(); ++k)
        {
            CHECK(s.pattern.mask[k] == 0 || s.pattern.mask[k] == 0xff); // no nibble wildcards in this group: a byte is fixed or free
            if (s.pattern.mask[k] == 0)
                masked.insert(s.match + static_cast<std::uint32_t>(k));
        }
        DecodedBytes(reg.seams[s.seam].anchors[s.anchor], s.match, decoded);
    }
    std::set<std::uint32_t> undecoded;
    for (std::uint32_t rva : masked)
    {
        if (decoded.count(rva) == 0)
            undecoded.insert(rva);
    }
    std::set<std::uint32_t> allowed;
    for (const Span& a : kMaskedOnPurpose)
    {
        for (std::uint32_t k = 0; k < a.length; ++k)
            CHECK(allowed.insert(a.rva + k).second); // no overlaps
    }
    std::set<std::uint32_t> unexplained, stale;
    for (std::uint32_t rva : undecoded)
    {
        if (allowed.count(rva) == 0)
            unexplained.insert(rva);
    }
    for (std::uint32_t rva : allowed)
    {
        if (undecoded.count(rva) == 0)
            stale.insert(rva);
    }
    if (!unexplained.empty())
        std::printf("FAIL: masked and decoded by nobody, not in kMaskedOnPurpose: %s\n", Runs(unexplained).c_str());
    if (!stale.empty())
        std::printf("FAIL: in kMaskedOnPurpose but not (any more) a masked, undecoded byte: %s\n", Runs(stale).c_str());
    CHECK(unexplained.empty() && stale.empty());
    std::printf("    masked positions %zu, decoded %zu, masked and undecoded %zu, allow-listed %zu\n", masked.size(), decoded.size(), undecoded.size(),
                allowed.size());
}

// 2. every fixed byte is enforced -----------------------------------------------------------------------------------------------------------
struct Hook
{
    const char* shape;
    const char* guardKey;
};

void TestEveryFixedByteIsEnforced()
{
    const Registry reg = QualityRegistry();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    const ResolvedBuild base = ResolveImage(*img, reg);
    const std::vector<Site> sites = Sites(reg, base);
    REQUIRE(sites.size() > 0);

    // rva -> the (site index) list of the anchors that FIX it
    std::map<std::uint32_t, std::vector<std::size_t>> fixedBy;
    for (std::size_t i = 0; i < sites.size(); ++i)
    {
        for (std::size_t k = 0; k < sites[i].pattern.size(); ++k)
        {
            if (sites[i].pattern.mask[k] != 0)
                fixedBy[sites[i].match + static_cast<std::uint32_t>(k)].push_back(i);
        }
    }
    const Hook hooks[] = { { "quality.querySizeShape", "nativeQuality.querySizeInstructions" },
                           { "quality.rendererUpdateShape", "nativeQuality.rendererUpdateInstructions" },
                           { "quality.resizeCallbacksShape", "nativeQuality.resizeCallbacksInstructions" } };
    std::size_t flips = 0, shapeFlips = 0;
    for (const auto& entry : fixedBy)
    {
        const std::uint32_t rva = entry.first;
        const std::uint8_t was = ByteAt(*img, rva);
        SetByte(*img, rva, static_cast<std::uint8_t>(was ^ 0xff));
        const ResolvedBuild rb = ResolveImage(*img, reg);
        SetByte(*img, rva, was); // restored
        ++flips;

        // per anchor: the anchors that fix this byte are no longer Unique
        std::map<std::size_t, std::size_t> hit; // seam -> how many of its anchors fix this byte
        for (std::size_t i : entry.second)
        {
            const Site& s = sites[i];
            const AnchorResult& r = rb.seams[s.seam].anchors[s.anchor];
            if (r.outcome == AnchorOutcome::Unique)
                std::printf("FAIL: 0x%x flipped: anchor %s/%s is still Unique\n", rva, reg.seams[s.seam].id.c_str(), reg.seams[s.seam].anchors[s.anchor].name.c_str());
            CHECK(r.outcome != AnchorOutcome::Unique);
            ++hit[s.seam];
        }
        // per seam: not plain Found unless at least two other anchors still agree; NotFound when every anchor fixes the byte
        for (const auto& h : hit)
        {
            const SeamSpec& spec = reg.seams[h.first];
            const std::size_t n = spec.anchors.size();
            const SeamStatus st = rb.seams[h.first].status;
            const bool mayStayFound = n - h.second >= 2;
            if ((st == SeamStatus::Found && !mayStayFound) || (h.second == n && st != SeamStatus::NotFound))
                std::printf("FAIL: 0x%x flipped: seam %s (%zu of %zu anchors fix it) is %s\n", rva, spec.id.c_str(), h.second, n, StatusName(st));
            CHECK(!(st == SeamStatus::Found && !mayStayFound));
            CHECK(!(h.second == n && st != SeamStatus::NotFound));
        }
        // a SHAPE seam: refused, and the Guard field it carries is zero (the hooks' compare then refuses)
        for (const Hook& hook : hooks)
        {
            for (std::size_t i : entry.second)
            {
                if (reg.seams[sites[i].seam].id != hook.shape)
                    continue;
                ++shapeFlips;
                const SeamResult& shape = Get(rb, hook.shape);
                if (shape.status != SeamStatus::NotFound || !AllZero(GuardOf(rb.build, hook.guardKey)))
                    std::printf("FAIL: 0x%x flipped: %s is %s, Guard %szero\n", rva, hook.shape, StatusName(shape.status),
                                AllZero(GuardOf(rb.build, hook.guardKey)) ? "" : "NOT ");
                CHECK(shape.status == SeamStatus::NotFound && AllZero(GuardOf(rb.build, hook.guardKey)));
            }
        }
    }
    std::printf("    fixed bytes flipped: %zu distinct positions (%zu of them inside a shape)\n", flips, shapeFlips);
    CHECK(flips > 400); // a registry that lost its patterns would flip nothing
    // the image is back to the baseline
    const ResolvedBuild again = ResolveImage(*img, reg);
    for (const SeamResult& s : base.seams)
        CHECK(Get(again, s.id.c_str()).status == s.status && Get(again, s.id.c_str()).rva == s.rva);
}

// 3. the patch region -----------------------------------------------------------------------------------------------------------------------
void TestPatchRegionIsFixed()
{
    const Registry reg = QualityRegistry();
    struct Entry
    {
        const char* seam;
        const char* anchor;
    };
    // the entry pattern of each hooked function and the strict shape pattern that is verified at the same address
    const Entry entries[] = { { "quality.querySize", "entry" },
                              { "quality.querySizeShape", "shape" },
                              { "quality.rendererUpdate", "entry" },
                              { "quality.rendererUpdateShape", "shape" },
                              { "quality.resizeCallbacks", "entry" },
                              { "quality.resizeCallbacksShape", "shape" } };
    for (const Entry& e : entries)
    {
        const SeamSpec* seam = FindSeam(reg, e.seam);
        REQUIRE(seam != nullptr);
        const AnchorSpec* anchor = nullptr;
        for (const AnchorSpec& a : seam->anchors)
        {
            if (a.name == e.anchor)
                anchor = &a;
        }
        REQUIRE(anchor != nullptr);
        Pattern p;
        REQUIRE(ParsePattern(anchor->pattern, p));
        REQUIRE(p.size() >= 14);
        for (std::size_t k = 0; k < 14; ++k)
        {
            if (p.mask[k] != 0xff)
                std::printf("FAIL: %s/%s: byte %zu of the patch region is not fixed\n", e.seam, e.anchor, k);
            CHECK(p.mask[k] == 0xff);
        }
    }
}
} // namespace

int main()
{
    TestMaskedBytesAreAllowListed();
    TestEveryFixedByteIsEnforced();
    TestPatchRegionIsFixed();
    return TEST_RESULT("resolve_quality_sweep");
}
