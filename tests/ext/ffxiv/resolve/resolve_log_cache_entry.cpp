// Logging, ResolveOnce, the resolved-build aggregate, the entry generator and the known-build cross-check.
//   * one fixed-format log line per seam (+ one total line) through an injectable sink; the product sink uses the LOG_* macros
//   * ResolveCache / ResolveOnce: resolve once, cache the result (also a failure), never scan again
//   * ResolvedBuild: every Rva / Guard field starts at 0 (never the C table), fixed offsets and constants keep their defaults
//   * FormatEntry (key=value text), the table dump for the golden test, unbound-field listing
//   * CrossCheck: on disagreement the table wins and the seam says so
// The LOG_* macros are defined here BEFORE the resolver headers so that ResolveLog.h builds its product sink.
#include <algorithm>
#include <atomic>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

inline std::vector<std::pair<int, std::string>> g_macroLog;
#define LOG_INFO(...) g_macroLog.emplace_back(0, std::format(__VA_ARGS__))
#define LOG_WARN(...) g_macroLog.emplace_back(1, std::format(__VA_ARGS__))
#define LOG_ERROR(...) g_macroLog.emplace_back(2, std::format(__VA_ARGS__))

#include "resolve_test_util.h"

#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/ResolveCache.h"
#include "ext/ffxiv/resolve/ResolveLog.h"
#include "ext/ffxiv/resolve/Resolver.h"

using namespace rt;

namespace
{
const char* const kAccessor = "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3";

AnchorSpec Plain(const char* pattern)
{
    AnchorSpec a;
    a.name = "a";
    a.pattern = pattern;
    return a;
}

const SeamResult& Get(const ResolvedBuild& rb, const char* id)
{
    static const SeamResult missing;
    const SeamResult* s = rb.Find(id);
    if (s == nullptr)
    {
        std::printf("FAIL: no result for seam %s\n", id);
        ++g_failures;
        return missing;
    }
    return *s;
}

void PlantAccessor(DenseImage& img, std::uint32_t at, std::uint32_t target)
{
    const std::int32_t disp = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - (static_cast<std::int64_t>(at) + 7));
    std::vector<std::uint8_t> bytes = FromHex("488b05");
    const auto* p = reinterpret_cast<const std::uint8_t*>(&disp);
    bytes.insert(bytes.end(), p, p + 4);
    const auto tail = FromHex("8bca488b84c8c8020000c3");
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    img.Put(at, bytes);
}

// A registry that exercises every binding kind: an address (Function), a global derived from it, a struct offset and a guard.
Registry BindingRegistry()
{
    Registry r;
    AnchorSpec acc = Plain(kAccessor);
    acc.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    acc.predicates = { pred::TargetIn(RegionKind::Bss) };
    r.seams.push_back({ "t.acc", SeamKind::Function, { acc } });

    AnchorSpec glob;
    glob.name = "load";
    glob.pattern = "48 8B 05 ?? ?? ?? ?? 8B CA";
    glob.fromSeam = "t.acc";
    glob.valueFrom = ValueFrom::Target;
    glob.decode = { DecodeKind::RipDisp32, 0, 3, 7, 0 };
    r.seams.push_back({ "t.glob", SeamKind::Global, { glob } });

    AnchorSpec off;
    off.name = "disp";
    off.pattern = "48 8B 84 C8 ?? ?? ?? ??";
    off.fromSeam = "t.acc";
    off.fromOffset = 9;
    off.valueFrom = ValueFrom::Target;
    off.decode = { DecodeKind::Field, 4, 0, 0, 4 };
    r.seams.push_back({ "t.off", SeamKind::Offset, { off } });

    r.seams.push_back({ "t.resize", SeamKind::Function, { Plain("48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20") } });

    r.bindings = { { "charaView.accessorRva", "t.acc", 0 },
                   { "charaView.managerPointerRva", "t.glob", 0 },
                   { "charaView.targetsOffset", "t.off", 0 },
                   { "nativeQuality.resizeCallbacksRva", "t.resize", 0 },
                   { "nativeQuality.resizeCallbacksInstructions", "t.resize", 0 } };
    return r;
}

std::unique_ptr<DenseImage> BindingImage()
{
    auto img = MakeImage(0x6ad10bfd);
    PlantAccessor(*img, 0x2000, 0x9100);
    Plant(*img, 0x2400, "48895c24104889 6c24184889742420 574883ec20");
    return img;
}

SeamResult Manual(const char* id, SeamStatus status, std::uint32_t rva, std::initializer_list<std::uint32_t> hits, std::uint64_t micros = 0)
{
    SeamResult r;
    r.id = id;
    r.status = status;
    r.hasValue = HasValue(status);
    r.rva = rva;
    for (std::uint32_t h : hits)
    {
        AnchorResult a;
        a.candidates = h;
        r.anchors.push_back(a);
        r.hits += h;
    }
    r.micros = micros;
    return r;
}

