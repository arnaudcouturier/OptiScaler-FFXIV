#include "check.h"

#include "ext/ffxiv/jitter/FfxivJitterSourceAnchors.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter::source;

// The instruction guards of the source fix against the bytes that were READ from the supported executable (ffxiv_dx11.exe, the October 20 build,
// SHA-256 46A52AB1DBE80B9A8D6C4D4531BF66693904200C5135FACACD053EF28A5DE14A, TimeDateStamp 0x6ad10bfd, SizeOfImage 0x381f000) when the table
// was written: the table must still hold exactly those bytes at those addresses, a wrong build or any changed byte must be refused, and the
// addresses and offsets in the table must be the ones the bytes themselves encode (call targets, rip-relative globals, structure offsets).

namespace
{
struct Recorded
{
    const char* name;
    std::uint32_t rva;
    const char* hex;
};

// Read from the executable at each RVA (tools: a read-only byte dump of the file, section table applied).
const Recorded kRecorded[] = {
    { "generator", 0x2ebd90, "488bc44889580848897010574883ec70" },
    { "cameraGetter", 0x25f610, "488b8178020000c3" },
    { "producerCall", 0x37411d, "e8eeb4eeff0f57c041bdc40302" },
    { "cameraHead", 0x25f690,
      "405557488dac2468feffff4881ec98020000488b05dfc568024833c4488985d0000000488b01488bf948899c24b8020000ff5038488b05a5ed6a0232d2440fb64054"
      "4180f802740683782c02750f" },
    { "phases", 0x25f721,
      "488b0568ed6a020f57f6488b9f78020000baffffffff488b8828040000488bf18bc148c1ee20f3480f2af04180f8027433488b05a7856a020f57c0f30f100d3ce7eb01"
      "8b888c000000f3480f2ac1f30f5ec6e88865b801f30f5905bce6ee01f3480f2cd0448b4b104c8d430489542420"
      "488d4c2430488bd3" },
    { "strength", 0x25f799,
      "e8f2c50800488b0debec6a024c8d45d0488b03488d575048894424380fb78100070000660f6ec80fb78104070000488b8f780200000f5bc9660f6ec0488b0594ec6a02"
      "0f5bc0f30f105078f30f5c1578e5eb01f30f5ec80f57c0f30f59484cf3480f2ac6f30f59c9f30f59d1f30f581557e5eb01f30f58d20f28caf30f5954243cf30f594c24"
      "380f571569e8eb01f30f5ed00f2805aee7eb01f30f5ece" },
    { "deviceSize", 0xd3f73, "488b0d863d8302448b81900000008b918c000000" },
};

std::vector<std::uint8_t> FromHex(const std::string& hex)
{
    std::vector<std::uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

const Anchor* Find(const std::array<Anchor, kAnchorCount>& a, const char* name)
{
    for (const Anchor& x : a)
        if (std::string(x.name) == name)
            return &x;
    return nullptr;
}

std::vector<std::uint8_t> Bytes(const Anchor& a) { return std::vector<std::uint8_t>(a.bytes, a.bytes + a.size); }

struct Image
{
    std::map<std::uintptr_t, std::uint8_t> bytes;
    bool Read(std::uintptr_t address, void* out, std::size_t n) const
    {
        auto* p = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto it = bytes.find(address + i);
            if (it == bytes.end())
                return false;
            p[i] = it->second;
        }
        return true;
    }
};

constexpr std::uintptr_t kBase = 0x140000000ull;

Image MakeImage(const JitterSourceBuild& b, std::uintptr_t base = kBase)
{
    Image image;
    for (const Anchor& a : Anchors(b))
        for (std::size_t i = 0; i < a.size; ++i)
            image.bytes[base + a.rva + i] = a.bytes[i];
    return image;
}

bool Contains(const std::vector<std::uint8_t>& hay, const std::vector<std::uint8_t>& needle)
{
    if (needle.empty() || hay.size() < needle.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0)
            return true;
    return false;
}

std::vector<std::uint8_t> Le32(std::uint32_t v)
{
    return { std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16), std::uint8_t(v >> 24) };
}

