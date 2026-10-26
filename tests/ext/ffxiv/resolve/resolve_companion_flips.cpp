// P2bfix review #4: the over-masking blind spot. The behavioural tests prove hits, decoys and decodes; the pinned text proves the text; neither
// proves that a byte the consumer relies on is still FIXED when a pattern and its pin are edited together (a "lockstep" over-mask keeps exactly
// one hit and a margin on the real executable). Two literal tables close it, both frozen: kMaskMap was generated ONCE from today's patterns and
// each masked range classified against the executable (capstone: the instruction operand it belongs to), so it no longer moves with the registry;
// kRelied is written independently of the patterns, from the consumers and the disassembly:
//
//   1. kMaskMap, one line per anchor of the registry: the EXACT set of masked byte offsets of its pattern, each range with a class:
//        d  decoded by this anchor's own decode (a struct displacement, a call target, a RIP displacement the seam reads back and range-checks)
//        f  decoded by a SIBLING anchor over the same pattern text (the three renderer offsets share one pattern: each seam decodes its own
//           displacement and the other two are masked, but read by the sibling seams)
//        R  the rel32 of a call / jmp / jcc that nobody decodes (position dependent, the consumer never reads it)      } the allow-list of
//        P  a RIP-relative displacement that nobody decodes                                                            } FREE masked bytes
//        S  a stack spill slot of the drawStateFlagOffset anchor (frame layout; the shape / argumentMapping anchors keep them fixed)
//        L  the two vertex displacements of the layout lookup (not compared with the BatchVertexSlot decode)
//      The test flips every bit of every byte of every anchor's span on the committed real bytes: a byte is MASKED when some single-bit flip
//      still matches (so a nibble wildcard `7?` counts), and the masked set must equal the table. d / f ranges must lie inside the decode of the
//      anchor / of its family; R / P / S / L ranges must lie outside every decode (they are free: the short allow-list).
//   2. kRelied, the bytes the consumers rely on that no decode reads (found in review, and the Offset-seam neighbours), by RVA of the
//      executable with the number of anchors that cover each range today: every anchor that covers a relied byte must have it FIXED in all
//      eight bits (an over-mask edited together with the map still dies here), and flipping the first byte of the range in the image makes each
//      covering anchor stop matching at that site.
// A change of a pattern means editing its line here on purpose.
#include "resolve_companion_bytes.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>

using namespace rt;
using namespace cb;