void TestLineFormat()
{
    // one fixed format: [ffxiv-resolve] <seam> <Status> rva=0x... hits=a+b table=0x... agree|DISAGREE|n/a ms=...
    SeamResult r = Manual("chara.accessor", SeamStatus::Found, 0x26abd0, { 1 }, 412);
    CHECK(FormatSeamLine(r) == "[ffxiv-resolve] chara.accessor Found rva=0x26abd0 hits=1 table=n/a n/a ms=0.412");
    CHECK(SeamLogLevel(r) == LogLevel::Info);

    r.hasTable = true;
    r.tableValue = 0x26abd0;
    r.agreement = Agreement::Agree;
    CHECK(FormatSeamLine(r) == "[ffxiv-resolve] chara.accessor Found rva=0x26abd0 hits=1 table=0x26abd0 agree ms=0.412");
    CHECK(SeamLogLevel(r) == LogLevel::Info);

    r.tableValue = 0x26abc0;
    r.agreement = Agreement::Disagree;
    r.reason = "charaView.accessorRva: resolved 0x26abd0, table 0x26abc0 (table wins)";
    CHECK(FormatSeamLine(r) == "[ffxiv-resolve] chara.accessor Found rva=0x26abd0 hits=1 table=0x26abc0 DISAGREE ms=0.412 note=charaView.accessorRva: "
                               "resolved 0x26abd0, table 0x26abc0 (table wins)");
    CHECK(SeamLogLevel(r) == LogLevel::Warn); // a disagreement is a warning even though the seam was found

    r = Manual("q.settings", SeamStatus::FoundSingleAnchor, 0x3745d7, { 1, 0 }, 1234567);
    r.reason = "anchor b found nothing";
    CHECK(FormatSeamLine(r) == "[ffxiv-resolve] q.settings FoundSingleAnchor rva=0x3745d7 hits=1+0 table=n/a n/a ms=1234.567 note=anchor b found nothing");
    CHECK(SeamLogLevel(r) == LogLevel::Warn);

    r = Manual("q.query", SeamStatus::Ambiguous, 0, { 10 }, 5);
    r.reason = "10 candidates";
    CHECK(FormatSeamLine(r) == "[ffxiv-resolve] q.query Ambiguous rva=n/a hits=10 table=n/a n/a ms=0.005 note=10 candidates");
    r = Manual("q.none", SeamStatus::NotFound, 0, { 0 });
    CHECK(FormatSeamLine(r).find(" NotFound rva=n/a hits=0 ") != std::string::npos && SeamLogLevel(r) == LogLevel::Warn);
    r = Manual("q.conf", SeamStatus::Conflict, 0, { 1, 1 });
    CHECK(FormatSeamLine(r).find(" Conflict rva=n/a hits=1+1 ") != std::string::npos && SeamLogLevel(r) == LogLevel::Warn);
    r = Manual("q.patched", SeamStatus::EntryPatched, 0x2da5b0, { 1 });
    CHECK(FormatSeamLine(r).find(" EntryPatched rva=0x2da5b0 hits=1 ") != std::string::npos && SeamLogLevel(r) == LogLevel::Warn);
    r = Manual("q.noanchor", SeamStatus::NotFound, 0, {});
    CHECK(FormatSeamLine(r).find(" hits=0 ") != std::string::npos);
    r = Manual("q.table", SeamStatus::NotFound, 0, { 0 });
    r.hasTable = true;
    r.tableValue = 0x2da5b0;
    CHECK(FormatSeamLine(r).find("rva=n/a hits=0 table=0x2da5b0 n/a ") != std::string::npos); // the table value is shown even without a result

    ResolvedBuild rb;
    rb.timeDateStamp = 0x6ad10bfd;
    rb.sizeOfImage = 0x381f000;
    rb.knownBuild = true;
    rb.seams = { Manual("a", SeamStatus::Found, 1, { 1 }), Manual("b", SeamStatus::NotFound, 0, { 0 }), Manual("c", SeamStatus::NotFound, 0, { 0 }),
                 Manual("d", SeamStatus::Ambiguous, 0, { 2 }), Manual("e", SeamStatus::EntryPatched, 5, { 1 }) };
    rb.scanMicros = 2500;
    rb.totalMicros = 3100;
    CHECK(FormatTotalLine(rb) == "[ffxiv-resolve] total seams=5 found=1 single=0 notfound=2 ambiguous=1 conflict=0 patched=1 scan_ms=2.500 ms=3.100 "
                                 "stamp=0x6ad10bfd size=0x381f000 known=yes");
    rb.knownBuild = false;
    rb.registryErrors = { "x" };
    CHECK(FormatTotalLine(rb).find("known=no registry_errors=1") != std::string::npos);
}

void TestResolveLogging()
{
    Registry reg;
    reg.seams.push_back({ "t.ok", SeamKind::Function, { Plain("11 22 33 44") } });
    reg.seams.push_back({ "t.missing", SeamKind::Function, { Plain("55 66 77 88") } });
    reg.seams.push_back({ "t.twice", SeamKind::Function, { Plain("99 AA BB CC") } });
    auto img = MakeImage(0x42);
    Plant(*img, 0x2000, "11223344");
    Plant(*img, 0x2100, "99aabbcc");
    Plant(*img, 0x2200, "99aabbcc");

    LogCapture log;
    ResolveOptions opts;
    opts.sink = log.Sink();
    const auto rb = ResolveImage(*img, reg, opts);
    REQUIRE(log.lines.size() == 4); // one per seam, then the total
    CHECK(log.lines[0].second.rfind("[ffxiv-resolve] t.ok Found rva=0x2000 hits=1 table=n/a n/a ms=", 0) == 0 && log.lines[0].first == LogLevel::Info);
    CHECK(log.lines[1].second.rfind("[ffxiv-resolve] t.missing NotFound rva=n/a hits=0 ", 0) == 0 && log.lines[1].first == LogLevel::Warn);
    CHECK(log.lines[2].second.rfind("[ffxiv-resolve] t.twice Ambiguous rva=n/a hits=2 ", 0) == 0 && log.lines[2].first == LogLevel::Warn);
    CHECK(log.lines[3].second.rfind("[ffxiv-resolve] total seams=3 found=1 single=0 notfound=1 ambiguous=1 conflict=0 patched=0 ", 0) == 0);
    CHECK(log.lines[3].second.find("stamp=0x42 size=0xc000 known=no") != std::string::npos);
    CHECK(rb.totalMicros >= rb.scanMicros && rb.timeDateStamp == 0x42 && rb.sizeOfImage == kImageSize && !rb.knownBuild);

    // no sink: silent. A sink that throws does not lose the result.
    CHECK(ResolveImage(*img, reg).seams.size() == 3);
    ResolveOptions bad;
    bad.sink = [](LogLevel, const std::string&) { throw std::runtime_error("sink failed"); };
    const auto rb2 = ResolveImage(*img, reg, bad);
    CHECK(rb2.seams.size() == 3 && Get(rb2, "t.ok").status == SeamStatus::Found);

    // registry errors are logged (Error level) before the seams, and the total says so
    Registry broken = reg;
    broken.seams[0].anchors[0].pattern = "11 2";
    LogCapture log2;
    ResolveOptions o2;
    o2.sink = log2.Sink();
    ResolveImage(*img, broken, o2);
    REQUIRE(log2.lines.size() >= 5);
    CHECK(log2.lines[0].first == LogLevel::Error && log2.lines[0].second.rfind("[ffxiv-resolve] registry error: ", 0) == 0);
    CHECK(log2.lines.back().second.find("registry_errors=1") != std::string::npos);
}

