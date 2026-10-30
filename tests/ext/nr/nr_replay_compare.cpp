// Comparing a replayed image with a blessed one: bit-exact comparison, the first differing pixel, PSNR, the difference
// picture, and the bookkeeping of what was blessed (request parsing, adapter check, provenance file).
// Pure code, nothing here touches Direct3D.
#include "ext/nr/diag/NrReplayCompare.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace ext::nr::replay;
namespace fs = std::filesystem;

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
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

std::vector<uint8_t> Rgba8(uint32_t w, uint32_t h, uint8_t fill)
{
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, fill);
}

void TestHalf()
{
    CHECK(HalfToFloat(0x0000) == 0.0f);
    CHECK(HalfToFloat(0x3C00) == 1.0f);
    CHECK(HalfToFloat(0x3800) == 0.5f);
    CHECK(HalfToFloat(0xC000) == -2.0f);
    CHECK(HalfToFloat(0x7BFF) == 65504.0f);
    CHECK(std::fabs(HalfToFloat(0x0001) - 5.9604645e-8f) < 1e-12f);
    CHECK(std::isinf(HalfToFloat(0x7C00)) && HalfToFloat(0x7C00) > 0);
    CHECK(std::isinf(HalfToFloat(0xFC00)) && HalfToFloat(0xFC00) < 0);
    CHECK(std::isnan(HalfToFloat(0x7E00)));
}

