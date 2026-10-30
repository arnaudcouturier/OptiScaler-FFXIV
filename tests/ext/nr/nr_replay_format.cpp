// The on-disk format of an NR frame-replay set: what the writer produces, what the reader accepts and what it refuses.
// Pure code, nothing here touches Direct3D.
#include "ext/nr/diag/NrReplayFormat.h"

#include <cstdio>
#include <string>

using namespace ext::nr::replay;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

Manifest Sample()
{
    Manifest m;
    m.frames = 2;
    Image a;
    a.id = 0;
    a.file = "images/img_0000.raw";
    a.width = 4;
    a.height = 2;
    a.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    a.srcRowPitch = 256;
    a.rowBytes = 32;
    a.bytes = 64;
    a.hash = 0x0123456789abcdefull;
    Image b = a;
    b.id = 1;
    b.file = "images/img_0001.raw";
    b.hash = 0xfedcba9876543210ull;
    m.images = { a, b };

    Dispatch d;
    d.frame = 1;
    d.index = 3;
    d.kind = "resolve";
    d.shader = "dlssnr";
    d.shaderHash = 0xaabbccddeeff0011ull;
    d.groups[0] = 5;
    d.groups[1] = 6;
    d.groups[2] = 1;
    d.constants[0] = 1;
    d.constants[1] = 2;
    d.constants[2] = 3;
    d.constants[3] = 4;

    Resource r0;
    r0.local = 0;
    r0.width = 4;
    r0.height = 2;
    r0.resourceFormat = DXGI_FORMAT_R16G16B16A16_TYPELESS;
    r0.copyFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    r0.before = 0;
    Resource r1 = r0;
    r1.local = 1;
    r1.resourceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    r1.before = kNoImage;
    r1.after = 1;
    d.resources = { r0, r1 };

    Binding s;
    s.slot = 0;
    s.res = 0;
    s.viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    s.mips = 1;
    s.state = kStateNonPixelShaderResource;
    Binding u;
    u.uav = true;
    u.slot = 0;
    u.res = 1;
    u.viewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    u.array = true;
    u.mip = 1;
    u.firstSlice = 2;
    u.slices = 3;
    u.state = kStateUnorderedAccess;
    d.bindings = { s };
    for (uint32_t slot = 1; slot < kSrvSlots; ++slot)
    {
        Binding more = s;
        more.slot = slot;
        d.bindings.push_back(more);
    }
    d.bindings.push_back(u);
    Binding u1 = u;
    u1.slot = 1;
    d.bindings.push_back(u1);
    m.dispatches = { d };
    m.frameInfos = { { 0, 0, 0, 0 }, { 1, 4, 0, 0 } };

    Boundary bd;
    bd.frame = 1;
    bd.name = "game_frame";
    bd.image = 0;
    bd.resourceFormat = DXGI_FORMAT_R16G16B16A16_TYPELESS;
    m.boundaries = { bd };
    return m;
}

// The manifest text is a contract with captures taken in the past: this string must not change by accident.
std::string Golden()
{
    return std::string("optiscaler-nr-replay 2\n"
                       "frames 2\n"
                       "frameinfo frame=0 dispatches=0 skipped=0 boundariesSkipped=0\n"
                       "frameinfo frame=1 dispatches=4 skipped=0 boundariesSkipped=0\n"
                       "image id=0 file=images/img_0000.raw width=4 height=2 format=10 srcRowPitch=256 rowBytes=32 bytes=64 hash=0123456789abcdef\n"
                       "image id=1 file=images/img_0001.raw width=4 height=2 format=10 srcRowPitch=256 rowBytes=32 bytes=64 hash=fedcba9876543210\n"
                       "dispatch frame=1 index=3 kind=resolve shader=dlssnr shaderHash=aabbccddeeff0011 groups=5,6,1 constants=01020304") +
           std::string(504, '0') +
           "\n"
           "res local=0 width=4 height=2 resourceFormat=9 copyFormat=10 mips=1 arraySize=1 before=0 after=-\n"
           "res local=1 width=4 height=2 resourceFormat=10 copyFormat=10 mips=1 arraySize=1 before=- after=1\n"
           "bind type=srv slot=0 res=0 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=64\n"
           "bind type=srv slot=1 res=0 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=64\n"
           "bind type=srv slot=2 res=0 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=64\n"
           "bind type=srv slot=3 res=0 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=64\n"
           "bind type=srv slot=4 res=0 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=64\n"
           "bind type=srv slot=5 res=0 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=64\n"
           "bind type=uav slot=0 res=1 view=10 dim=2darray mip=1 slice=2 slices=3 mips=1 state=8\n"
           "bind type=uav slot=1 res=1 view=10 dim=2darray mip=1 slice=2 slices=3 mips=1 state=8\n"
           "boundary frame=1 name=game_frame image=0 resourceFormat=9\n"
           "end\n";
}