void TestResolvedBuildFields()
{
    const Registry reg = BindingRegistry();
    auto img = BindingImage();
    const auto rb = ResolveImage(*img, reg);
    CHECK(rb.Count(SeamStatus::Found) == 4);
    const GameBuild& b = rb.build;
    CHECK(b.charaView.accessorRva == 0x2000 && b.charaView.managerPointerRva == 0x9100 && b.charaView.targetsOffset == 0x2c8);
    CHECK(b.nativeQuality.resizeCallbacksRva == 0x2400);
    const auto expectGuard = FromHex("48895c24104889 6c24184889742420 574883ec20");
    CHECK(std::memcmp(b.nativeQuality.resizeCallbacksInstructions.data(), expectGuard.data(), expectGuard.size()) == 0);
    CHECK(b.timeDateStamp == 0x6ad10bfd && b.sizeOfImage == kImageSize);

    // every Rva and Guard field no seam filled is ZERO, not the C table value (the 0x25f610 reuse trap)
    CHECK(b.nativeQuality.querySizeRva == 0 && b.nativeQuality.settingsPointerRva == 0 && b.jitterSource.cameraGetterRva == 0 && b.jitterSource.generatorRva == 0);
    CHECK(b.jitter.cameraGlobalRva == 0 && b.jitterSource.cameraGetterRva == 0 && b.jitterSource.generatorRva == 0 && b.jitterSource.deviceSizeRva == 0);
    bool guardsZero = true;
    for (std::uint8_t v : b.jitterSource.strengthInstructions)
        guardsZero = guardsZero && v == 0;
    for (std::uint8_t v : b.nativeQuality.querySizeInstructions)
        guardsZero = guardsZero && v == 0;
    CHECK(guardsZero);
    // offsets and constants keep the table defaults (not self-healing) unless a seam fills them
    const GameBuild table {};
    CHECK(b.charaView.depthOffset0 == table.charaView.depthOffset0 && b.charaView.entryCount == table.charaView.entryCount);
    CHECK(b.nativeQuality.settingsModeOffset == table.nativeQuality.settingsModeOffset && b.jitterSource.strengthFloorBits == table.jitterSource.strengthFloorBits);
    CHECK(b.charaView.pattern == table.charaView.pattern);

    // a guard read at an offset from the seam's value (guardOffset) reads there, with the length of the table array
    {
        Registry shifted = BindingRegistry();
        shifted.bindings[4].guardOffset = 4;
        const auto rbShift = ResolveImage(*img, shifted);
        std::vector<std::uint8_t> expect(20);
        CHECK(img->Read(0x2404, expect.data(), expect.size()));
        CHECK(std::memcmp(rbShift.build.nativeQuality.resizeCallbacksInstructions.data(), expect.data(), expect.size()) == 0);
        CHECK(std::memcmp(rbShift.build.nativeQuality.resizeCallbacksInstructions.data(), expectGuard.data(), expectGuard.size()) != 0);
    }

    // a seam that is not usable fills nothing
    auto none = MakeImage(1);
    const auto rb2 = ResolveImage(*none, reg);
    CHECK(rb2.build.charaView.accessorRva == 0 && rb2.build.charaView.managerPointerRva == 0 && rb2.build.nativeQuality.resizeCallbacksRva == 0);
    CHECK(rb2.build.charaView.targetsOffset == table.charaView.targetsOffset); // the Offset seam did not resolve: the default stays

    // an EntryPatched function is not filled either
    auto patched = BindingImage();
    Plant(*patched, 0x2400, "e901020304");
    Registry body;
    body.seams.push_back({ "t.resize", SeamKind::Function, { [] { AnchorSpec a = Plain("57 48 83 EC 20"); a.valueOffset = -15; return a; }() } });
    body.bindings = { { "nativeQuality.resizeCallbacksRva", "t.resize", 0 } };
    const auto rb3 = ResolveImage(*patched, body);
    CHECK(Get(rb3, "t.resize").status == SeamStatus::EntryPatched && Get(rb3, "t.resize").rva == 0x2400);
    CHECK(rb3.build.nativeQuality.resizeCallbacksRva == 0);

    // a guard whose bytes cannot be read downgrades the seam
    Registry far;
    AnchorSpec a = Plain("DE AD BE EF");
    far.seams.push_back({ "t.edge", SeamKind::Function, { a } });
    far.bindings = { { "nativeQuality.resizeCallbacksInstructions", "t.edge", 0 } };
    auto edge = MakeImage();
    Plant(*edge, kTextRva + kTextSize - 4, "deadbeef"); // the 20 guard bytes would run past the end of .text into the unmapped gap
    const auto rb4 = ResolveImage(*edge, far);
    CHECK(Get(rb4, "t.edge").status == SeamStatus::NotFound && !Get(rb4, "t.edge").reason.empty());
}

