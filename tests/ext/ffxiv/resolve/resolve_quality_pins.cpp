// The quality group's registry, pinned and resolved over committed bytes (no executable needed).
//   * every anchor of every seam is pinned as text: pattern, decode, valueFrom, guard site, predicates, margin (CHANGES 21 of the frozen types:
//     over-wildcarding passes every other gate, so the literal text is the only thing that holds the fixed bytes in place); the pinned pattern
//     texts live in resolve_quality_data.h as copies independent of AnchorsQuality.h and of GameBuild.h;
//   * the bindings, one per field, as a set;
//   * the recorded bytes of build C planted into a dense image with the layout of build C and its .pdata: every seam is plain Found at the table
//     value, every anchor is Unique, the entry text equals the table text, the guard fields hold the bytes of the table, nothing is a WARN;
//   * a resolve WITHOUT the table gives the same answer (the pure pattern-search result).
#include "resolve_quality_util.h"

#include <map>
#include <set>

using namespace qt;

namespace
{
constexpr ValueFrom M = ValueFrom::Match, E = ValueFrom::EnclosingFunction, T = ValueFrom::Target;
constexpr DecodeKind kNone = DecodeKind::None, kRip = DecodeKind::RipDisp32, kRel = DecodeKind::Rel32, kFld = DecodeKind::Field;

struct ExpectSeam
{
    const char* id;
    SeamKind kind;
    std::vector<ExpectAnchor> anchors;
    std::uint32_t rva; // the value on build C
    bool failClosed = false; // SeamSpec::failClosed: true on the three SHAPE seams only (never table-filled on a known build)
};

// name, pattern, from, fromOffset, valueFrom, valueOffset, decode kind, at, dispAt, length, width, entry, minMargin, predicates
const std::vector<ExpectSeam>& Expected()
{
    static const std::vector<ExpectSeam> e = {
        { "chara.accessor", SeamKind::Function,
          { { "accessorLoad", "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3", "", 0, M, 0, kRip, 0, 3, 7, 0, false, 3, "TargetIn:Bss" } }, 0x26abd0 },
        { "chara.managerPointer", SeamKind::Global,
          { { "accessorDisp", "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 C8 02 00 00 C3", "chara.accessor", 0, T, 0, kRip, 0, 3, 7, 0, false, 3,
              "TargetIn:Bss" } },
          0x290e490 },
        { "chara.targetsOffset", SeamKind::Offset,
          { { "accessorTargets", kPinCharaTargets, "chara.accessor", 0, T, 0, kFld, 13, 0, 0, 4, false, 3, "ValueIn:256-4096" } }, 0x2c8 },

        { "quality.settingsInstructions", SeamKind::CodeSite, { { "settingsBlock", kPinSettingsBlock, "", 0, M, 0, kNone, 0, 0, 0, 0, false, 3, "" } }, 0x3745d7 },
        { "quality.querySize", SeamKind::Function,
          { { "entry", kPinQuerySizeEntry, "", 0, M, 0, kNone, 0, 0, 0, 0, true, 3, "" },
            { "body", kPinQuerySizeBody, "", 0, E, 0, kNone, 0, 0, 0, 0, false, 3, "" },
            // the third, independent view (LOW-1): `lea rcx,[querySize]` of the manager constructor, the function stored in the renderer's size-query slot
            { "storedByManagerCtor", kPinManagerCtor, "", 0, T, 0, kRip, 7, 3, 7, 0, false, 3, "TargetIn:Code" } },
          0x352350 },
        // the SHAPE seams (CodeSite, ONE guard anchor each, the strict entry pattern verified at the location seam's address): the Guard fields
        // are bound to them, so a shape that does not match leaves the Guard field ZERO (see the hook-site rule in AnchorsQuality.h)
        { "quality.querySizeShape", SeamKind::CodeSite, { { "shape", kPinQuerySizeEntry, "quality.querySize", 0, M, 0, kNone, 0, 0, 0, 0, false, 3, "" } },
          0x352350, true },
        { "quality.rendererUpdate", SeamKind::Function,
          { { "entry", kPinRendererUpdateEntry, "", 0, M, 0, kNone, 0, 0, 0, 0, true, 3, "" },
            { "body", kPinRendererUpdateBody, "", 0, E, 0, kNone, 0, 0, 0, 0, false, 3, "" },
            { "caller", kPinRendererUpdateCaller, "", 0, T, 0, kRel, 11, 0, 0, 0, false, 3, "" } },
          0x2da5b0 },
        { "quality.rendererUpdateShape", SeamKind::CodeSite,
          { { "shape", kPinRendererUpdateEntry, "quality.rendererUpdate", 0, M, 0, kNone, 0, 0, 0, 0, false, 3, "" } }, 0x2da5b0, true },
        { "quality.resizeCallbacks", SeamKind::Function,
          { { "callInRendererUpdate", kPinResizeBlock, "", 0, T, 0, kRel, 4, 0, 0, 0, false, 3, "SameFunctionAs:quality.rendererUpdate" },
            { "entry", kPinResizeEntry, "", 0, M, 0, kNone, 0, 0, 0, 0, true, 3, "" } },
          0x2369a0 },
        { "quality.resizeCallbacksShape", SeamKind::CodeSite,
          { { "shape", kPinResizeEntry, "quality.resizeCallbacks", 0, M, 0, kNone, 0, 0, 0, 0, false, 3, "" } }, 0x2369a0, true },
        { "quality.settingsPointer", SeamKind::Global,
          { { "settingsBlockLoad", kPinSettingsBlock, "quality.settingsInstructions", 0, T, 0, kRip, 0, 3, 7, 0, false, 3, "TargetIn:Bss" },
            { "resizeBlockLoad", kPinResizeBlock, "", 0, T, 0, kRip, 9, 3, 7, 0, false, 3, "TargetIn:Bss" },
            { "querySizeLoad", kPinQuerySettingsWindow, "quality.querySize", 0x31, T, 0, kRip, 0, 3, 7, 0, false, 3, "TargetIn:Bss" } },
          0x290e470 },
        { "quality.managerPointer", SeamKind::Global,
          { { "contextLoad", kPinQueryManagerWindow, "quality.querySize", 0x15, T, 0, kRip, 0, 3, 7, 0, false, 3, "TargetIn:Bss" } }, 0x290e498 },
        { "quality.devicePointer", SeamKind::Global,
          { { "r14Load", kPinRendererUpdateBody, "quality.rendererUpdate", 0x1a, T, 0, kRip, 0x28, 3, 7, 0, false, 3, "TargetIn:Bss" },
            { "sizeBlockLoad", kPinSizeBlock, "", 0, T, 0, kRip, 0, 3, 7, 0, false, 3, "TargetIn:Bss;SameFunctionAs:quality.rendererUpdate" } },
          0x2907d00 },

        { "quality.settingsModeOffset", SeamKind::Offset,
          { { "settingsBlock", kPinSettingsBlock, "quality.settingsInstructions", 0, T, 0, kFld, 13, 0, 0, 1, false, 3, "ValueIn:16-85" },
            { "resizeBlock", kPinResizeBlock, "", 0, T, 0, kFld, 18, 0, 0, 1, false, 3, "ValueIn:16-85" },
            { "querySize", kPinQuerySettingsWindow, "quality.querySize", 0x31, T, 0, kFld, 9, 0, 0, 1, false, 3, "ValueIn:16-85" } },
          0x54 },
        { "quality.settingsQualityOffset", SeamKind::Offset,
          { { "settingsBlock", kPinSettingsBlock, "quality.settingsInstructions", 0, T, 0, kFld, 20, 0, 0, 1, false, 3, "ValueIn:16-85" },
            { "resizeBlock", kPinResizeBlock, "", 0, T, 0, kFld, 24, 0, 0, 1, false, 3, "ValueIn:16-85" } },
          0x44 },
        { "quality.settingsDlaaOffset", SeamKind::Offset,
          { { "settingsBlock", kPinSettingsBlock, "quality.settingsInstructions", 0, T, 0, kFld, 26, 0, 0, 1, false, 3, "ValueIn:16-85" },
            { "resizeBlock", kPinResizeBlock, "", 0, T, 0, kFld, 30, 0, 0, 1, false, 3, "ValueIn:16-85" } },
          0x45 },
        { "quality.settingsSelectorOffset", SeamKind::Offset,
          { { "settingsBlock", kPinSettingsBlock, "quality.settingsInstructions", 0, T, 0, kFld, 33, 0, 0, 1, false, 3, "ValueIn:16-85" } }, 0x55 },
        { "quality.managerContextOffset", SeamKind::Offset,
          { { "querySize", kPinQueryManagerWindow, "quality.querySize", 0x15, T, 0, kFld, 19, 0, 0, 4, false, 3, "ValueIn:4096-65536" },
            { "managerCtor", kPinManagerCtor, "", 0, T, 0, kFld, 3, 0, 0, 4, false, 3, "ValueIn:4096-65536" } },
          0x4220 },
        { "quality.deviceCallbacksOffset", SeamKind::Offset,
          { { "resizeBlock", kPinResizeBlock, "", 0, T, 0, kFld, 3, 0, 0, 1, false, 3, "ValueIn:8-127" } }, 0x30 },
        { "quality.rendererSceneSizeOffset", SeamKind::Offset,
          { { "sizeBlock", kPinSizeBlock, "", 0, T, 0, kFld, 15, 0, 0, 4, false, 3, "SameFunctionAs:quality.rendererUpdate;ValueIn:256-8192" } }, 0x428 },

        { "quality.contextResolver", SeamKind::Function,
          { { "viaSettingsBlock", kPinSettingsBlock, "", 0, E, 0, kNone, 0, 0, 0, 0, false, 3, "" },
            { "calledByQuerySize", kPinQueryCallWindow, "quality.querySize", 0x3e, T, 0, kRel, 9, 0, 0, 0, false, 3, "" } },
          0x374590 },
        { "quality.contextFlagsOffset", SeamKind::Offset,
          { { "flagsTest", kPinContextFlagsWindow, "quality.contextResolver", 8, T, 0, kFld, 2, 0, 0, 4, false, 3, "ValueIn:256-1024" },
            { "flagsLoad", kPinContextFlagsLoad, "", 0, T, 0, kFld, 3, 0, 0, 4, false, 4, "SameFunctionAs:quality.contextResolver;ValueIn:256-1024" } },
          0x181 },
        { "quality.cacheCompare", SeamKind::Function,
          { { "calledBeforeSettingsBlock", kPinCallContext, "quality.settingsInstructions", -16, T, 0, kRel, 11, 0, 0, 0, false, 3, "" },
            { "entry", kPinCacheCompare, "", 0, M, 0, kNone, 0, 0, 0, 0, true, 3, "" } },
          0x374f10 },
        { "quality.contextCacheOffset", SeamKind::Offset,
          { { "cacheLoad", kPinCacheWindow, "quality.cacheCompare", 0x0d, T, 0, kFld, 2, 0, 0, 4, false, 3, "ValueIn:256-1024" } }, 0x150 },

        { "camera.global", SeamKind::Global,
          { { "accessor", kPinCameraAccessor, "", 0, T, 0, kRip, 0, 3, 7, 0, false, 2, "TargetIn:Bss" },
            { "producerWindow", kPinCameraProducer, "", 0, T, 0, kRip, 0, 3, 7, 0, false, 3, "TargetIn:Bss" } },
          0x290dba0 },
        { "camera.getter", SeamKind::Function, { { "calledByProducer", kPinCameraProducer, "", 0, T, 0, kRel, 84, 0, 0, 0, false, 3, "" } }, 0x25f610 },
        { "camera.cameraOffset", SeamKind::Offset,
          { { "accessor", kPinCameraAccessor, "", 0, T, 0, kFld, 10, 0, 0, 4, false, 2, "ValueIn:4096-65536" },
            { "producerWindow", kPinCameraProducer, "", 0, T, 0, kFld, 24, 0, 0, 4, false, 3, "ValueIn:4096-65536" } },
          0x3028 },
        { "camera.jitterPointerOffset", SeamKind::Offset,
          { { "getterBody", kPinCameraGetter, "camera.getter", 0, T, 0, kFld, 3, 0, 0, 4, false, 3, "ValueIn:256-1024" } }, 0x278 },
    };
    return e;
}

// key -> seam of every binding (all guardOffset 0 and valueOffset 0: the guards start at the seam value)
const std::map<std::string, std::string>& ExpectedBindings()
{
    static const std::map<std::string, std::string> b = {
        { "charaView.accessorRva", "chara.accessor" },
        { "charaView.managerPointerRva", "chara.managerPointer" },
        { "charaView.targetsOffset", "chara.targetsOffset" },
        { "nativeQuality.settingsInstructionsRva", "quality.settingsInstructions" },
        { "nativeQuality.settingsInstructions", "quality.settingsInstructions" },
        { "nativeQuality.querySizeRva", "quality.querySize" },
        { "nativeQuality.querySizeInstructions", "quality.querySizeShape" },
        { "nativeQuality.rendererUpdateRva", "quality.rendererUpdate" },
        { "nativeQuality.rendererUpdateInstructions", "quality.rendererUpdateShape" },
        { "nativeQuality.resizeCallbacksRva", "quality.resizeCallbacks" },
        { "nativeQuality.resizeCallbacksInstructions", "quality.resizeCallbacksShape" },
        { "nativeQuality.settingsPointerRva", "quality.settingsPointer" },
        { "nativeQuality.managerPointerRva", "quality.managerPointer" },
        { "nativeQuality.devicePointerRva", "quality.devicePointer" },
        { "nativeQuality.settingsModeOffset", "quality.settingsModeOffset" },
        { "nativeQuality.settingsQualityOffset", "quality.settingsQualityOffset" },
        { "nativeQuality.settingsDlaaOffset", "quality.settingsDlaaOffset" },
        { "nativeQuality.settingsSelectorOffset", "quality.settingsSelectorOffset" },
        { "nativeQuality.managerContextOffset", "quality.managerContextOffset" },
        { "nativeQuality.contextFlagsOffset", "quality.contextFlagsOffset" },
        { "nativeQuality.contextCacheOffset", "quality.contextCacheOffset" },
        { "nativeQuality.deviceCallbacksOffset", "quality.deviceCallbacksOffset" },
        { "nativeQuality.rendererSceneSizeOffset", "quality.rendererSceneSizeOffset" },
        { "jitter.cameraGlobalRva", "camera.global" },
        { "jitter.cameraOffset", "camera.cameraOffset" },
        { "jitter.jitterPointerOffset", "camera.jitterPointerOffset" },
    };
    return b;
}

void TestPins()
{
    const Registry reg = QualityRegistry();
    CHECK(ValidateRegistry(reg).empty());
    const auto& expected = Expected();
    REQUIRE(reg.seams.size() == expected.size());
    CHECK(reg.mustDiffer.empty());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const ExpectSeam& e = expected[i];
        const SeamSpec& s = reg.seams[i];
        if (s.id != e.id)
        {
            std::printf("FAIL: seam #%zu is %s, expected %s\n", i, s.id.c_str(), e.id);
            ++g_failures;
            continue;
        }
        CHECK(s.kind == e.kind);
        if (s.failClosed != e.failClosed)
            std::printf("FAIL: seam %s: failClosed is %d, expected %d\n", e.id, s.failClosed ? 1 : 0, e.failClosed ? 1 : 0);
        CHECK(s.failClosed == e.failClosed);
        REQUIRE(s.anchors.size() == e.anchors.size());
        for (std::size_t a = 0; a < e.anchors.size(); ++a)
        {
            std::string why;
            if (!AnchorEquals(s.anchors[a], e.anchors[a], &why))
                std::printf("FAIL: seam %s: %s\n", e.id, why.c_str());
            CHECK(AnchorEquals(s.anchors[a], e.anchors[a]));
        }
        // a seam of this group has the prefix the registry test enforces
        const std::string_view id = e.id;
        CHECK(id.rfind("chara.", 0) == 0 || id.rfind("quality.", 0) == 0 || id.rfind("camera.", 0) == 0);
    }
    // the seam order: the two CharaView seams stay first (the frozen CharaView test reads the first two log lines)
    CHECK(reg.seams[0].id == "chara.accessor" && reg.seams[1].id == "chara.managerPointer");