namespace
{
struct MaskRow
{
    const char* key;    // "<seam>|<anchor>"
    const char* masked; // "a-b:C a-b:C": offsets inside the pattern, class C
};

const MaskRow kMaskMap[] = {
    { "companion.batch16|entryToIndexShift", "73-76:R" },
    { "companion.batch16|dispatcherCall", "21-24:f 28-31:d" },
    { "companion.batch16|dispatcherStub", "13-16:R 18-21:d" },
    { "companion.batch32|entryToIndexShift", "73-76:R" },
    { "companion.batch32|dispatcherCall", "21-24:d 28-31:f" },
    { "companion.batch16Shape|shape", "73-76:R" },
    { "companion.batch32Shape|shape", "73-76:R" },
    { "companion.enqueue|entry", "" },
    { "companion.enqueue|entryWriteTail", "" },
    { "companion.enqueue|callerEpilogue", "11-14:d" },
    { "companion.enqueueShape|shape", "" },
    { "companion.bindState|entry", "" },
    { "companion.bindState|argumentMapping", "43-46:R" },
    { "companion.bindState|contextCall", "12-15:f 28-31:P" },
    { "companion.bindStateShape|shape", "43-46:R" },
    { "companion.nameplateModulePointer|chainKeySet", "3-6:d" },
    { "companion.nameplateModulePointer|chainKeyReset", "3-6:d" },
    { "companion.nameplateModulePointer|chainSecondary", "3-6:d 13-16:R" },
    { "companion.moduleStatePointerOffset|chainKeySet", "3-6:f" },
    { "companion.moduleStatePointerOffset|chainKeyReset", "3-6:f" },
    { "companion.moduleStatePointerOffset|chainSecondary", "3-6:f 13-16:R" },
    { "companion.stateQueuePointerOffset|chainKeySet", "3-6:f" },
    { "companion.stateQueuePointerOffset|chainKeyReset", "3-6:f" },
    { "companion.stateQueuePointerOffset|chainSecondary", "3-6:f 13-16:R" },
    { "companion.layoutSlotTable|inBatchA", "3-6:P 11-14:d 25-28:L 32-35:L" },
    { "companion.layoutSlotTable|inBatchB", "3-6:P 11-14:d 25-28:L 32-35:L" },
    { "companion.layoutSlotTable|content", "" },
    { "companion.indexBufferMap|entry", "35-38:R 53-56:R" },
    { "companion.indexBufferMap|calledFromBatchA", "29-32:d" },
    { "companion.indexBufferMap|calledFromBatchB", "29-32:d" },
    { "companion.indexBufferMap|flagLogic", "2-2:f 22-25:R 28-28:f 40-43:R 46-49:P 61-61:f 78-78:f 93-93:f" },
    { "companion.rendererBatchListOffset|inBatch16", "3-6:d 22-25:f 32-35:f 43-46:R" },
    { "companion.rendererBatchListOffset|inBatch32", "3-6:d 22-25:f 32-35:f 43-46:R" },
    { "companion.rendererBatchCountOffset|inBatch16", "3-6:f 22-25:d 32-35:f 43-46:R" },
    { "companion.rendererBatchCountOffset|inBatch32", "3-6:f 22-25:d 32-35:f 43-46:R" },
    { "companion.rendererIndexBufferOffset|inBatch16", "3-6:f 22-25:f 32-35:d 43-46:R" },
    { "companion.rendererIndexBufferOffset|inBatch32", "3-6:f 22-25:f 32-35:d 43-46:R" },
    { "companion.rendererVertexOffsetOffset|inBatch16", "5-8:d 12-15:f 19-22:f" },
    { "companion.rendererVertexOffsetOffset|inBatch32", "5-8:d 12-15:f 19-22:f" },
    { "companion.rendererVertexAllocationOffset|inBatch16", "5-8:f 12-15:d 19-22:f" },
    { "companion.rendererVertexAllocationOffset|inBatch32", "5-8:f 12-15:d 19-22:f" },
    { "companion.rendererVertexStrideOffset|inBatch16", "5-8:f 12-15:f 19-22:d" },
    { "companion.rendererVertexStrideOffset|inBatch32", "5-8:f 12-15:f 19-22:d" },
    { "companion.batchFormatOffset|inBatch16", "4-4:d" },
    { "companion.batchFormatOffset|inBatch32", "4-4:d" },
    { "companion.batchExtensionOffset|inBatch16", "5-5:d" },
    { "companion.batchExtensionOffset|inBatch32", "5-5:d" },
    { "companion.drawStateFlagOffset|firstCompare", "2-2:d 11-11:S 19-19:S 24-24:S 32-32:S 35-38:R" },
    { "companion.nativeContextOffset|loadBeforeVirtualCall", "3-6:d" },
    { "companion.nativeContextOffset|loadBeforeStateCall", "12-15:d 28-31:P" },
    { "companion.indexBufferCapacityOffset|capacityLoad", "2-2:d 22-25:R 28-28:f 40-43:R 46-49:P 61-61:f 78-78:f 93-93:f" },
    { "companion.indexBufferFlagsOffset|flagsLoad", "2-2:f 22-25:R 28-28:d 40-43:R 46-49:P 61-61:f 78-78:f 93-93:f" },
    { "companion.indexBufferCpuStartOffset|startCompare", "2-2:f 22-25:R 28-28:f 40-43:R 46-49:P 61-61:d 78-78:f 93-93:f" },
    { "companion.indexBufferCpuStartOffset|startReturn", "2-2:f 22-25:R 28-28:f 40-43:R 46-49:P 61-61:f 78-78:d 93-93:f" },
    { "companion.indexBufferCpuEndOffset|endReturn", "2-2:f 22-25:R 28-28:f 40-43:R 46-49:P 61-61:f 78-78:f 93-93:d" },
    { "companion.entryPointerOffset|listEntryInBatch16", "11-11:d 17-20:R" },
    { "companion.entryPointerOffset|listEntryInBatch32", "11-11:d 17-20:R" },
    { "companion.entryPointerOffset|commandStore", "17-17:d" },
};

struct Relied
{
    std::uint32_t rva, len, covers; // covers: how many anchors of the registry span the whole range
    const char* what;
};

const Relied kRelied[] = {
    { 0x240b84, 3, 2, "enqueue: mov r11,rdx (the command pointer the Detour reads is the one stored in the entry)" },
    { 0x240c23, 4, 2, "enqueue tail: mov rdx,[r9+0x20] (the write pointer of the entry)" },
    { 0x240c27, 4, 2, "enqueue tail: lea rcx,[rdx+0x10] (16-byte entries)" },
    { 0x240c2b, 4, 2, "enqueue tail: mov [r9+0x20],rcx (the write pointer advances by one entry)" },
    { 0x240c2f, 2, 2, "enqueue tail: mov [rdx],eax (the entry key)" },
    { 0x240c31, 3, 2, "enqueue tail: mov [rdx+8],r11 without its displacement (the command goes into the entry)" },
    { 0x22d75c, 3, 4, "bindState: mov r14,rdx (the state argument)" },
    { 0x22d764, 3, 3, "bindState: mov rsi,rcx (the context argument)" },
    { 0x22d771, 3, 3, "bindState: mov r12d,r8d (the instances argument)" },
    { 0x22d8b4, 4, 2, "bindState: cmp [rbx+0x3c],2 (the index format of the state)" },
    { 0x22d8b8, 5, 2, "bindState: mov edx,0x39 (R16_UINT)" },
    { 0x22d8c4, 6, 2, "bindState: mov r8d,0x2a (R32_UINT)" },
    { 0x22d8ca, 4, 2, "bindState: cmovne r8d,edx (format selection)" },
    { 0x22d8d7, 3, 2, "bindState: mov rax,[rcx] (the vtable of the context)" },
    { 0x22d8df, 6, 2, "bindState: call [rax+0x98] (vtable slot 19 = IASetIndexBuffer)" },
    { 0x63b6fe, 6, 2, "dispatcher: cmp [rax+0x3c],2 ; jne +7 (which batch function gets which call)" },
    { 0x63b737, 6, 1, "dispatcher stub: cmp [rax+0x3c],2 ; je (the same choice)" },
    { 0x6e58b0, 11, 1, "batch16: the list entry read mov r9,[rbp-0x18] ; add rax,rax ; mov r12,[r9+rax*8+8] (16-byte entries, the batch header pointer)" },
    { 0x6e6450, 11, 1, "batch32: the list entry read (same)" },
    { 0x6e58ef, 1, 1, "batch16: cmp word [r12+4],2 (the extension constant)" },
    { 0x6e648f, 1, 1, "batch32: cmp word [r12+4],2" },
    { 0x6e593a, 3, 1, "batch16: and eax,0xf (the consumer's format & 15)" },
    { 0x6e64da, 3, 1, "batch32: and eax,0xf" },
    { 0x6e5a17, 4, 1, "batch16 layout lookup: movsxd rcx,[rcx+rbx*8+T] (table entry size 8)" },
    { 0x6e65b9, 4, 1, "batch32 layout lookup: movsxd rcx,[rcx+rbx*8+T]" },
    { 0x6e5a38, 12, 1, "batch16 layout lookup: lea rax,[rcx+0x50] ; add rax,rax ; mov ecx,[r13+rax*8] (slot stride 16)" },
    { 0x6e65da, 12, 1, "batch32 layout lookup: the same" },
    { 0x21eb9e, 8, 6, "index buffer map: test cl,0x11 ; je ; test cl,0x40 (the flag logic that selects the CPU start or end pointer)" },
    { 0x67362a, 3, 3, "chain site 1: mov rcx,[rax+8] (module -> renderer state)" },
    { 0x67362e, 3, 3, "chain site 1: mov rax,[rcx+8] (state -> queue)" },
    { 0x67369d, 3, 3, "chain site 2: mov rcx,[rax+8]" },
    { 0x6736a1, 3, 3, "chain site 2: mov rax,[rcx+8]" },
    { 0x6a532b, 3, 3, "chain site 3: mov rbx,[rax+8]" },
    { 0x6a5335, 3, 3, "chain site 3: mov rcx,[rbx+8]" },
};

const AnchorResult& AnchorOf(const ResolvedBuild& rb, const std::string& seam, const std::string& anchor)
{
    static const AnchorResult missing;
    const SeamResult* s = rb.Find(seam);
    if (s != nullptr)
    {
        for (const AnchorResult& a : s->anchors)
        {
            if (a.name == anchor)
                return a;
        }
    }
    std::printf("FAIL: no result for %s / %s\n", seam.c_str(), anchor.c_str());
    ++g_failures;
    return missing;
}

std::map<int, char> ParseMap(const char* text)
{
    std::map<int, char> m;
    std::string t = text;
    std::size_t i = 0;
    while (i < t.size())
    {
        while (i < t.size() && t[i] == ' ')
            ++i;
        if (i >= t.size())
            break;
        int a = 0, b = 0;
        auto number = [&](int& v)
        {
            if (i >= t.size() || t[i] < '0' || t[i] > '9')
                return false;
            v = 0;
            while (i < t.size() && t[i] >= '0' && t[i] <= '9')
                v = v * 10 + (t[i++] - '0');
            return true;
        };
        // "a-b:C"
        if (!number(a) || i >= t.size() || t[i] != '-' || !(++i, number(b)) || i + 1 >= t.size() || t[i] != ':')
        {
            std::printf("FAIL: bad mask map text '%s'\n", text);
            ++g_failures;
            return m;
        }
        const char c = t[i + 1];
        i += 2;
        for (int k = a; k <= b; ++k)
            m[k] = c;
    }
    return m;
}

// the bytes the anchor's own decode reads (a Rel32 reads the 4 bytes after its opcode; a RIP decode the disp32; a Field its width)
std::set<int> DecodedBytes(const AnchorSpec& a)
{
    std::set<int> s;
    const DecodeSpec& d = a.decode;
    switch (d.kind)
    {
    case DecodeKind::Field:
        for (int k = 0; k < d.width; ++k)
            s.insert(d.at + k);
        break;
    case DecodeKind::Rel32:
        for (int k = 1; k <= 4; ++k)
            s.insert(d.at + k);
        break;
    case DecodeKind::RipDisp32:
    case DecodeKind::RipDeref32:
        for (int k = 0; k < 4; ++k)
            s.insert(d.at + d.dispAt + k);
        break;
    case DecodeKind::None:
        break;
    }
    return s;
}

// a byte is masked when SOME single-bit flip of it still matches (a whole-byte flip would miss a nibble wildcard `7?`)
bool Masked(const std::vector<std::uint8_t>& bytes, const Pattern& p, std::size_t k)
{
    for (int bit = 0; bit < 8; ++bit)
    {
        std::vector<std::uint8_t> b = bytes;
        b[k] ^= static_cast<std::uint8_t>(1u << bit);
        if (MatchAt(b.data(), p))
            return true;
    }
    return false;
}

struct Site
{
    std::string seam, anchor;
    const AnchorSpec* spec = nullptr;
    Pattern pattern;
    std::vector<std::uint8_t> bytes;
    std::uint32_t match = 0;
};

std::vector<Site> Sites(const Registry& reg, const ResolvedBuild& rb, DenseImage& img)
{
    std::vector<Site> sites;
    for (const SeamSpec& s : reg.seams)
    {
        for (const AnchorSpec& a : s.anchors)
        {
            Site site;
            site.seam = s.id;
            site.anchor = a.name;
            site.spec = &a;
            const AnchorResult& ar = AnchorOf(rb, s.id, a.name);
            site.match = ar.match;
            if (ar.outcome != AnchorOutcome::Unique || !ParsePattern(a.pattern, site.pattern))
            {
                std::printf("FAIL: %s / %s is not a Unique anchor of the committed bytes\n", s.id.c_str(), a.name.c_str());
                ++g_failures;
                continue;
            }
            site.bytes.assign(img.Bytes().begin() + site.match, img.Bytes().begin() + site.match + static_cast<std::ptrdiff_t>(site.pattern.size()));
            sites.push_back(std::move(site));
        }
    }
    return sites;
}

void TestMaskMapIsExact()
{
    const Registry reg = CompanionRegistry();
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    const ResolvedBuild rb = ResolveImage(*img, reg);
    const std::vector<Site> sites = Sites(reg, rb, *img);
    std::size_t anchors = 0;
    for (const SeamSpec& s : reg.seams)
        anchors += s.anchors.size();
    CHECK(sites.size() == anchors);
    CHECK(sizeof(kMaskMap) / sizeof(kMaskMap[0]) == anchors);

    std::map<std::string, std::set<int>> family; // pattern text -> every byte any anchor over that text decodes
    for (const Site& s : sites)
    {
        const std::set<int> d = DecodedBytes(*s.spec);
        family[s.spec->pattern].insert(d.begin(), d.end());
    }

    std::set<std::string> used;
    std::map<char, std::size_t> perClass;
    std::size_t flipped = 0;
    for (const Site& s : sites)
    {
        const std::string key = s.seam + "|" + s.anchor;
        const MaskRow* row = nullptr;
        for (const MaskRow& r : kMaskMap)
        {
            if (key == r.key)
                row = &r;
        }
        if (row == nullptr || !used.insert(key).second)
        {
            std::printf("FAIL: %s has no (or a second) mask map line\n", key.c_str());
            ++g_failures;
            continue;
        }
        CHECK(MatchAt(s.bytes.data(), s.pattern));
        const std::map<int, char> want = ParseMap(row->masked);
        const std::set<int> own = DecodedBytes(*s.spec);
        const std::set<int>& fam = family[s.spec->pattern];
        for (std::size_t k = 0; k < s.bytes.size(); ++k)
        {
            ++flipped;
            const bool masked = Masked(s.bytes, s.pattern, k);
            const bool listed = want.count(static_cast<int>(k)) != 0;
            if (masked != listed)
                std::printf("FAIL: %s byte %zu (0x%x) is %s, the mask map says %s\n", key.c_str(), k, s.match + static_cast<std::uint32_t>(k),
                            masked ? "masked" : "fixed", listed ? "masked" : "fixed");
            CHECK(masked == listed);
        }
        for (const auto& [k, c] : want)
        {
            ++perClass[c];
            const bool inOwn = own.count(k) != 0, inFam = fam.count(k) != 0;
            const bool ok = c == 'd' ? inOwn : c == 'f' ? (inFam && !inOwn) : (c == 'R' || c == 'P' || c == 'S' || c == 'L') ? !inFam : false;
            if (!ok)
                std::printf("FAIL: %s byte %d class %c does not fit the decodes (own %d, family %d)\n", key.c_str(), k, c, inOwn, inFam);
            CHECK(ok);
        }
    }
    CHECK(used.size() == sizeof(kMaskMap) / sizeof(kMaskMap[0]));
    std::printf("    mask map: %zu anchors, %zu byte positions flipped bit by bit; masked bytes by class: d %zu, f %zu, free R %zu, P %zu, S %zu, L %zu\n",
                sites.size(), flipped, perClass['d'], perClass['f'], perClass['R'], perClass['P'], perClass['S'], perClass['L']);
}

void TestConsumerReliedBytes()
{
    const Registry reg = CompanionRegistry();
    auto img = MakeRealImage();
    REQUIRE(img != nullptr);
    const ResolvedBuild base = ResolveImage(*img, reg);
    const std::vector<Site> sites = Sites(reg, base, *img);
    std::size_t bytesChecked = 0, resolves = 0;
    for (const Relied& r : kRelied)
    {
        std::vector<const Site*> covering;
        for (const Site& s : sites)
        {
            if (s.match <= r.rva && r.rva + r.len <= s.match + s.pattern.size())
                covering.push_back(&s);
        }
        if (covering.size() != r.covers)
            std::printf("FAIL: 0x%x (%s) is covered by %zu anchors, the table says %u\n", r.rva, r.what, covering.size(), r.covers);
        CHECK(covering.size() == r.covers && r.covers >= 1);
        // pattern level: every bit of every byte of the range is fixed in every anchor that spans it
        for (const Site* s : covering)
        {
            for (std::uint32_t k = 0; k < r.len; ++k)
            {
                ++bytesChecked;
                const bool masked = Masked(s->bytes, s->pattern, r.rva + k - s->match);
                if (masked)
                    std::printf("FAIL: %s / %s masks the consumer-relied byte 0x%x (%s)\n", s->seam.c_str(), s->anchor.c_str(), r.rva + k, r.what);
                CHECK(!masked);
            }
        }
        // behaviour: the first byte flipped in the image, every covering anchor stops matching at that site (one raw match less)
        auto flipped = MakeRealImage();
        REQUIRE(flipped != nullptr);
        flipped->Bytes()[r.rva] ^= 0xff;
        const ResolvedBuild rb = ResolveImage(*flipped, reg);
        ++resolves;
        for (const Site* s : covering)
        {
            const AnchorResult& was = AnchorOf(base, s->seam, s->anchor);
            const AnchorResult& now = AnchorOf(rb, s->seam, s->anchor);
            if (now.patternHits + 1 != was.patternHits)
                std::printf("FAIL: flipping 0x%x (%s): %s / %s raw hits %u -> %u\n", r.rva, r.what, s->seam.c_str(), s->anchor.c_str(), was.patternHits,
                            now.patternHits);
            CHECK(now.patternHits + 1 == was.patternHits);
        }
    }
    std::printf("    consumer-relied: %zu ranges, %zu (anchor, byte) pairs fixed in all bits, %zu image-level flips\n", sizeof(kRelied) / sizeof(kRelied[0]),
                bytesChecked, resolves);
}
} // namespace

int main()
{
    TestMaskMapIsExact();
    TestConsumerReliedBytes();
    return TEST_RESULT("resolve_companion_flips");
}