void TestEntryAndCoverage()
{
    const Registry reg = BindingRegistry();
    auto img = BindingImage();
    auto rb = ResolveImage(*img, reg);

    const std::string expected =
        "timeDateStamp=0x6ad10bfd\n"
        "sizeOfImage=0xc000\n"
        "charaView.accessorRva=0x2000\n"
        "charaView.managerPointerRva=0x9100\n"
        "charaView.targetsOffset=0x2c8\n"
        "nativeQuality.resizeCallbacksInstructions=48895c24104889" "6c24184889742420" "574883ec20\n"
        "nativeQuality.resizeCallbacksRva=0x2400\n";
    CHECK(FormatEntry(rb, reg) == expected);

    // the table dump of an equal table is the same text; any difference shows
    GameBuild table;
    table.timeDateStamp = 0x6ad10bfd;
    table.sizeOfImage = kImageSize;
    table.charaView.accessorRva = 0x2000;
    table.charaView.managerPointerRva = 0x9100;
    table.charaView.targetsOffset = 0x2c8;
    table.nativeQuality.resizeCallbacksRva = 0x2400;
    const auto guard = FromHex("48895c24104889 6c24184889742420 574883ec20");
    std::memcpy(table.nativeQuality.resizeCallbacksInstructions.data(), guard.data(), guard.size());
    CHECK(FormatTableEntry(table, reg) == expected);
    table.charaView.accessorRva = 0x2001;
    CHECK(FormatTableEntry(table, reg) != expected);
    table.charaView.accessorRva = 0x2000;
    table.nativeQuality.resizeCallbacksInstructions[19] ^= 1;
    CHECK(FormatTableEntry(table, reg) != expected);

    // an unresolved seam is listed as a comment after the values, never as a value
    auto partial = MakeImage(0x6ad10bfd);
    Plant(*partial, 0x2400, "48895c24104889 6c24184889742420 574883ec20");
    rb = ResolveImage(*partial, reg);
    const std::string text = FormatEntry(rb, reg);
    CHECK(text.find("charaView.accessorRva=") == std::string::npos);
    CHECK(text.find("# charaView.accessorRva unresolved: NotFound\n") != std::string::npos);
    CHECK(text.find("nativeQuality.resizeCallbacksRva=0x2400\n") != std::string::npos);
    CHECK(text.find("# nativeQuality.resizeCallbacksRva") == std::string::npos);
    CHECK(text.rfind('#') > text.find("nativeQuality.resizeCallbacksRva=")); // comments come last

    // what is still unbound: every Rva and Guard field of the table minus the ones this registry binds (3 Rva + 1 Guard)
    const auto unbound = UnboundFields(reg);
    std::size_t rvaGuard = 0;
    for (const FieldRow& row : kFieldTable)
        rvaGuard += (row.kind == FieldKind::Rva || row.kind == FieldKind::Guard) ? 1 : 0;
    CHECK(unbound.size() == rvaGuard - 4);
    auto has = [&](const char* key) { return std::find(unbound.begin(), unbound.end(), key) != unbound.end(); };
    CHECK(!has("charaView.accessorRva") && !has("nativeQuality.resizeCallbacksInstructions") && has("nativeQuality.querySizeRva") && has("jitterSource.strengthInstructions"));
    CHECK(!has("charaView.targetsOffset")); // offsets are never "unbound": they keep defaults
    CHECK(UnboundFields(Registry {}).size() == rvaGuard);
}

// M9: a value that was not confirmed by two anchors is marked in the entry text, a plain Found value never is
void TestEntryMarksWeakSeams()
{
    Registry r;
    // two agreeing anchors: Found; one anchor of two found nothing: single-anchor; a seam that depends on the single-anchor seam: via-weak-dependency
    r.seams.push_back({ "t.found", SeamKind::Function, { Plain("11 22 33 44"), Plain("11 22 33 44") } });
    r.seams.push_back({ "t.single", SeamKind::Function, { Plain("55 66 77 88"), Plain("77 77 77 77") } });
    AnchorSpec dep = Plain("99 AA BB CC");
    dep.predicates = { pred::After("t.single", 0x2000) };
    r.seams.push_back({ "t.weak", SeamKind::Function, { dep } });
    r.bindings = { { "charaView.accessorRva", "t.found", 0 }, { "nativeQuality.querySizeRva", "t.single", 0 }, { "nativeQuality.rendererUpdateRva", "t.weak", 0 } };
    auto img = MakeImage(0x6ad10bfd);
    Plant(*img, 0x2000, "11223344");
    Plant(*img, 0x2400, "55667788");
    Plant(*img, 0x3500, "99aabbcc");
    const auto rb = ResolveImage(*img, r);
    CHECK(Get(rb, "t.found").status == SeamStatus::Found && Get(rb, "t.single").status == SeamStatus::FoundSingleAnchor);
    CHECK(Get(rb, "t.weak").status == SeamStatus::FoundSingleAnchor && Get(rb, "t.weak").viaWeakDependency);
    const std::string text = FormatEntry(rb, r);
    CHECK(text.find("charaView.accessorRva=0x2000\n") != std::string::npos); // a plain Found value: no marker
    CHECK(text.find("nativeQuality.querySizeRva=0x2400 # single-anchor\n") != std::string::npos);
    CHECK(text.find("nativeQuality.rendererUpdateRva=0x3500 # via-weak-dependency\n") != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '#') == 2);
    // the table text of the same values has no marker: a comparison strips the trailing comments (tools/ffxiv-resolve/Verdict.h)
    GameBuild table;
    table.timeDateStamp = 0x6ad10bfd;
    table.sizeOfImage = kImageSize;
    CHECK(FormatTableEntry(table, r).find('#') == std::string::npos);
}

// L2: the first call of the cache from several threads resolves exactly once, and everyone sees the same object
void TestCacheConcurrentFirstCall()
{
    const Registry reg = BindingRegistry();
    auto img = BindingImage();
    ResolveCache cache;
    std::atomic<int> lines { 0 };
    ResolveOptions opts;
    opts.sink = [&](LogLevel, const std::string&) { ++lines; };
    std::vector<const ResolvedBuild*> seen(8, nullptr);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < seen.size(); ++t)
        threads.emplace_back([&, t] { seen[t] = &cache.Get(*img, reg, opts); });
    for (std::thread& t : threads)
        t.join();
    CHECK(cache.Resolved());
    CHECK(lines == static_cast<int>(reg.seams.size()) + 1); // one resolve: one line per seam and the total, once
    bool same = true;
    for (const ResolvedBuild* p : seen)
        same = same && p == seen[0] && p != nullptr;
    CHECK(same);
}