    // bindings: one per field, every one with guardOffset 0 and valueOffset 0
    std::map<std::string, std::string> got;
    for (const Binding& b : reg.bindings)
    {
        CHECK(got.emplace(b.key, b.seam).second);
        CHECK(b.guardOffset == 0 && b.valueOffset == 0);
    }
    CHECK(got == ExpectedBindings());
    // HIGH-1 (structural, not a convention): each of the three hooked functions has its Rva field bound to the LOCATION seam and its Guard field to
    // a SHAPE seam, a CodeSite with exactly ONE anchor, a guard (never scanned) at the location seam's address, offset 0, the strict entry pattern
    // of the location seam. The Guard field on an unknown build is the live bytes at the resolved site: only a shape seam that FAILS leaves it zero.
    // The settings block is its own shape (one scanned strict anchor): its Guard and Rva stay on the same seam.
    {
        struct Hook
        {
            const char* rvaKey;
            const char* guardKey;
            const char* location;
            const char* shape;
        };
        const Hook hooks[] = {
            { "nativeQuality.querySizeRva", "nativeQuality.querySizeInstructions", "quality.querySize", "quality.querySizeShape" },
            { "nativeQuality.rendererUpdateRva", "nativeQuality.rendererUpdateInstructions", "quality.rendererUpdate", "quality.rendererUpdateShape" },
            { "nativeQuality.resizeCallbacksRva", "nativeQuality.resizeCallbacksInstructions", "quality.resizeCallbacks", "quality.resizeCallbacksShape" },
        };
        for (const Hook& h : hooks)
        {
            CHECK(got.at(h.rvaKey) == h.location);
            CHECK(got.at(h.guardKey) == h.shape);
            const SeamSpec* loc = FindSeam(reg, h.location);
            const SeamSpec* shp = FindSeam(reg, h.shape);
            REQUIRE(loc != nullptr && shp != nullptr);
            CHECK(loc->kind == SeamKind::Function && shp->kind == SeamKind::CodeSite);
            CHECK(shp->failClosed && !loc->failClosed); // the shape is never table-filled on a known build (a table guard is a prefix of it)
            REQUIRE(shp->anchors.size() == 1); // ONE anchor: with two, a failing one next to a passing one reads FoundSingleAnchor (usable)
            const AnchorSpec& g = shp->anchors[0];
            CHECK(g.fromSeam == h.location && g.fromOffset == 0 && g.valueOffset == 0 && g.valueFrom == ValueFrom::Match);
            CHECK(g.predicates.empty() && !g.entryAnchor);
            // the strict pattern IS the location seam's entry pattern, byte for byte, masks included
            const AnchorSpec* entry = nullptr;
            for (const AnchorSpec& a : loc->anchors)
            {
                if (a.name == "entry")
                    entry = &a;
            }
            REQUIRE(entry != nullptr);
            CHECK(g.pattern == entry->pattern);
        }
        CHECK(got.at("nativeQuality.settingsInstructions") == got.at("nativeQuality.settingsInstructionsRva"));
    }