std::string Replace(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

void TestWriterIsPinned()
{
    CHECK(WriteManifest(Sample()) == Golden());
}

void TestRoundTrip()
{
    const ParseResult p = ParseManifest(Golden());
    CHECK(p.ok);
    CHECK(p.manifest.frames == 2);
    CHECK(p.manifest.images.size() == 2);
    CHECK(p.manifest.images[1].hash == 0xfedcba9876543210ull);
    CHECK(p.manifest.images[0].srcRowPitch == 256 && p.manifest.images[0].rowBytes == 32 && p.manifest.images[0].bytes == 64);
    CHECK(p.manifest.images[0].format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(p.manifest.dispatches.size() == 1);
    const Dispatch& d = p.manifest.dispatches[0];
    CHECK(d.frame == 1 && d.index == 3 && d.kind == "resolve" && d.shader == "dlssnr");
    CHECK(d.shaderHash == 0xaabbccddeeff0011ull);
    CHECK(d.groups[0] == 5 && d.groups[1] == 6 && d.groups[2] == 1);
    CHECK(d.constants[0] == 1 && d.constants[3] == 4 && d.constants[4] == 0 && d.constants[255] == 0);
    CHECK(d.resources.size() == 2 && d.bindings.size() == 8);
    CHECK(p.manifest.frameInfos.size() == 2 && p.manifest.frameInfos[1].frame == 1 && p.manifest.frameInfos[1].dispatches == 4);
    CHECK(d.resources[0].resourceFormat == DXGI_FORMAT_R16G16B16A16_TYPELESS && d.resources[0].copyFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(d.resources[0].before == 0 && d.resources[0].after == kNoImage);
    CHECK(d.resources[1].before == kNoImage && d.resources[1].after == 1);
    CHECK(!d.bindings[0].uav && d.bindings[0].state == kStateNonPixelShaderResource && !d.bindings[0].array);
    CHECK(d.bindings[6].uav && d.bindings[6].array && d.bindings[6].mip == 1 && d.bindings[6].firstSlice == 2 && d.bindings[6].slices == 3);
    CHECK(d.bindings[6].viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(p.manifest.boundaries.size() == 1 && p.manifest.boundaries[0].name == "game_frame" && p.manifest.boundaries[0].image == 0);
    // writing what was read gives the same bytes
    CHECK(WriteManifest(p.manifest) == Golden());
}

void TestRefusals()
{
    // another version of the format is refused with a message that names both versions
    for (const char* bad : { "optiscaler-nr-replay 0\n", "optiscaler-nr-replay 1\n", "optiscaler-nr-replay 3\n", "optiscaler-nr-replay 999\n" })
    {
        const ParseResult p = ParseManifest(std::string(bad) + "frames 1\nend\n");
        CHECK(!p.ok);
        CHECK(p.error.find("unsupported manifest version") != std::string::npos);
    }
    CHECK(!ParseManifest("something-else 1\nend\n").ok);
    CHECK(!ParseManifest("").ok);
    CHECK(!ParseManifest("optiscaler-nr-replay x\nend\n").ok);

    // a missing end record means the file was cut short
    {
        std::string text = Golden();
        text.erase(text.rfind("end\n"));
        const ParseResult p = ParseManifest(text);
        CHECK(!p.ok && p.error.find("truncated") != std::string::npos);
    }
    {
        // cut short in the middle (a failed cleanup left half a manifest): refused, never read as a set
        std::string text = Golden();
        text.erase(100);
        const ParseResult p = ParseManifest(text);
        CHECK(!p.ok);
    }
    {
        // cut short with no trailing newline at all
        std::string text = Golden();
        text.erase(text.find('\n', 30));
        const ParseResult p = ParseManifest(text);
        CHECK(!p.ok);
    }
    // each required key
    for (const char* key : { " width=4 height=2 format=10", " srcRowPitch=256", " rowBytes=32", " bytes=64", " hash=0123456789abcdef", " file=images/img_0000.raw" })
    {
        const ParseResult p = ParseManifest(Replace(Golden(), key, ""));
        CHECK(!p.ok);
    }
    CHECK(!ParseManifest(Replace(Golden(), " shaderHash=aabbccddeeff0011", "")).ok);
    CHECK(!ParseManifest(Replace(Golden(), " groups=5,6,1", "")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "groups=5,6,1", "groups=5,6")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "groups=5,6,1", "groups=5,6,1,1")).ok);
    CHECK(!ParseManifest(Replace(Golden(), " mips=1 arraySize=1 before=0", " mips=1 before=0")).ok);
    CHECK(!ParseManifest(Replace(Golden(), " state=64", "")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "type=srv", "type=cbv")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "dim=2d ", "dim=3d ")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "width=4 height=2 format=10 srcRowPitch", "width=four height=2 format=10 srcRowPitch")).ok);
    // constants must be exactly 256 bytes of lowercase hex
    CHECK(!ParseManifest(Replace(Golden(), "constants=0102", "constants=010")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "constants=0102", "constants=0102aa")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "constants=0102", "constants=01AB")).ok);
    // a record that has no dispatch to belong to
    CHECK(!ParseManifest("optiscaler-nr-replay 2\nres local=0 width=1 height=1 resourceFormat=1 copyFormat=1 mips=1 arraySize=1 before=- after=-\nend\n").ok);
    CHECK(!ParseManifest(Replace(Golden(), "boundary ", "wat ")).ok);
}