void TestCrossCheck()
{
    const Registry reg = BindingRegistry();
    auto img = BindingImage();
    GameBuild table;
    table.timeDateStamp = 0x6ad10bfd; // the table describes THIS image (identity gate: stamp and size)
    table.sizeOfImage = kImageSize;
    table.charaView.accessorRva = 0x2000;
    table.charaView.managerPointerRva = 0x9100;
    table.charaView.targetsOffset = 0x2c8;
    table.nativeQuality.resizeCallbacksRva = 0x2400;
    const auto guard = FromHex("48895c24104889 6c24184889742420 574883ec20");
    std::memcpy(table.nativeQuality.resizeCallbacksInstructions.data(), guard.data(), guard.size());

    // the table agrees with every seam
    LogCapture log;
    ResolveOptions opts;
    opts.knownTable = &table;
    opts.sink = log.Sink();
    auto rb = ResolveImage(*img, reg, opts);
    CHECK(rb.knownBuild);
    for (const SeamResult& s : rb.seams)
        CHECK(s.agreement == Agreement::Agree && s.hasTable);
    CHECK(Get(rb, "t.acc").tableValue == 0x2000 && Get(rb, "t.off").tableValue == 0x2c8);
    CHECK(log.lines[0].second.find("table=0x2000 agree ") != std::string::npos && log.lines[0].first == LogLevel::Info);
    CHECK(log.lines.back().second.find("known=yes") != std::string::npos);

    // one disagreeing RVA: that seam is flagged, the TABLE value wins in the build, a WARN line, the others stay agreed
    table.charaView.managerPointerRva = 0x9108;
    log.lines.clear();
    rb = ResolveImage(*img, reg, opts);
    CHECK(Get(rb, "t.glob").agreement == Agreement::Disagree && Get(rb, "t.glob").tableValue == 0x9108 && Get(rb, "t.glob").rva == 0x9100);
    CHECK(rb.build.charaView.managerPointerRva == 0x9108);                       // table wins
    CHECK(Get(rb, "t.glob").status == SeamStatus::Found);                         // the seam result itself is not rewritten
    CHECK(Get(rb, "t.glob").reason.find("charaView.managerPointerRva") != std::string::npos && Get(rb, "t.glob").reason.find("table wins") != std::string::npos);
    CHECK(Get(rb, "t.acc").agreement == Agreement::Agree && rb.build.charaView.accessorRva == 0x2000);
    CHECK(log.lines[1].second.find(" DISAGREE ") != std::string::npos && log.lines[1].first == LogLevel::Warn);

    // a disagreeing guard
    table.charaView.managerPointerRva = 0x9100;
    table.nativeQuality.resizeCallbacksInstructions[3] ^= 0xff;
    rb = ResolveImage(*img, reg, opts);
    CHECK(Get(rb, "t.resize").agreement == Agreement::Disagree);
    CHECK(rb.build.nativeQuality.resizeCallbacksInstructions == table.nativeQuality.resizeCallbacksInstructions);

    // an unresolved seam is not "checked", but its table value is reported ...
    table = GameBuild {};
    table.timeDateStamp = 0x6ad10bfd;
    table.sizeOfImage = kImageSize;
    table.charaView.accessorRva = 0x26abd0;
    auto none = MakeImage(0x6ad10bfd);
    rb = ResolveImage(*none, reg, opts);
    CHECK(Get(rb, "t.acc").agreement == Agreement::NotChecked && Get(rb, "t.acc").hasTable && Get(rb, "t.acc").tableValue == 0x26abd0);
    // ... and on a KNOWN build (the table is this image's build) the table value fills the field of a seam that did not resolve. The
    // status stays honest, the flag says where the value came from (M11).
    CHECK(rb.knownBuild && rb.build.charaView.accessorRva == 0x26abd0 && Get(rb, "t.acc").status == SeamStatus::NotFound);
    CHECK(Get(rb, "t.acc").fellBackToTable && rb.UsableOrTable("t.acc") && !rb.Usable("t.acc"));
    CHECK(Get(rb, "t.acc").reason.find("filled from the table") != std::string::npos);
    // a table value never feeds another seam: the global that depends on the unresolved accessor is still DependencyMissing
    CHECK(Get(rb, "t.glob").anchors[0].outcome == AnchorOutcome::DependencyMissing && Get(rb, "t.glob").fellBackToTable);

    // no known table: nothing is compared
    rb = ResolveImage(*img, reg);
    CHECK(!rb.knownBuild && Get(rb, "t.acc").agreement == Agreement::NotChecked && !Get(rb, "t.acc").hasTable);

    // the free function works on a finished build as well (and applies the identity gate itself)
    rb = ResolveImage(*img, reg);
    table = GameBuild {};
    table.charaView.accessorRva = 0x1234;
    CHECK(!CrossCheck(rb, table, reg) && Get(rb, "t.acc").agreement == Agreement::NotChecked && rb.build.charaView.accessorRva == 0x2000); // another build
    table.timeDateStamp = rb.timeDateStamp;
    table.sizeOfImage = rb.sizeOfImage;
    CHECK(CrossCheck(rb, table, reg) && rb.knownBuild);
    CHECK(Get(rb, "t.acc").agreement == Agreement::Disagree && rb.build.charaView.accessorRva == 0x1234);
}