    // the fields of the group that no seam fills are exactly the documented ones (Rva and Guard fields: all covered)
    const auto unbound = UnboundFields(reg);
    for (const std::string& key : unbound)
    {
        const std::string_view k = key;
        CHECK(!(k.rfind("nativeQuality.", 0) == 0 || k.rfind("charaView.", 0) == 0 || k == "jitter.cameraGlobalRva"));
    }
    for (const char* key : { "nativeQuality.settingsRendererOffset", "nativeQuality.settingsAdditionalOffset" })
        CHECK(got.find(key) == got.end()); // not self-healing: no instruction carries them
    CHECK(FindField("nativeQuality.settingsRendererOffset") != nullptr && FindField("nativeQuality.settingsRendererOffset")->kind == FieldKind::Offset);
}

void TestResolveCommittedBytes()
{
    const Registry reg = QualityRegistry();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);

    LogCapture log;
    ResolveOptions opts;
    opts.knownTable = &ext::ffxiv::kCurrentBuild;
    opts.sink = log.Sink();
    const ResolvedBuild rb = ResolveImage(*img, reg, opts);
    CHECK(rb.registryErrors.empty() && rb.knownBuild);

    for (const ExpectSeam& e : Expected())
    {
        const SeamResult& s = Get(rb, e.id);
        if (s.status != SeamStatus::Found)
            std::printf("FAIL: seam %s is %s: %s\n", e.id, StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::Found && s.rva == e.rva && s.reason.empty());
        CHECK(s.anchors.size() == e.anchors.size());
        for (const AnchorResult& a : s.anchors)
        {
            if (a.outcome != AnchorOutcome::Unique)
                std::printf("FAIL: seam %s anchor %s: not unique (%s)\n", e.id, a.name.c_str(), a.note.c_str());
            CHECK(a.outcome == AnchorOutcome::Unique && a.candidates == 1 && a.patternHits == 1 && !a.weakDependency);
        }
        CHECK(!s.fellBackToTable);
        if (s.hasTable)
            CHECK(s.agreement == Agreement::Agree);
    }
    // the log: one line per seam, Info only (a Found seam that agrees with the table is not a WARN), then the total
    CHECK(log.lines.size() == Expected().size() + 1);
    for (const auto& l : log.lines)
        CHECK(l.first == LogLevel::Info);
    CHECK(log.lines[0].second.rfind("[ffxiv-resolve] chara.accessor Found rva=0x26abd0 hits=1 table=0x26abd0 agree ms=", 0) == 0);
    CHECK(log.lines[1].second.rfind("[ffxiv-resolve] chara.managerPointer Found rva=0x290e490 hits=1 table=0x290e490 agree ms=", 0) == 0);

    // the same without the table: the pure pattern-search answer, field for field and as text
    const ResolvedBuild pure = ResolveImage(*img, reg);
    CHECK(!pure.knownBuild);
    for (const ExpectSeam& e : Expected())
        CHECK(Get(pure, e.id).status == SeamStatus::Found && Get(pure, e.id).rva == e.rva);
    CHECK(FormatEntry(pure, reg) == FormatTableEntry(ext::ffxiv::kCurrentBuild, reg));
    CHECK(FormatEntry(rb, reg) == FormatTableEntry(ext::ffxiv::kCurrentBuild, reg));

    const ext::ffxiv::NativeQualityBuild& q = pure.build.nativeQuality;
    CHECK(q.settingsInstructionsRva == 0x3745d7 && q.querySizeRva == 0x352350 && q.rendererUpdateRva == 0x2da5b0 && q.resizeCallbacksRva == 0x2369a0);
    CHECK(q.settingsPointerRva == 0x290e470 && q.managerPointerRva == 0x290e498 && q.devicePointerRva == 0x2907d00);
    CHECK(q.settingsModeOffset == 0x54 && q.settingsQualityOffset == 0x44 && q.settingsDlaaOffset == 0x45 && q.settingsSelectorOffset == 0x55);
    CHECK(q.managerContextOffset == 0x4220 && q.contextFlagsOffset == 0x181 && q.contextCacheOffset == 0x150 && q.deviceCallbacksOffset == 0x30 &&
          q.rendererSceneSizeOffset == 0x428);
    CHECK(pure.build.charaView.accessorRva == 0x26abd0 && pure.build.charaView.managerPointerRva == 0x290e490 && pure.build.charaView.targetsOffset == 0x2c8);
    CHECK(pure.build.jitter.cameraGlobalRva == 0x290dba0 && pure.build.jitter.cameraOffset == 0x3028 && pure.build.jitter.jitterPointerOffset == 0x278);
    // the offsets the group does NOT bind keep the table defaults (not self-healing)
    CHECK(q.settingsRendererOffset == 0x47 && q.settingsAdditionalOffset == 0x49);

    // cross-check of two groups' facts: the CharaView manager global IS the renderer object that rendererUpdate is called with (the caller loads it
    // into rcx right before the call), i.e. the same global as jitterSource.rendererPointerRva of the jitter group (0x290e490)
    {
        std::int32_t disp = 0;
        CHECK(img->Read(0x2b9e69 + 3, &disp, 4));
        CHECK(static_cast<std::uint32_t>(0x2b9e69 + 7 + disp) == pure.build.charaView.managerPointerRva);
    }

    // the guard fields: the bytes at the resolved address, with the length of the table's array, equal to the table's
    CHECK(q.settingsInstructions == ext::ffxiv::kCurrentBuild.nativeQuality.settingsInstructions);
    CHECK(q.querySizeInstructions == ext::ffxiv::kCurrentBuild.nativeQuality.querySizeInstructions);
    CHECK(q.rendererUpdateInstructions == ext::ffxiv::kCurrentBuild.nativeQuality.rendererUpdateInstructions);
    CHECK(q.resizeCallbacksInstructions == ext::ffxiv::kCurrentBuild.nativeQuality.resizeCallbacksInstructions);

    // the helper seams have no table field
    for (const char* id : { "quality.contextResolver", "quality.cacheCompare", "camera.getter" })
        CHECK(!Get(rb, id).hasTable && Get(rb, id).agreement == Agreement::NotChecked);
}