std::int32_t S32(const std::vector<std::uint8_t>& b, std::size_t at)
{
    std::int32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

// Every rip-relative `mov r64,[rip+disp32]` (REX.W 48/4c, 8b, modrm mod=00 rm=101) in a block, as its target RVA.
std::vector<std::uint32_t> RipLoads(const Anchor& a)
{
    std::vector<std::uint32_t> out;
    const auto b = Bytes(a);
    for (std::size_t i = 0; i + 7 <= b.size(); ++i)
        if ((b[i] == 0x48 || b[i] == 0x4c) && b[i + 1] == 0x8b && (b[i + 2] & 0xC7) == 0x05)
            out.push_back(static_cast<std::uint32_t>(a.rva + i + 7 + S32(b, i + 3)));
    return out;
}

bool HasTarget(const std::vector<std::uint32_t>& v, std::uint32_t rva)
{
    for (auto t : v)
        if (t == rva)
            return true;
    return false;
}

// The two render heights the strength block reads, and where the game keeps the first of them. The block at RVA 0x2dab0d, read from
// the supported executable, chooses RenderHeight (+0x702) or MaxRenderHeight (+0x704) with the DLSS / dynamic-resolution tests, stores
// the choice at +0x700, and the camera update's strength block divides that +0x700 by +0x704. So +0x700 is a field of its own (the
// structure notes leave it unnamed) and not a misaligned read of RenderHeight: this checks the table's two reads against the
// instructions that define and use the field, so moving them to a neighbouring structure field fails here.
void TestExtentFieldIsTheDerivedRenderHeight(const JitterSourceBuild& b)
{
    // mov rcx,[r14+0x30] ; call resizeCallbacks ; mov rax,[settings] ; cmp byte [rax+0x54],2 ; je ; cmp byte [rax+0x44],0 ;
    // je ; cmp byte [rax+0x45],0 ; je ; movzx eax,word [rbx+0x702] ; jmp ; movzx eax,word [rbx+0x704] ; mov [rbx+0x700],ax
    const std::vector<std::uint8_t> resizeBlock = FromHex("498b4e30e88abef5ff488b055339630280785402740680784400740f8078450074090f"
                                                          "b78302070000eb070fb7830407000066898300070000");
    const auto loadWord = [](std::uint32_t offset)
    {
        std::vector<std::uint8_t> p { 0x0f, 0xb7, 0x83 }; // movzx eax, word [rbx+offset]
        for (auto v : Le32(offset))
            p.push_back(v);
        return p;
    };
    const auto storeWord = [](std::uint32_t offset)
    {
        std::vector<std::uint8_t> p { 0x66, 0x89, 0x83 }; // mov [rbx+offset], ax
        for (auto v : Le32(offset))
            p.push_back(v);
        return p;
    };
    CHECK(resizeBlock.size() == 57);
    CHECK(Contains(resizeBlock, loadWord(0x702))); // RenderHeight, the height the scene targets are rendered at
    CHECK(Contains(resizeBlock, loadWord(0x704))); // MaxRenderHeight, used while render scaling is off
    CHECK(Contains(resizeBlock, storeWord(0x700))); // what the camera's strength formula reads
    // the table's pair is the field this block writes and the field it falls back to, four bytes apart
    CHECK(b.rendererExtentOffset == 0x700);
    CHECK(Contains(resizeBlock, loadWord(b.rendererExtentOffset + 4)));
    CHECK(Contains(resizeBlock, storeWord(b.rendererExtentOffset)));
}

void TestTableMatchesRecordedBytes()
{
    const JitterSourceBuild& b = kCurrentBuild.jitterSource;
    const auto anchors = Anchors(b);
    for (const Recorded& r : kRecorded)
    {
        const Anchor* a = Find(anchors, r.name);
        CHECK_MSG(a != nullptr, "anchor %s present", r.name);
        if (!a)
            continue;
        CHECK_MSG(a->rva == r.rva, "%s rva %x vs recorded %x", r.name, a->rva, r.rva);
        CHECK_MSG(Bytes(*a) == FromHex(r.hex), "%s bytes equal the bytes read from the executable", r.name);
    }
    CHECK(anchors.size() == sizeof(kRecorded) / sizeof(kRecorded[0]));
    // the executable identity the bytes belong to
    CHECK(kCurrentBuild.timeDateStamp == 0x6ad10bfd && kCurrentBuild.sizeOfImage == 0x381f000);
}

void TestAddressesAreWhatTheBytesEncode()
{
    const JitterSourceBuild& b = kCurrentBuild.jitterSource;
    const auto anchors = Anchors(b);

    // the NGX packet producer calls the camera getter: e8 rel32, the call is the first instruction of the block, its return address follows it
    {
        const auto bytes = Bytes(*Find(anchors, "producerCall"));
        CHECK(bytes[0] == 0xe8);
        CHECK(b.producerCallRva + 5 + S32(bytes, 1) == b.cameraGetterRva);
        CHECK(b.producerReturnRva == b.producerCallRva + 5);
    }
    // the camera update calls the generator: the call is the first instruction of the strength block
    {
        const auto bytes = Bytes(*Find(anchors, "strength"));
        CHECK(bytes[0] == 0xe8);
        CHECK(b.strengthRva + 5 + S32(bytes, 1) == b.generatorRva);
        CHECK(b.generatorCallReturnRva == b.strengthRva + 5);
    }
    // the globals: the loads in the bytes target the RVAs of the table
    const auto head = RipLoads(*Find(anchors, "cameraHead"));
    const auto phases = RipLoads(*Find(anchors, "phases"));
    const auto strength = RipLoads(*Find(anchors, "strength"));
    const auto deviceSize = RipLoads(*Find(anchors, "deviceSize"));
    CHECK(HasTarget(head, b.settingsPointerRva));
    CHECK(HasTarget(phases, b.rendererPointerRva) && HasTarget(phases, b.devicePointerRva));
    CHECK(HasTarget(strength, b.rendererPointerRva) && HasTarget(strength, b.settingsPointerRva));
    CHECK(HasTarget(deviceSize, b.devicePointerRva));
    // the same globals as the other users of this build's table
    CHECK(b.settingsPointerRva == kCurrentBuild.nativeQuality.settingsPointerRva);
    CHECK(b.devicePointerRva == kCurrentBuild.nativeQuality.devicePointerRva);
    CHECK(b.rendererPointerRva == kCurrentBuild.charaView.managerPointerRva);
    CHECK(b.rendererSceneSizeOffset == kCurrentBuild.nativeQuality.rendererSceneSizeOffset);
    CHECK(b.settingsModeOffset == kCurrentBuild.nativeQuality.settingsModeOffset);
}

void TestOffsetsAreInTheBytes()
{
    const JitterSourceBuild& b = kCurrentBuild.jitterSource;
    const auto anchors = Anchors(b);
    const auto head = Bytes(*Find(anchors, "cameraHead"));
    const auto phases = Bytes(*Find(anchors, "phases"));
    const auto strength = Bytes(*Find(anchors, "strength"));
    const auto deviceSize = Bytes(*Find(anchors, "deviceSize"));

    // settings mode byte: movzx r8d, byte [rax+0x54]
    CHECK(Contains(head, { 0x44, 0x0f, 0xb6, 0x40, std::uint8_t(b.settingsModeOffset) }));
    CHECK(b.settingsModeOffset == 0x54);
    // renderer scene size: mov rcx, [rax+0x428] (width in the low half, height in the high half)
    {
        std::vector<std::uint8_t> p { 0x48, 0x8b, 0x88 };
        for (auto v : Le32(b.rendererSceneSizeOffset))
            p.push_back(v);
        CHECK(Contains(phases, p));
    }
    // the unbounded phase count: mov edx, 0xFFFFFFFF
    CHECK(Contains(phases, { 0xba, 0xff, 0xff, 0xff, 0xff }));
    // the DLSS-mode branch and the counter / arguments of the generator call: cmp r8b,2 ; je ; mov r9d,[rbx+0x10] ; lea r8,[rbx+4] ; mov [rsp+0x20],edx
    CHECK(Contains(phases, { 0x41, 0x80, 0xf8, 0x02, 0x74 }));
    CHECK(Contains(phases, { 0x44, 0x8b, 0x4b, 0x10, 0x4c, 0x8d, 0x43, 0x04, 0x89, 0x54, 0x24, 0x20 }));
    // F10: the block runs up to the strength block with no gap: it ends with the generator's remaining arguments, lea rcx,[rsp+0x30] (the
    // opaque argument) and mov rdx,rbx (the generator's x output = the jitter object P, the pointer the world tie compares). The bytes at
    // 0x25f791..0x25f798 were read from the executable: 48 8d 4c 24 30 48 8b d3, and the strength block (the generator call) follows at once.
    {
        const Anchor& a = *Find(anchors, "phases");
        const Anchor& next = *Find(anchors, "strength");
        CHECK(b.phasesRva + a.size == b.strengthRva && a.rva + a.size == next.rva);
        const std::vector<std::uint8_t> gap { 0x48, 0x8d, 0x4c, 0x24, 0x30, 0x48, 0x8b, 0xd3 };
        const bool endsWithGap = phases.size() >= gap.size() && std::equal(gap.begin(), gap.end(), phases.end() - gap.size());
        CHECK(endsWithGap);
        CHECK(Contains(phases, { 0x48, 0x8b, 0xd3 })); // mov rdx,rbx
    }
    // device output width: mov ecx,[rax+0x8c]
    {
        std::vector<std::uint8_t> p { 0x8b, 0x88 };
        for (auto v : Le32(b.deviceOutputSizeOffset))
            p.push_back(v);
        CHECK(Contains(phases, p));
    }
    // the output size as a pair, width at +0x8c and height at +0x90 (mov r8d,[rcx+0x90] ; mov edx,[rcx+0x8c])
    {
        std::vector<std::uint8_t> height { 0x44, 0x8b, 0x81 }, width { 0x8b, 0x91 };
        for (auto v : Le32(b.deviceOutputSizeOffset + 4))
            height.push_back(v);
        for (auto v : Le32(b.deviceOutputSizeOffset))
            width.push_back(v);
        CHECK(Contains(deviceSize, height) && Contains(deviceSize, width));
    }
    // the extents: movzx eax, word [rcx+0x700] and [rcx+0x704]
    {
        std::vector<std::uint8_t> e0 { 0x0f, 0xb7, 0x81 }, e2 { 0x0f, 0xb7, 0x81 };
        for (auto v : Le32(b.rendererExtentOffset))
            e0.push_back(v);
        for (auto v : Le32(b.rendererExtentOffset + 4))
            e2.push_back(v);
        CHECK(Contains(strength, e0) && Contains(strength, e2));
        CHECK(b.rendererExtentOffset == 0x700);
        // the second read is the next u16 of the same pair, four bytes on: a table that moved either read to a neighbouring
        // field (the structure notes name RenderHeight at +0x702) would break the strength formula, not just an anchor
        CHECK(b.rendererExtentOffset + 4 == 0x704);
        CHECK(e0[3] == 0x00 && e0[4] == 0x07 && e2[3] == 0x04 && e2[4] == 0x07);
        TestExtentFieldIsTheDerivedRenderHeight(b);
    }
    // amount: movss xmm2,[rax+0x78] ; scale: mulss xmm1,[rax+0x4c]
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x10, 0x50, std::uint8_t(b.settingsAmountOffset) }));
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x59, 0x48, std::uint8_t(b.settingsScaleOffset) }));
    CHECK(b.settingsAmountOffset == 0x78 && b.settingsScaleOffset == 0x4c);
    // the strength expression: subss c ; divss ; mulss scale ; mulss r,r ; mulss s,r ; addss c ; (doubling) addss s,s ; ... ; divss by the size
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x5c, 0x15 }));       // subss xmm2,[rip+...]  (amount - 0.001f)
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x5e, 0xc8 }));       // divss xmm1,xmm0      (extent0 / extent2)
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x59, 0xc9 }));       // mulss xmm1,xmm1      (ratio * ratio)
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x59, 0xd1 }));       // mulss xmm2,xmm1      ((amount - c) * ratio^2)
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x58, 0x15 }));       // addss xmm2,[rip+...] (+ 0.001f)
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x58, 0xd2 }));       // addss xmm2,xmm2      (2 * strength)
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x59, 0x54, 0x24, 0x3c, 0xf3, 0x0f, 0x59, 0x4c, 0x24, 0x38 })); // times jy, times jx
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x5e, 0xd0 }));       // divss xmm2,xmm0     (/ height)
    CHECK(Contains(strength, { 0xf3, 0x0f, 0x5e, 0xce }));       // divss xmm1,xmm6     (/ width)
}