// H1: the known table counts only for the build it describes. Stamp AND size must both equal the image's.
void TestKnownIdentityGate()
{
    const Registry reg = BindingRegistry();
    auto img = BindingImage(); // stamp 0x6ad10bfd, size kImageSize
    auto make = [&](std::uint32_t stamp, std::uint32_t size)
    {
        GameBuild t;
        t.timeDateStamp = stamp;
        t.sizeOfImage = size;
        t.charaView.accessorRva = 0x26abd0; // a value that differs from what the image resolves (0x2000)
        t.charaView.managerPointerRva = 0x290e490;
        return t;
    };
    const GameBuild exact = make(0x6ad10bfd, kImageSize);
    const GameBuild wrongStamp = make(0x6ac3dc96, kImageSize); // the real other build's stamp, same size
    const GameBuild wrongSize = make(0x6ad10bfd, kImageSize + 0x1000);

    auto run = [&](const DenseImage& image, const GameBuild& table, LogCapture* log = nullptr)
    {
        ResolveOptions o;
        o.knownTable = &table;
        if (log != nullptr)
            o.sink = log->Sink();
        return ResolveImage(image, reg, o);
    };

    // the exact table: used (table wins on the disagreeing field)
    auto rb = run(*img, exact);
    CHECK(rb.knownBuild && Get(rb, "t.acc").agreement == Agreement::Disagree && rb.build.charaView.accessorRva == 0x26abd0);

    // either identity half missing: the table is ignored entirely, one WARN, no comparison, no table value anywhere
    const GameBuild* wrong[] = { &wrongStamp, &wrongSize };
    for (const GameBuild* table : wrong)
    {
        LogCapture log;
        rb = run(*img, *table, &log);
        CHECK(!rb.knownBuild);
        CHECK(rb.build.charaView.accessorRva == 0x2000 && rb.build.charaView.managerPointerRva == 0x9100); // the resolved values, not the table's
        for (const SeamResult& s : rb.seams)
            CHECK(s.agreement == Agreement::NotChecked && !s.hasTable && !s.fellBackToTable);
        std::size_t ignoredLines = 0;
        for (const auto& l : log.lines)
        {
            if (l.second.find("known table ignored") != std::string::npos)
            {
                ++ignoredLines;
                CHECK(l.first == LogLevel::Warn);
            }
        }
        CHECK(ignoredLines == 1 && log.lines.back().second.find("known=no") != std::string::npos);
    }

    // nothing resolved + another build's table: no fallback either (unknown build: the table is never a source of values)
    auto none = MakeImage(0x6ad10bfd);
    rb = run(*none, wrongStamp);
    CHECK(!rb.knownBuild && rb.build.charaView.accessorRva == 0 && rb.build.charaView.managerPointerRva == 0 && !rb.UsableOrTable("t.acc"));
    rb = run(*none, wrongSize);
    CHECK(!rb.knownBuild && rb.build.charaView.accessorRva == 0 && !rb.UsableOrTable("t.acc"));
    rb = ResolveImage(*none, reg);
    CHECK(!rb.knownBuild && rb.build.charaView.accessorRva == 0 && !Get(rb, "t.acc").fellBackToTable && !rb.UsableOrTable("t.acc"));
    rb = run(*none, exact);
    CHECK(rb.knownBuild && rb.build.charaView.accessorRva == 0x26abd0 && rb.UsableOrTable("t.acc"));

    // the free function applies the same gate and leaves the build untouched
    rb = ResolveImage(*img, reg);
    const ResolvedBuild before = rb;
    CHECK(!CrossCheck(rb, wrongStamp, reg) && !CrossCheck(rb, wrongSize, reg));
    CHECK(!rb.knownBuild && rb.build.charaView.accessorRva == before.build.charaView.accessorRva && Get(rb, "t.acc").agreement == Agreement::NotChecked);
}

// M11 in detail: which statuses fall back, which fields, and the flags
void TestTableFallback()
{
    const Registry reg = BindingRegistry();
    GameBuild table;
    table.timeDateStamp = 0x6ad10bfd;
    table.sizeOfImage = kImageSize;
    table.charaView.accessorRva = 0x26abd0;
    table.charaView.managerPointerRva = 0x290e490;
    table.charaView.targetsOffset = 0x555;
    table.nativeQuality.resizeCallbacksRva = 0x2369a0;
    const auto guard = FromHex("48895c24104889 6c24184889742420 574883ec20");
    std::memcpy(table.nativeQuality.resizeCallbacksInstructions.data(), guard.data(), guard.size());
    ResolveOptions opts;
    opts.knownTable = &table;

    // NotFound: everything bound to the seam (an Rva, a Guard, an Offset) comes from the table
    auto none = MakeImage(0x6ad10bfd);
    auto rb = ResolveImage(*none, reg, opts);
    CHECK(rb.build.nativeQuality.resizeCallbacksRva == 0x2369a0);
    CHECK(rb.build.nativeQuality.resizeCallbacksInstructions == table.nativeQuality.resizeCallbacksInstructions);
    CHECK(rb.build.charaView.targetsOffset == 0x555 && rb.build.charaView.managerPointerRva == 0x290e490);
    for (const SeamResult& s : rb.seams)
        CHECK(s.status == SeamStatus::NotFound && s.fellBackToTable && !IsUsable(s.status) && rb.UsableOrTable(s.id));
    // the entry text keeps saying "unresolved": it never prints a table value as if it were resolved
    const std::string entry = FormatEntry(rb, reg);
    CHECK(entry.find("charaView.accessorRva=") == std::string::npos && entry.find("# charaView.accessorRva unresolved: NotFound") != std::string::npos);
    // the log line is a WARN and names the fallback
    LogCapture log;
    opts.sink = log.Sink();
    ResolveImage(*none, reg, opts);
    REQUIRE(log.lines.size() >= 2);
    CHECK(log.lines[0].first == LogLevel::Warn && log.lines[0].second.find("filled from the table") != std::string::npos);
    opts.sink = nullptr;

    // Ambiguous, Conflict and EntryPatched fall back too, and keep their own status
    {
        Registry r;
        r.seams.push_back({ "t.amb", SeamKind::Function, { Plain("DE AD BE EF") } });
        r.seams.push_back({ "t.conf", SeamKind::Function, { Plain("11 22 33 44"), Plain("55 66 77 88") } });
        r.seams.push_back({ "t.patched", SeamKind::Function, { [] { AnchorSpec a = Plain("CA FE BA BE 11 22 33 44"); a.valueOffset = -0x20; return a; }() } });
        r.bindings = { { "nativeQuality.querySizeRva", "t.amb", 0 }, { "nativeQuality.rendererUpdateRva", "t.conf", 0 }, { "nativeQuality.resizeCallbacksRva", "t.patched", 0 } };
        auto img = MakeImage(0x6ad10bfd);
        Plant(*img, 0x2000, "deadbeef");
        Plant(*img, 0x2100, "deadbeef");
        Plant(*img, 0x2400, "11223344");
        Plant(*img, 0x2800, "55667788");
        Plant(*img, 0x3120, "cafebabe11223344");
        Plant(*img, 0x3100, "e901020304"); // a foreign hook over the entry
        GameBuild t2;
        t2.timeDateStamp = 0x6ad10bfd;
        t2.sizeOfImage = kImageSize;
        t2.nativeQuality.querySizeRva = 0x352350;
        t2.nativeQuality.rendererUpdateRva = 0x2da5b0;
        t2.nativeQuality.resizeCallbacksRva = 0x2369a0;
        ResolveOptions o2;
        o2.knownTable = &t2;
        rb = ResolveImage(*img, r, o2);
        CHECK(Get(rb, "t.amb").status == SeamStatus::Ambiguous && Get(rb, "t.conf").status == SeamStatus::Conflict &&
              Get(rb, "t.patched").status == SeamStatus::EntryPatched);
        for (const SeamResult& s : rb.seams)
        {
            if (s.status == SeamStatus::EntryPatched) // fail closed: a foreign hook over the entry is never filled from the table
                CHECK(!s.fellBackToTable && !rb.UsableOrTable(s.id) && !rb.Usable(s.id));
            else
                CHECK(s.fellBackToTable && rb.UsableOrTable(s.id) && !rb.Usable(s.id));
        }
        CHECK(rb.build.nativeQuality.querySizeRva == 0x352350 && rb.build.nativeQuality.rendererUpdateRva == 0x2da5b0 &&
              rb.build.nativeQuality.resizeCallbacksRva == 0);
        // the same registry on an UNKNOWN build (no table): nothing is ever filled from the table
        rb = ResolveImage(*img, r);
        for (const SeamResult& s : rb.seams)
            CHECK(!s.fellBackToTable && !rb.UsableOrTable(s.id));
        CHECK(rb.build.nativeQuality.querySizeRva == 0 && rb.build.nativeQuality.rendererUpdateRva == 0 && rb.build.nativeQuality.resizeCallbacksRva == 0);
    }

    // failClosed: a strict SHAPE seam that is not usable on a known build is NOT filled from the table (the table's guard array is only a
    // short prefix of the strict pattern); a plain seam of the same registry still falls back
    {
        Registry r;
        r.seams.push_back({ "t.shape", SeamKind::CodeSite, { Plain("DE AD BE EF 01 02") }, true });
        r.seams.push_back({ "t.plain", SeamKind::CodeSite, { Plain("DE AD BE EF 03 04") } });
        r.bindings = { { "nativeQuality.settingsInstructionsRva", "t.shape", 0 },
                       { "nativeQuality.settingsInstructions", "t.shape", 0 },
                       { "nativeQuality.querySizeRva", "t.plain", 0 } };
        GameBuild t3;
        t3.timeDateStamp = 0x6ad10bfd;
        t3.sizeOfImage = kImageSize;
        ResolveOptions o3;
        o3.knownTable = &t3;
        auto img3 = MakeImage(0x6ad10bfd);
        const ResolvedBuild fc = ResolveImage(*img3, r, o3);
        CHECK(Get(fc, "t.shape").status == SeamStatus::NotFound && !Get(fc, "t.shape").fellBackToTable && !fc.UsableOrTable("t.shape"));
        const std::array<std::uint8_t, 37> zeroGuard {};
        CHECK(fc.build.nativeQuality.settingsInstructionsRva == 0 && fc.build.nativeQuality.settingsInstructions == zeroGuard);
        CHECK(Get(fc, "t.plain").fellBackToTable && fc.UsableOrTable("t.plain") && fc.build.nativeQuality.querySizeRva == t3.nativeQuality.querySizeRva);
        // the same registry with failClosed off falls back (the flag is what changes the outcome)
        Registry open = r;
        open.seams[0].failClosed = false;
        const ResolvedBuild fo = ResolveImage(*img3, open, o3);
        CHECK(Get(fo, "t.shape").fellBackToTable && fo.build.nativeQuality.settingsInstructionsRva == t3.nativeQuality.settingsInstructionsRva);
    }

    // a usable seam is never "fellBack": the resolved value stands, UsableOrTable is Usable
    auto img = BindingImage();
    rb = ResolveImage(*img, reg, opts);
    CHECK(!Get(rb, "t.acc").fellBackToTable && rb.Usable("t.acc") && rb.UsableOrTable("t.acc") && !rb.UsableOrTable("t.nothing"));
}