// The anchors, one by one and independent of the other anchors of their seam: the seam with ONE anchor left (every other anchor removed) still
// resolves to the same value and is plain Found when it is a one-anchor seam, FoundSingleAnchor otherwise. This is what the weak-seam policy sees
// on an unknown build in which only one route survives.
void TestEachAnchorAlone()
{
    const Registry full = QualityRegistry();
    auto img = MakeQualityCWithSites();
    REQUIRE(img != nullptr);
    for (const ExpectSeam& e : Expected())
    {
        for (std::size_t keep = 0; keep < e.anchors.size(); ++keep)
        {
            if (e.anchors.size() == 1)
                continue;
            Registry reg = full;
            SeamSpec* s = FindSeam(reg, e.id);
            REQUIRE(s != nullptr);
            if (keep >= s->anchors.size())
            {
                CHECK(keep < s->anchors.size()); // fewer anchors than pinned: the pins test already says so
                continue;
            }
            AnchorSpec only = s->anchors[keep];
            s->anchors = { only };
            const ResolvedBuild rb = ResolveImage(*img, reg);
            const SeamResult& r = Get(rb, e.id);
            if (r.status != SeamStatus::Found || r.rva != e.rva)
                std::printf("FAIL: seam %s with only anchor '%s': %s rva=0x%x (%s)\n", e.id, only.name.c_str(), StatusName(r.status), r.rva, r.reason.c_str());
            CHECK(r.status == SeamStatus::Found && r.rva == e.rva);
        }
    }
}
} // namespace

int main()
{
    TestPins();
    TestResolveCommittedBytes();
    TestEachAnchorAlone();
    return TEST_RESULT("resolve_quality_pins");
}