void TestVerifyOnTheRecordedImage(const std::string& repo)
{
    // The consumer's read of the table's pair: the glue hands the core the u16 at the table's offset and the one after the
    // skipped u16 (extent[2]), never the neighbouring RenderHeight at +0x702 that the same 6-byte read also covers. A glue
    // that read extent[1] would compute the strength from RenderHeight instead of the height the scene is drawn at.
    {
        std::ifstream f(repo + "/OptiScaler/ext/ffxiv/jitter/FfxivJitterSourceHooks.cpp", std::ios::binary);
        const std::string hooks { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
        CHECK(hooks.size() > 2000);
        CHECK(hooks.find("in.extent0 = extent[0];") != std::string::npos);
        CHECK(hooks.find("in.extent2 = extent[2];") != std::string::npos);
        CHECK(hooks.find("in.extent0 = extent[1];") == std::string::npos);
        CHECK(hooks.find("ReadBytes(renderer + b.rendererExtentOffset, extent, sizeof(extent))") != std::string::npos);
    }
    const JitterSourceBuild& b = kCurrentBuild.jitterSource;
    const Image image = MakeImage(b);
    auto reader = [&](std::uintptr_t a, void* out, std::size_t n) { return image.Read(a, out, n); };
    AnchorResult r = VerifyAnchors(reader, b, kBase);
    CHECK(r.ok);

    // a module at another base is the same image
    const Image shifted = MakeImage(b, 0x7ff600000000ull);
    r = VerifyAnchors([&](std::uintptr_t a, void* out, std::size_t n) { return shifted.Read(a, out, n); }, b, 0x7ff600000000ull);
    CHECK(r.ok);

    // the image is not where it is looked for: refused, naming the first block
    r = VerifyAnchors(reader, b, kBase + 0x1000);
    CHECK(!r.ok && std::string(r.failed) == "generator");

    // every single byte of every block, changed one at a time, is refused and names ITS block
    int tried = 0;
    for (const Anchor& a : Anchors(b))
    {
        for (std::size_t i = 0; i < a.size; ++i)
        {
            Image broken = image;
            broken.bytes[kBase + a.rva + i] ^= 0x01;
            const AnchorResult bad = VerifyAnchors([&](std::uintptr_t addr, void* out, std::size_t n) { return broken.Read(addr, out, n); }, b, kBase);
            CHECK_MSG(!bad.ok && std::string(bad.failed) == a.name, "block %s byte %zu", a.name, i);
            ++tried;
        }
    }
    CHECK(tried > 400);

    // an unreadable byte is a refusal too
    for (const Anchor& a : Anchors(b))
    {
        Image missing = image;
        missing.bytes.erase(kBase + a.rva + a.size - 1);
        const AnchorResult bad = VerifyAnchors([&](std::uintptr_t addr, void* out, std::size_t n) { return missing.Read(addr, out, n); }, b, kBase);
        CHECK_MSG(!bad.ok && std::string(bad.failed) == a.name, "unreadable %s", a.name);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    TestTableMatchesRecordedBytes();
    TestAddressesAreWhatTheBytesEncode();
    TestOffsetsAreInTheBytes();
    TestVerifyOnTheRecordedImage(repo);
    return TEST_RESULT("jitter_source_anchors");
}