// M12: Binding::valueOffset adds a delta to the seam value ("producerReturnRva = producerCallRva + 5")
void TestBindingValueOffset()
{
    Registry r;
    r.seams.push_back({ "t.call", SeamKind::CodeSite, { Plain("E8 ?? ?? ?? ?? 90 90 90 90") } });
    r.bindings = { { "jitterSource.producerCallRva", "t.call", 0, 0 }, { "jitterSource.producerReturnRva", "t.call", 0, 5 } };
    CHECK(ValidateRegistry(r).empty());
    auto img = MakeImage(0x6ad10bfd);
    Plant(*img, 0x2000, "e800000000");
    Plant(*img, 0x2005, "90909090");
    auto rb = ResolveImage(*img, r);
    CHECK(Get(rb, "t.call").status == SeamStatus::Found && rb.build.jitterSource.producerCallRva == 0x2000 && rb.build.jitterSource.producerReturnRva == 0x2005);
    CHECK(FormatEntry(rb, r).find("jitterSource.producerReturnRva=0x2005\n") != std::string::npos);

    // a negative offset
    Registry n = r;
    n.bindings = { { "jitterSource.producerReturnRva", "t.call", 0, -0x10 } };
    rb = ResolveImage(*img, n);
    CHECK(rb.build.jitterSource.producerReturnRva == 0x1ff0);

    // the cross-check compares the FIELD (seam + offset) with the table; the logged table value is the table's view of the SEAM value
    GameBuild table;
    table.timeDateStamp = 0x6ad10bfd;
    table.sizeOfImage = kImageSize;
    table.jitterSource.producerCallRva = 0x2000;
    table.jitterSource.producerReturnRva = 0x2005;
    ResolveOptions o;
    o.knownTable = &table;
    rb = ResolveImage(*img, r, o);
    CHECK(Get(rb, "t.call").agreement == Agreement::Agree && Get(rb, "t.call").tableValue == 0x2000 && rb.build.jitterSource.producerReturnRva == 0x2005);
    table.jitterSource.producerReturnRva = 0x2006; // the table disagrees on the offset field only
    rb = ResolveImage(*img, r, o);
    CHECK(Get(rb, "t.call").agreement == Agreement::Disagree && rb.build.jitterSource.producerReturnRva == 0x2006);
    // only the offset-5 binding present: the seam shows table=0x2000 (0x2005 - 5)
    Registry only;
    only.seams = r.seams;
    only.bindings = { { "jitterSource.producerReturnRva", "t.call", 0, 5 } };
    table.jitterSource.producerReturnRva = 0x2005;
    rb = ResolveImage(*img, only, o);
    CHECK(Get(rb, "t.call").tableValue == 0x2000 && Get(rb, "t.call").agreement == Agreement::Agree);

    // validation: valueOffset is for Rva and Offset fields only, never for a Guard field
    Registry bad = r;
    bad.bindings = { { "nativeQuality.resizeCallbacksInstructions", "t.call", 0, 5 } };
    CHECK(!ValidateRegistry(bad).empty());
    // a value that leaves the image downgrades the seam (it cannot fill its field), it never writes a wild RVA
    Registry far = r;
    far.bindings = { { "jitterSource.producerReturnRva", "t.call", 0, 0x7ffffff0 } };
    rb = ResolveImage(*img, far);
    CHECK(Get(rb, "t.call").status == SeamStatus::NotFound && rb.build.jitterSource.producerReturnRva == 0 && !Get(rb, "t.call").reason.empty());
    Registry neg = r;
    neg.bindings = { { "jitterSource.producerReturnRva", "t.call", 0, -0x3000 } };
    rb = ResolveImage(*img, neg);
    CHECK(Get(rb, "t.call").status == SeamStatus::NotFound && rb.build.jitterSource.producerReturnRva == 0);
}