void TestConsistencyChecks()
{
    // an image the manifest does not define
    CHECK(!ParseManifest(Replace(Golden(), "before=0 after=-", "before=7 after=-")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "name=game_frame image=0", "name=game_frame image=9")).ok);
    // a binding on a resource that does not exist, and slots out of range
    CHECK(!ParseManifest(Replace(Golden(), "type=srv slot=0 res=0", "type=srv slot=0 res=5")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "type=srv slot=0", "type=srv slot=6")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "type=uav slot=0", "type=uav slot=2")).ok);
    // a ninth binding past the last slot is refused on its own (not only because a real slot went missing)
    CHECK(!ParseManifest(Replace(Golden(), "boundary ", "bind type=srv slot=6 res=0 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=64\nboundary ")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "boundary ", "bind type=uav slot=2 res=1 view=10 dim=2d mip=0 slice=0 slices=1 mips=1 state=8\nboundary ")).ok);
    // an output with no image after the dispatch could never be compared
    CHECK(!ParseManifest(Replace(Golden(), "before=- after=1", "before=- after=-")).ok);
    // sizes that disagree
    CHECK(!ParseManifest(Replace(Golden(), "rowBytes=32 bytes=64 hash=0123", "rowBytes=32 bytes=65 hash=0123")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "srcRowPitch=256 rowBytes=32 bytes=64 hash=0123", "srcRowPitch=16 rowBytes=32 bytes=64 hash=0123")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "image id=1", "image id=0")).ok);
}

void TestTolerantReading()
{
    // blank lines, comments, carriage returns and keys this reader does not know are not errors
    std::string text = Replace(Golden(), "frames 2\n", "# a comment\r\n\r\nframes 2\r\n");
    text = Replace(text, "kind=resolve", "kind=resolve futureKey=1");
    const ParseResult p = ParseManifest(text);
    CHECK(p.ok);
    CHECK(p.ok && p.manifest.dispatches[0].kind == "resolve");
}

void TestHashingAndRows()
{
    // two rows of 6 bytes in a buffer with a pitch of 10; the 4 padding bytes differ and must not matter
    std::vector<uint8_t> a(20, 0xEE), b(20, 0x11);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 6; ++x)
            a[y * 10 + x] = b[y * 10 + x] = static_cast<uint8_t>(1 + y * 6 + x);
    CHECK(HashRows(a.data(), 10, 6, 2) == HashRows(b.data(), 10, 6, 2));
    b[11] ^= 1; // a real byte of the second row
    CHECK(HashRows(a.data(), 10, 6, 2) != HashRows(b.data(), 10, 6, 2));

    // the same bytes read as another shape do not collide
    std::vector<uint8_t> tight(12);
    for (int i = 0; i < 12; ++i)
        tight[i] = static_cast<uint8_t>(i);
    CHECK(HashRows(tight.data(), 6, 6, 2) != HashRows(tight.data(), 4, 4, 3));
    CHECK(HashRows(tight.data(), 12, 12, 1) != HashRows(tight.data(), 6, 6, 2));

    const std::vector<uint8_t> packed = PackRows(a.data(), 10, 6, 2);
    CHECK(packed.size() == 12);
    CHECK(packed[0] == 1 && packed[5] == 6 && packed[6] == 7 && packed[11] == 12);
    CHECK(HashRows(packed.data(), 6, 6, 2) == HashRows(a.data(), 10, 6, 2));
}