void TestIdentical()
{
    const auto a = Rgba8(4, 3, 7);
    const Diff d = CompareImages(a, a, 4, 3, 16, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(d.sameShape && d.identical && d.differingPixels == 0);
    CHECK(d.psnrValid && std::isinf(d.psnr) && d.psnr > 0 && d.mse == 0.0 && d.maxAbs == 0.0);
}

void TestFirstDifferingPixel()
{
    // 8x4 RGBA8 in a buffer whose rows are padded to 48 bytes; the padding holds different garbage on each side
    const uint32_t w = 8, h = 4, pitch = 48, rowBytes = 32;
    std::vector<uint8_t> a(static_cast<size_t>(pitch) * h, 0xAA), b(static_cast<size_t>(pitch) * h, 0x55);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < rowBytes; ++x)
            a[y * pitch + x] = b[y * pitch + x] = static_cast<uint8_t>(x + y);
    {
        const Diff d = CompareImages(PackRows(a.data(), pitch, rowBytes, h), PackRows(b.data(), pitch, rowBytes, h), w, h, rowBytes,
                                     DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(d.identical); // padding never takes part
    }
    // one pixel at (5, 2): first byte of that pixel
    b[2 * pitch + 5 * 4 + 1] ^= 0x10;
    {
        const Diff d = CompareImages(PackRows(a.data(), pitch, rowBytes, h), PackRows(b.data(), pitch, rowBytes, h), w, h, rowBytes,
                                     DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(!d.identical && d.differingPixels == 1 && d.firstX == 5 && d.firstY == 2);
        CHECK(Near(d.maxAbs, 16.0 / 255.0, 1e-6)); // decoded in single precision
    }
    // two differences: the first in row-major order wins, not the first in column order
    b[1 * pitch + 7 * 4] ^= 1; // (7, 1)
    b[3 * pitch + 0] ^= 1;     // (0, 3)
    {
        const Diff d = CompareImages(PackRows(a.data(), pitch, rowBytes, h), PackRows(b.data(), pitch, rowBytes, h), w, h, rowBytes,
                                     DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(d.differingPixels == 3 && d.firstX == 7 && d.firstY == 1);
    }
    // two bytes of one pixel differ: one pixel
    {
        auto x = PackRows(a.data(), pitch, rowBytes, h), y = x;
        y[4] ^= 1;
        y[6] ^= 1;
        const Diff d = CompareImages(x, y, w, h, rowBytes, DXGI_FORMAT_R8G8B8A8_UNORM);
        CHECK(d.differingPixels == 1 && d.firstX == 1 && d.firstY == 0);
    }
}

void TestShapeAndUnknownFormat()
{
    const auto a = Rgba8(4, 4, 1);
    CHECK(!CompareImages(a, Rgba8(4, 3, 1), 4, 4, 16, DXGI_FORMAT_R8G8B8A8_UNORM).sameShape);
    CHECK(!CompareImages(a, a, 0, 4, 16, DXGI_FORMAT_R8G8B8A8_UNORM).sameShape);
    CHECK(!CompareImages(a, a, 4, 4, 0, DXGI_FORMAT_R8G8B8A8_UNORM).sameShape);

    // a format that cannot be decoded still compares bit for bit, with the pixel size taken from the row
    auto b = a;
    b[4 * 4 * 2 + 4 * 3 + 2] ^= 1; // row 2, pixel 3
    const Diff d = CompareImages(a, b, 4, 4, 16, DXGI_FORMAT_R32_UINT);
    CHECK(d.sameShape && !d.identical && !d.psnrValid && d.firstX == 3 && d.firstY == 2 && d.differingPixels == 1);
}

void TestPsnr()
{
    // one component of one pixel of a 2x2 RGBA8 image is 51/255 = 0.2 off: mse = 0.04 / 16 = 0.0025, PSNR = 26.0206 dB
    auto a = Rgba8(2, 2, 0), b = a;
    b[5] = 51;
    Diff d = CompareImages(a, b, 2, 2, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(d.psnrValid && Near(d.mse, 0.0025, 1e-8) && Near(d.psnr, 26.0206, 1e-3) && Near(d.maxAbs, 0.2, 1e-6));

    // every component 0.5 off: mse = 0.25, PSNR = 6.0206 dB (peak 1.0)
    std::vector<uint8_t> f0(16), f1(16);
    const float zero = 0.0f, half = 0.5f;
    for (int i = 0; i < 4; ++i)
    {
        std::memcpy(&f0[i * 4], &zero, 4);
        std::memcpy(&f1[i * 4], &half, 4);
    }
    d = CompareImages(f0, f1, 2, 2, 8, DXGI_FORMAT_R32_FLOAT);
    CHECK(Near(d.mse, 0.25, 1e-12) && Near(d.psnr, 6.0206, 1e-3));

    // half floats: 1.0 against 0.5 in the red channel of one RGBA16F pixel
    std::vector<uint8_t> h0(8, 0), h1(8, 0);
    const uint16_t one = 0x3C00, half16 = 0x3800;
    std::memcpy(&h0[0], &one, 2);
    std::memcpy(&h1[0], &half16, 2);
    d = CompareImages(h0, h1, 1, 1, 8, DXGI_FORMAT_R16G16B16A16_FLOAT);
    CHECK(Near(d.mse, 0.25 / 4.0, 1e-12) && Near(d.psnr, 12.0412, 1e-3));

    // a NaN against a number is an error of one, whatever the number
    std::vector<uint8_t> n0(4), n1(4);
    const float nan = std::numeric_limits<float>::quiet_NaN(), seven = 7.0f;
    std::memcpy(n0.data(), &nan, 4);
    std::memcpy(n1.data(), &seven, 4);
    d = CompareImages(n0, n1, 1, 1, 4, DXGI_FORMAT_R32_FLOAT);
    CHECK(!d.identical && Near(d.maxAbs, 1.0, 1e-12) && Near(d.psnr, 0.0, 1e-9));

    // formulas
    CHECK(std::isinf(PsnrFromMse(0.0)));
    CHECK(Near(PsnrFromMse(0.01), 20.0, 1e-9));
    CHECK(Near(PsnrFromMse(1.0), 0.0, 1e-9));
    CHECK(Near(PsnrFromMse(0.01, 2.0), 26.0206, 1e-3));
}

void TestDiffPicture()
{
    const fs::path dir = fs::temp_directory_path() / "optiscaler-nr-replay-compare-test";
    fs::create_directories(dir);
    auto a = Rgba8(3, 2, 0), b = a;
    b[(1 * 3 + 2) * 4 + 1] = 200; // pixel (2, 1), green
    const Diff d = CompareImages(a, b, 3, 2, 12, DXGI_FORMAT_R8G8B8A8_UNORM);
    const fs::path file = dir / "diff.ppm";
    CHECK(WriteDiffPpm(file, a, b, 3, 2, 12, DXGI_FORMAT_R8G8B8A8_UNORM, d));

    std::string all;
    {
        std::ifstream in(file, std::ios::binary);
        all.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const std::string header = "P6\n3 2\n255\n";
    CHECK(all.size() == header.size() + 3 * 2 * 3);
    CHECK(all.compare(0, header.size(), header) == 0);
    if (all.size() == header.size() + 18)
    {
        const auto px = [&](int x, int y, int c) { return static_cast<uint8_t>(all[header.size() + (y * 3 + x) * 3 + c]); };
        CHECK(px(2, 1, 1) == 255 && px(2, 1, 0) == 0 && px(2, 1, 2) == 0); // the largest error is full scale, in green
        CHECK(px(0, 0, 0) == 0 && px(0, 0, 1) == 0 && px(1, 1, 1) == 0);    // everything else black
    }
    // images of different sizes cannot be drawn
    CHECK(!WriteDiffPpm(dir / "no.ppm", a, Rgba8(1, 1, 0), 3, 2, 12, DXGI_FORMAT_R8G8B8A8_UNORM, d));
    fs::remove_all(dir);
}

void TestBlessRequest()
{
    BlessSpec s = ParseBless("");
    CHECK(!s.Any() && !s.Covers("encode"));
    s = ParseBless("all");
    CHECK(s.Any() && s.all && s.Covers("encode") && s.Covers("anything"));
    s = ParseBless(" all ");
    CHECK(s.all);
    s = ParseBless("encode, resolve,,downsample");
    CHECK(s.Any() && !s.all && s.kinds.size() == 3);
    CHECK(s.Covers("encode") && s.Covers("resolve") && s.Covers("downsample") && !s.Covers("meter") && !s.Covers("enc"));
    CHECK(BlessesNow(s, "resolve") && !BlessesNow(s, "feedback"));
    s = ParseBless("meter");
    CHECK(s.Any() && !s.all && s.Covers("meter") && !s.Covers("encode"));
}

void TestAdapterCheck()
{
    WarpInfo a { "10.0.26100.1", 0x1111 }, b = a, c = a, e = a;
    b.version = "10.0.26100.2";
    c.hash = 0x2222;
    CHECK(SameWarp(a, e) && !SameWarp(a, b) && !SameWarp(a, c));
    CHECK(WarpChangedMessage(a, b) ==
          "WARP changed 10.0.26100.1 (0000000000001111) -> 10.0.26100.2 (0000000000001111): re-bless, not a shader regression");

    Provenance p;
    p.warp = a;
    CHECK(AdapterAcceptable(p, a, ParseBless("")));
    CHECK(!AdapterAcceptable(p, b, ParseBless("")));
    CHECK(!AdapterAcceptable(p, c, ParseBless("")));
    CHECK(!AdapterAcceptable(p, b, ParseBless("encode"))); // a partial bless still compares the other kinds
    CHECK(AdapterAcceptable(p, b, ParseBless("all")));
    CHECK(AdapterAcceptable(p, a, ParseBless("encode")));
}

void TestUnidentifiedAdapter()
{
    WarpInfo good { "10.0.26100.1", 0x1111 };
    CHECK(good.Valid());
    // an adapter file that could not be read is never a match, not even for an identical failure
    WarpInfo unreadable;
    unreadable.version = "unknown";
    unreadable.error = "cannot read d3d10warp.dll";
    CHECK(!unreadable.Valid());
    const WarpInfo noVersion { "", 0x1111 }, noHash { "10.0.1.0", 0 }, unknownVersion { "unknown", 0x1111 };
    CHECK(!noVersion.Valid() && !noHash.Valid() && !unknownVersion.Valid());
    WarpInfo withError = good;
    withError.error = "version unreadable";
    CHECK(!withError.Valid());
    CHECK(!SameWarp(unreadable, unreadable));
    CHECK(!SameWarp(good, unreadable) && !SameWarp(unreadable, good));
    CHECK(!SameWarp(withError, good));

    Provenance blessedOnUnreadable;
    blessedOnUnreadable.warp = unreadable;
    CHECK(!AdapterAcceptable(blessedOnUnreadable, unreadable, ParseBless("")));
    // a full re-bless may replace a blessing made on another version, but never vouches for an adapter nobody could name
    Provenance p;
    p.warp = good;
    CHECK(!AdapterAcceptable(p, unreadable, ParseBless("all")));
    CHECK(!AdapterAcceptable(p, unreadable, ParseBless("encode")));
    CHECK(!AdapterAcceptable(p, withError, ParseBless("all")));
    const WarpInfo newer { "10.0.26100.2", 0x2222 };
    CHECK(AdapterAcceptable(p, newer, ParseBless("all")));

    // only the software adapter Windows ships
    CHECK(AdapterIsWarp(0x1414, 0x8c));
    CHECK(!AdapterIsWarp(0x10de, 0x2684) && !AdapterIsWarp(0x1414, 0x8d) && !AdapterIsWarp(0x1002, 0x8c));
}

void TestRunMode()
{
    // nothing requested: a compare, whatever else is set
    CHECK(DecideRunMode("", "") == RunMode::Compare);
    CHECK(DecideRunMode("", "1") == RunMode::Compare);
    // a request without the confirmation is refused: a variable left in the environment must not re-bless
    CHECK(DecideRunMode("all", "") == RunMode::Refused);
    CHECK(DecideRunMode("encode,resolve", "") == RunMode::Refused);
    CHECK(DecideRunMode("all", "0") == RunMode::Refused);
    CHECK(DecideRunMode("all", "yes") == RunMode::Refused);
    CHECK(DecideRunMode(",", "") == RunMode::Refused); // set to something, even something empty of kinds
    CHECK(DecideRunMode("all", "1") == RunMode::Bless);
    CHECK(DecideRunMode("encode", "1") == RunMode::Bless);
    // the refusal has its own exit code, which neither a pass (0) nor a failed compare (1) nor a usage error (2) returns
    CHECK(kExitBlessRefused == 3);
}

void TestUnmatchedKindsAndBanner()
{
    Manifest a, b;
    Dispatch d;
    d.kind = "encode";
    a.dispatches = { d };
    d.kind = "spatial_pack";
    b.dispatches = { d };
    const std::vector<const Manifest*> sets { &a, &b };
    CHECK(UnmatchedBlessKinds(ParseBless("all"), sets).empty());
    CHECK(UnmatchedBlessKinds(ParseBless("encode"), sets).empty());
    // a kind that only one of the sets has is matched: the sets are looked at together
    CHECK(UnmatchedBlessKinds(ParseBless("encode,spatial_pack"), sets).empty());
    const auto missing = UnmatchedBlessKinds(ParseBless("encode,resolv,meter"), sets);
    CHECK(missing.size() == 2 && missing[0] == "meter" && missing[1] == "resolv");
    CHECK(UnmatchedBlessKinds(ParseBless("encode"), {}).size() == 1);

    CHECK(BlessBanner(ParseBless("all"), "abc123", false) == "BLESSED all at abc123");
    CHECK(BlessBanner(ParseBless("resolve,encode"), "abc123", true) == "BLESSED encode,resolve at abc123 (working tree has uncommitted changes)");
    CHECK(BlessBanner(ParseBless("all"), "", false) == "BLESSED all at unknown");
}

void TestProvenance()
{
    Provenance p;
    p.commit = "0123abcd";
    p.date = "2026-10-29";
    p.dirty = true;
    p.warp = { "10.0.26100.1", 0xdeadbeefcafef00dull };
    p.dispatches[DispatchKey(1, 2)] = { "resolve", 0x1111222233334444ull, "0123abcd", "2026-10-29", true, p.warp };
    // an older blessing of another kind keeps its own record
    p.dispatches[DispatchKey(0, 0)] = { "meter", 0xaaaaaaaaaaaaaaaaull, "99ffee00", "2026-10-20", false, { "10.0.26100.0", 0x0123 } };
    const std::string text = WriteProvenance(p);
    CHECK(text ==
          "optiscaler-nr-replay-reference 2\n"
          "commit 0123abcd\n"
          "dirty 1\n"
          "date 2026-10-29\n"
          "warp version=10.0.26100.1 hash=deadbeefcafef00d\n"
          "dispatch frame=0 index=0 kind=meter shaderHash=aaaaaaaaaaaaaaaa commit=99ffee00 dirty=0 date=2026-10-20 warpVersion=10.0.26100.0 warpHash=0000000000000123\n"
          "dispatch frame=1 index=2 kind=resolve shaderHash=1111222233334444 commit=0123abcd dirty=1 date=2026-10-29 warpVersion=10.0.26100.1 warpHash=deadbeefcafef00d\n"
          "end\n");

    Provenance q;
    std::string error;
    CHECK(ParseProvenance(text, q, error));
    CHECK(q.commit == p.commit && q.date == p.date && q.dirty && SameWarp(q.warp, p.warp) && q.dispatches.size() == 2);
    CHECK(q.dispatches[DispatchKey(1, 2)].kind == "resolve" && q.dispatches[DispatchKey(1, 2)].shaderHash == 0x1111222233334444ull);
    CHECK(q.dispatches[DispatchKey(0, 0)].commit == "99ffee00" && q.dispatches[DispatchKey(0, 0)].date == "2026-10-20" &&
          !q.dispatches[DispatchKey(0, 0)].dirty && q.dispatches[DispatchKey(0, 0)].warp.version == "10.0.26100.0" &&
          q.dispatches[DispatchKey(0, 0)].warp.hash == 0x0123);
    CHECK(q.dispatches[DispatchKey(1, 2)].dirty && q.dispatches[DispatchKey(1, 2)].commit == "0123abcd");
    CHECK(WriteProvenance(q) == text);

    CHECK(!ParseProvenance("optiscaler-nr-replay-reference 3\nend\n", q, error) && error.find("unsupported") != std::string::npos);
    CHECK(!ParseProvenance("optiscaler-nr-replay-reference 1\nend\n", q, error) && error.find("unsupported") != std::string::npos);
    CHECK(!ParseProvenance("optiscaler-nr-replay-reference 0\nend\n", q, error));
    CHECK(!ParseProvenance("", q, error));
    CHECK(!ParseProvenance("nope 1\nend\n", q, error));
    CHECK(!ParseProvenance("optiscaler-nr-replay-reference 2\ncommit x\n", q, error) && error.find("truncated") != std::string::npos);
    CHECK(!ParseProvenance("optiscaler-nr-replay-reference 2\nwarp version=1\nend\n", q, error));
    CHECK(!ParseProvenance("optiscaler-nr-replay-reference 2\ndispatch frame=0 index=0 kind=a\nend\n", q, error));
    // every record of a dispatch carries its own blessing: leaving one out is an error
    for (const char* key : { " commit=99ffee00", " dirty=0", " date=2026-10-20", " warpVersion=10.0.26100.0", " warpHash=0000000000000123" })
    {
        std::string damaged = text;
        const size_t at = damaged.find(key);
        CHECK(at != std::string::npos);
        if (at != std::string::npos)
            damaged.erase(at, std::string(key).size());
        CHECK(!ParseProvenance(damaged, q, error));
    }

    CHECK(ReferenceFileName(1, 22, 0) == "f1_d22_u0.raw");
    CHECK(DispatchKey(3, 4) == "3:4");
}

void TestGitCommand()
{
    // a path with spaces is quoted, so the repository may live in one
    CHECK(GitCommand("C:\\a b\\c", "rev-parse HEAD") == "git -C \"C:\\a b\\c\" rev-parse HEAD");
    // a trailing separator would escape the closing quote when the child parses its command line: it is dropped
    CHECK(GitCommand("C:\\a b\\", "rev-parse HEAD") == "git -C \"C:\\a b\" rev-parse HEAD");
    CHECK(GitCommand("C:\\a b/", "status --porcelain") == "git -C \"C:\\a b\" status --porcelain");
    // a path that cannot survive the quoting gives no command: the caller then records an unknown commit and a dirty tree
    CHECK(GitCommand("", "rev-parse HEAD").empty());
    CHECK(GitCommand("C:\\a\"b", "rev-parse HEAD").empty());
    CHECK(GitCommand("C:\\a\nb", "rev-parse HEAD").empty());
    CHECK(GitCommand("C:\\100%\\c", "rev-parse HEAD").empty()); // cmd.exe would expand the percent signs
}

void TestCommitOverride()
{
    CHECK(CommitOverrideProblem("").empty()); // not set: the commit the repository is on is recorded
    CHECK(CommitOverrideProblem("abc123").empty());
    // the per-dispatch records are space-separated key=value pairs: a commit holding whitespace would be cut short there
    const std::string problem = CommitOverrideProblem("my commit");
    CHECK(!problem.empty() && problem.find("OPTISCALER_NR_REPLAY_COMMIT") != std::string::npos &&
          problem.find("one word") != std::string::npos);
    CHECK(!CommitOverrideProblem("my\tcommit").empty());
}

void TestBlessedSetOnDisk()
{
    const fs::path dir = fs::temp_directory_path() / "optiscaler-nr-replay-blessed-test";
    fs::remove_all(dir);
    CHECK(!HasBlessedSet(dir / "reference"));
    fs::create_directories(dir / "reference");
    CHECK(!HasBlessedSet(dir / "reference")); // a folder without a provenance file is not a blessed set
    std::ofstream(dir / "reference" / "provenance.txt") << "x";
    CHECK(HasBlessedSet(dir / "reference"));
    fs::remove_all(dir);
}

void TestRecordingGate()
{
    CHECK(ReplayRecordingWanted(true, true, 0, 2));
    CHECK(ReplayRecordingWanted(true, true, 1, 2));
    CHECK(!ReplayRecordingWanted(true, true, 2, 2));
    CHECK(!ReplayRecordingWanted(false, true, 0, 2)); // key off: never
    CHECK(!ReplayRecordingWanted(true, false, 0, 2)); // not asked for: never
    CHECK(!ReplayRecordingWanted(true, true, 0, 0));
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the output so far
#define RUN(f) (std::printf("  %s\n", #f), f())
    try
    {
        RUN(TestHalf);
        RUN(TestIdentical);
        RUN(TestFirstDifferingPixel);
        RUN(TestShapeAndUnknownFormat);
        RUN(TestPsnr);
        RUN(TestDiffPicture);
        RUN(TestBlessRequest);
        RUN(TestAdapterCheck);
        RUN(TestUnidentifiedAdapter);
        RUN(TestRunMode);
        RUN(TestUnmatchedKindsAndBanner);
        RUN(TestProvenance);
        RUN(TestGitCommand);
        RUN(TestCommitOverride);
        RUN(TestBlessedSetOnDisk);
        RUN(TestRecordingGate);
    }
    catch (const std::exception& e)
    {
        std::printf("exception: %s\n", e.what());
        return 1;
    }
    std::printf(failures == 0 ? "nr_replay_compare: all checks passed\n" : "nr_replay_compare: %d check(s) failed\n", failures);
    return failures;
}