void TestCache()
{
    const Registry reg = BindingRegistry();
    auto img = BindingImage();
    int reads = 0;
    img->readObserver = [&](std::uint32_t, std::size_t) { ++reads; };

    ResolveCache cache;
    CHECK(!cache.Resolved());
    const ResolvedBuild& first = cache.Get(*img, reg);
    CHECK(cache.Resolved() && reads > 0 && Get(first, "t.acc").status == SeamStatus::Found);
    const int afterFirst = reads;
    const ResolvedBuild& second = cache.Get(*img, reg);
    CHECK(&first == &second);        // the very same object
    CHECK(reads == afterFirst);      // nothing was read: no lazy re-scan
    // another image or registry changes nothing: the first result stays (the process image does not change)
    auto other = MakeImage();
    const ResolvedBuild& third = cache.Get(*other, Registry {});
    CHECK(&third == &first && Get(third, "t.acc").status == SeamStatus::Found);

    // a failure is cached as well and never retried
    struct Throwing : ImageView
    {
        mutable int calls = 0;
        explicit Throwing(const DenseImage& i) { _info = i.Info(); }
        bool Read(std::uint32_t, void*, std::size_t) const override
        {
            ++calls;
            throw std::runtime_error("boom");
        }
    } bad(*img);
    ResolveCache failing;
    LogCapture failLog;
    ResolveOptions failOptions;
    failOptions.sink = failLog.Sink();
    const ResolvedBuild& f1 = failing.Get(bad, reg, failOptions);
    CHECK(f1.seams.size() == reg.seams.size() && f1.Count(SeamStatus::NotFound) == reg.seams.size() && !f1.registryErrors.empty());
    // H2: the failure value has every Rva / Guard field ZERO (never the C table), like a ResolveImage result, and says so in the log
    CHECK(f1.build.charaView.accessorRva == 0 && f1.build.nativeQuality.querySizeRva == 0 && f1.build.charaView.managerPointerRva == 0);
    CHECK(f1.build.jitterSource.strengthRva == 0 && f1.build.jitter.cameraGlobalRva == 0);
    {
        bool allZero = true;
        for (const FieldRow& row : kFieldTable)
        {
            if (row.kind != FieldKind::Rva && row.kind != FieldKind::Guard)
                continue;
            for (std::uint32_t k = 0; k < row.size; ++k)
                allZero = allZero && FieldBytes(f1.build, row)[k] == 0;
        }
        CHECK(allZero);
    }
    CHECK(f1.timeDateStamp == 0x6ad10bfd && f1.sizeOfImage == kImageSize && f1.build.timeDateStamp == 0x6ad10bfd && f1.build.sizeOfImage == kImageSize);
    CHECK(!f1.knownBuild);
    REQUIRE(failLog.lines.size() == 1);
    CHECK(failLog.lines[0].first == LogLevel::Error && failLog.lines[0].second.find("[ffxiv-resolve]") == 0 && failLog.lines[0].second.find("boom") != std::string::npos);
    // a default-constructed ResolvedBuild never carries the C table either
    const ResolvedBuild blank;
    CHECK(blank.build.charaView.accessorRva == 0 && blank.build.timeDateStamp == 0 && blank.build.sizeOfImage == 0 && blank.build.nativeQuality.querySizeRva == 0);
    const int calls = bad.calls;
    CHECK(calls > 0);
    const ResolvedBuild& f2 = failing.Get(bad, reg);
    CHECK(&f1 == &f2 && bad.calls == calls);

    // ResolveOnce: one process-wide cache; the second call (any arguments) returns the first result
    const ResolvedBuild& once1 = ResolveOnce(*img, reg);
    const ResolvedBuild& once2 = ResolveOnce(*other, Registry {});
    CHECK(&once1 == &once2 && Get(once1, "t.acc").status == SeamStatus::Found);
}

void TestDefaultSink()
{
    // the product sink goes through the LOG_* macros (the product's macros prefix the function name; the line itself is fixed)
    g_macroLog.clear();
    const LogSink sink = MakeDefaultSink();
    sink(LogLevel::Info, "[ffxiv-resolve] a");
    sink(LogLevel::Warn, "[ffxiv-resolve] b");
    sink(LogLevel::Error, "[ffxiv-resolve] c {braces}");
    REQUIRE(g_macroLog.size() == 3);
    CHECK(g_macroLog[0] == std::make_pair(0, std::string("[ffxiv-resolve] a")));
    CHECK(g_macroLog[1] == std::make_pair(1, std::string("[ffxiv-resolve] b")));
    CHECK(g_macroLog[2] == std::make_pair(2, std::string("[ffxiv-resolve] c {braces}"))); // the line is an argument, never a format string
}
} // namespace

int main()
{
    TestLineFormat();
    TestResolveLogging();
    TestResolvedBuildFields();
    TestEntryAndCoverage();
    TestEntryMarksWeakSeams();
    TestCrossCheck();
    TestKnownIdentityGate();
    TestTableFallback();
    TestBindingValueOffset();
    TestCache();
    TestCacheConcurrentFirstCall();
    TestDefaultSink();
    return TEST_RESULT("resolve_log_cache_entry");
}