void TestHex()
{
    const uint8_t bytes[4] = { 0x00, 0x0f, 0xa5, 0xff };
    CHECK(Hex(bytes, 4) == "000fa5ff");
    uint8_t back[4] = {};
    CHECK(FromHex("000fa5ff", back, 4) && back[1] == 0x0f && back[2] == 0xa5 && back[3] == 0xff);
    CHECK(!FromHex("000fa5f", back, 4));
    CHECK(!FromHex("000fa5fg", back, 4));
    CHECK(!FromHex("000FA5FF", back, 4)); // lowercase only: one spelling per value
    uint64_t v = 0;
    CHECK(FromHex64("0123456789abcdef", v) && v == 0x0123456789abcdefull);
    CHECK(Hex64(0x1ull) == "0000000000000001");
    CHECK(Hex64(0xfedcba9876543210ull) == "fedcba9876543210");
}

void TestFormats()
{
    // typeless colour formats are copied as their float or unorm member
    CHECK(CopyFormatFor(DXGI_FORMAT_R16G16B16A16_TYPELESS) == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(CopyFormatFor(DXGI_FORMAT_R32G32B32A32_TYPELESS) == DXGI_FORMAT_R32G32B32A32_FLOAT);
    CHECK(CopyFormatFor(DXGI_FORMAT_R8G8B8A8_TYPELESS) == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(CopyFormatFor(DXGI_FORMAT_R10G10B10A2_TYPELESS) == DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK(CopyFormatFor(DXGI_FORMAT_R32_TYPELESS) == DXGI_FORMAT_R32_FLOAT);
    CHECK(CopyFormatFor(DXGI_FORMAT_R16_TYPELESS) == DXGI_FORMAT_R16_UNORM);
    CHECK(CopyFormatFor(DXGI_FORMAT_R16G16_TYPELESS) == DXGI_FORMAT_R16G16_FLOAT);
    CHECK(CopyFormatFor(DXGI_FORMAT_R32G32_TYPELESS) == DXGI_FORMAT_R32G32_FLOAT);
    CHECK(CopyFormatFor(DXGI_FORMAT_R8_TYPELESS) == DXGI_FORMAT_R8_UNORM);
    // depth buffers declared as depth formats are copied through their colour twin
    CHECK(CopyFormatFor(DXGI_FORMAT_D32_FLOAT) == DXGI_FORMAT_R32_FLOAT);
    CHECK(CopyFormatFor(DXGI_FORMAT_D16_UNORM) == DXGI_FORMAT_R16_UNORM);
    // typed formats are left alone, and so are the planar depth formats (their footprint comes from the device)
    CHECK(CopyFormatFor(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(CopyFormatFor(DXGI_FORMAT_R32G8X24_TYPELESS) == DXGI_FORMAT_R32G8X24_TYPELESS);
    CHECK(CopyFormatFor(DXGI_FORMAT_R24G8_TYPELESS) == DXGI_FORMAT_R24G8_TYPELESS);
    CHECK(CopyFormatFor(DXGI_FORMAT_R24_UNORM_X8_TYPELESS) == DXGI_FORMAT_R24_UNORM_X8_TYPELESS);

    CHECK(IsPlanarDepthFormat(DXGI_FORMAT_R24G8_TYPELESS) && IsPlanarDepthFormat(DXGI_FORMAT_R32G8X24_TYPELESS));
    CHECK(IsPlanarDepthFormat(DXGI_FORMAT_D24_UNORM_S8_UINT) && IsPlanarDepthFormat(DXGI_FORMAT_D32_FLOAT_S8X24_UINT));
    CHECK(IsPlanarDepthFormat(DXGI_FORMAT_R24_UNORM_X8_TYPELESS) && IsPlanarDepthFormat(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS));
    CHECK(!IsPlanarDepthFormat(DXGI_FORMAT_D32_FLOAT) && !IsPlanarDepthFormat(DXGI_FORMAT_R32_FLOAT));
    CHECK(IsDepthResourceFormat(DXGI_FORMAT_D32_FLOAT) && IsDepthResourceFormat(DXGI_FORMAT_D16_UNORM));
    CHECK(IsDepthResourceFormat(DXGI_FORMAT_R32G8X24_TYPELESS));
    CHECK(!IsDepthResourceFormat(DXGI_FORMAT_R32_FLOAT) && !IsDepthResourceFormat(DXGI_FORMAT_R16G16B16A16_FLOAT));
}

bool Mentions(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

bool AnyProblem(const std::vector<std::string>& problems, const char* part)
{
    for (const std::string& p : problems)
        if (Mentions(p, part))
            return true;
    return false;
}

// Refused when read back, with a message that names the cause.
void CheckRefused(const Manifest& m, const char* cause, int line)
{
    const ParseResult p = ParseManifest(WriteManifest(m));
    if (p.ok || !Mentions(p.error, cause))
    {
        std::printf("FAIL line %d: expected a refusal mentioning \"%s\", got %s \"%s\"\n", line, cause, p.ok ? "acceptance" : "refusal", p.error.c_str());
        ++failures;
    }
}
#define REFUSED(m, cause) CheckRefused((m), (cause), __LINE__)

// A set with two frames of one dispatch each, nothing left out.
Manifest Complete()
{
    Manifest m = Sample();
    Dispatch d = m.dispatches[0];
    d.frame = 0;
    d.index = 0;
    Dispatch e = d;
    e.frame = 1;
    m.dispatches = { d, e };
    m.frameInfos = { { 0, 1, 0, 0 }, { 1, 1, 0, 0 } };
    return m;
}

void TestFrameInfo()
{
    CHECK(!ParseManifest(Replace(Golden(), " skipped=0 boundariesSkipped=0", "")).ok);
    CHECK(!ParseManifest(Replace(Golden(), " boundariesSkipped=0", "")).ok);
    CHECK(!ParseManifest(Replace(Golden(), " dispatches=0", "")).ok);
    CHECK(!ParseManifest(Replace(Golden(), "frameinfo frame=0 ", "frameinfo ")).ok);
    {
        Manifest m = Sample();
        m.frameInfos[1].skipped = 2;
        m.frameInfos[1].boundariesSkipped = 5;
        const ParseResult p = ParseManifest(WriteManifest(m));
        CHECK(p.ok && p.manifest.frameInfos[1].skipped == 2 && p.manifest.frameInfos[1].boundariesSkipped == 5);
    }
    // a frame described twice, or a frame the set does not have
    {
        Manifest m = Sample();
        m.frameInfos.push_back(m.frameInfos[0]);
        REFUSED(m, "duplicate frameinfo");
        m = Sample();
        m.frameInfos[0].frame = 2;
        REFUSED(m, "frameinfo for frame 2");
    }
}

void TestCompleteness()
{
    CHECK(CheckComplete(Complete()).empty());
    CHECK(ParseManifest(WriteManifest(Complete())).ok);

    {
        Manifest m = Complete();
        m.frameInfos[1].skipped = 1;
        CHECK(AnyProblem(CheckComplete(m), "frame 1: 1 dispatch(es) were left out"));
    }
    {
        Manifest m = Complete();
        m.frameInfos[0].boundariesSkipped = 3;
        CHECK(AnyProblem(CheckComplete(m), "frame 0: 3 boundary picture(s) were left out"));
    }
    {
        // the pass made two dispatches in frame 1 and only the first is in the set: the gap is named
        Manifest m = Complete();
        m.frameInfos[1].dispatches = 2;
        CHECK(AnyProblem(CheckComplete(m), "frame 1: dispatch index 1 missing (1 of 2)"));
        m.frameInfos[1].dispatches = 12;
        CHECK(AnyProblem(CheckComplete(m), "dispatch index 1, 2, 3, 4, 5, 6, 7, 8 and more missing (11 of 12)"));
    }
    {
        Manifest m = Complete();
        m.dispatches.erase(m.dispatches.begin());
        const auto problems = CheckComplete(m);
        CHECK(AnyProblem(problems, "frame 0 has no dispatches") && AnyProblem(problems, "frame 0: dispatch index 0 missing"));
    }
    {
        Manifest m = Complete();
        m.frameInfos.pop_back();
        CHECK(AnyProblem(CheckComplete(m), "frame 1: the manifest does not say how many dispatches"));
    }
    {
        Manifest m = Complete();
        m.frames = 0;
        m.frameInfos.clear();
        m.dispatches.clear();
        CHECK(AnyProblem(CheckComplete(m), "the set has no frames"));
    }
    // an empty frame is reported even when the recorder says it had no dispatches either
    {
        Manifest m = Complete();
        m.frameInfos[1].dispatches = 0;
        m.dispatches.pop_back();
        CHECK(AnyProblem(CheckComplete(m), "frame 1 has no dispatches"));
    }
}

void TestStructure()
{
    {
        Manifest m = Complete();
        m.dispatches.push_back(m.dispatches[0]);
        REFUSED(m, "duplicate dispatch");
    }
    {
        Manifest m = Complete();
        m.dispatches[1].index = 1; // the frame had one dispatch
        REFUSED(m, "the index is past the 1 dispatches");
    }
    {
        Manifest m = Complete();
        m.dispatches[1].frame = 2; // the set has two frames
        REFUSED(m, "the set has 2 frames");
    }
    {
        Manifest m = Complete();
        m.dispatches[0].bindings[1].slot = 0; // t0 twice, t1 never
        REFUSED(m, "t0 is bound twice");
    }
    {
        Manifest m = Complete();
        m.dispatches[0].bindings[7].slot = 0; // u0 twice
        REFUSED(m, "u0 is bound twice");
    }
    {
        Manifest m = Complete();
        m.dispatches[0].bindings.erase(m.dispatches[0].bindings.begin() + 3);
        REFUSED(m, "t3 is not bound");
    }
    {
        Manifest m = Complete();
        m.dispatches[0].bindings.pop_back();
        REFUSED(m, "u1 is not bound");
    }
}

void TestFilesStayInTheSet()
{
    CHECK(IsConfinedPath("images/img_0000.raw") && IsConfinedPath("a.raw") && IsConfinedPath("a/b/c.raw"));
    CHECK(IsConfinedPath("images\\img_0000.raw"));
    CHECK(!IsConfinedPath("") && !IsConfinedPath("..") && !IsConfinedPath("../x.raw") && !IsConfinedPath("images/../../x.raw"));
    CHECK(!IsConfinedPath("images\\..\\x.raw") && !IsConfinedPath("a/./b.raw") && !IsConfinedPath("a/.."));
    CHECK(!IsConfinedPath("/abs.raw") && !IsConfinedPath("\\abs.raw") && !IsConfinedPath("C:\\x.raw") && !IsConfinedPath("C:x.raw"));
    for (const char* bad : { "../evil.raw", "images/../../evil.raw", "/etc/evil.raw", "C:/evil.raw", "\\\\server\\share\\evil.raw" })
    {
        Manifest m = Complete();
        m.images[0].file = bad;
        REFUSED(m, "is not inside the set folder");
    }
}

void TestImagesMatchTheirResources()
{
    {
        Manifest m = Complete();
        m.images[0].format = DXGI_FORMAT_R16G16B16A16_UNORM; // same byte size, other meaning
        REFUSED(m, "has format");
    }
    {
        Manifest m = Complete();
        m.images[1].format = DXGI_FORMAT_R16G16B16A16_UNORM; // the image a resource writes
        REFUSED(m, "has format");
    }
    {
        Manifest m = Complete();
        m.images[0].width = 2; // 2 x 2 x 8 bytes is not the 4 x 2 resource, whatever the row bytes say
        REFUSED(m, "the resource is 4x2");
    }
    {
        Manifest m = Complete();
        m.dispatches[0].resources[0].copyFormat = DXGI_FORMAT_R16G16B16A16_UNORM; // not the copy format of its resource format
        REFUSED(m, "is not the copy format of resourceFormat");
    }
    {
        Manifest m = Complete();
        m.boundaries[0].resourceFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
        REFUSED(m, "boundary game_frame");
    }
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the output so far
    TestWriterIsPinned();
    TestRoundTrip();
    TestRefusals();
    TestConsistencyChecks();
    TestTolerantReading();
    TestHashingAndRows();
    TestHex();
    TestFormats();
    TestFrameInfo();
    TestCompleteness();
    TestStructure();
    TestFilesStayInTheSet();
    TestImagesMatchTheirResources();
    std::printf(failures == 0 ? "nr_replay_format: all checks passed\n" : "nr_replay_format: %d check(s) failed\n", failures);
    return failures;
}
