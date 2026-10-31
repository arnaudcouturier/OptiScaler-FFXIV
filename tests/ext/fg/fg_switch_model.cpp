// The measured release model table (ext/fg/outswitch/LibraryModel.h): how a probe run becomes M1 or M2 (or "unknown",
// which is refused), why three consistent runs are needed, what the witness answer means, and that a lookup for a
// library build nobody measured fails closed instead of assuming the friendlier model.
//
// It also parses the probe data file (tools/fg-probe/model-rows.txt) with its own parser and compares every row with the
// compiled table (ModelRows.h): the generated header must describe exactly the data a person wrote down. The parser is
// exercised on a fixture too, so an empty data file still tests it.
#include "ext/fg/outswitch/LibraryModel.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace ext::fg::outswitch;

static int g_failed = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
LibraryIdentity Identity(const char* file, uint64_t size, uint32_t stamp)
{
    LibraryIdentity id;
    std::snprintf(id.file, sizeof(id.file), "%s", file);
    id.size = size;
    id.stamp = stamp;
    return id;
}

// A measurement with a witness the library accepted: M2 is only a row with one, so every M2 expectation starts from
// this.
ProbeRuns Witnessed(ProbeRuns runs)
{
    runs.witnessAttempted = true;
    runs.witnessAccepted = true;
    runs.witnessReleasedWithOuter = true;
    for (size_t i = 0; i < runs.runCount; ++i)
        runs.runs[i].witnessAliveBeforePin = true;
    return runs;
}

ProbeRuns Runs(bool freeAfterDestroy, bool freeAfterPin, size_t count = kProbeRuns)
{
    ProbeRuns runs;
    runs.ran = true;
    runs.runCount = count;
    for (size_t i = 0; i < count; ++i)
    {
        runs.runs[i].windowFreeAfterDestroy = freeAfterDestroy;
        runs.runs[i].windowStableAfterDestroy = true;
        runs.runs[i].windowFreeAfterPin = freeAfterPin;
        runs.runs[i].windowStableAfterPin = true;
    }
    return runs;
}

// One pin-first run: the caller's reference released first, then one destroy that frees the object. Every field the
// classifier reads is set the way the host writes it in that order.
ProbeRuns PinFirstRuns(size_t count = kProbeRuns)
{
    ProbeRuns runs;
    runs.ran = true;
    runs.runCount = count;
    for (size_t i = 0; i < count; ++i)
    {
        runs.runs[i].pinFirst = true;
        runs.runs[i].windowFreeAfterDestroy = true;
        runs.runs[i].windowStableAfterDestroy = true;
        runs.runs[i].windowFreeAfterPin = true;
        runs.runs[i].windowStableAfterPin = true;
        runs.runs[i].witnessAliveBeforePin = true;     // the context held the object after the reference was released
        runs.runs[i].witnessAliveAfterDestroy = false; // and the single destroy freed it
        runs.runs[i].firstDestroyCode = 0;
        runs.runs[i].destroyAfterPinAttempted = false;
    }
    return runs;
}

void Classify()
{
    std::printf("  a probe run becomes a model\n");

    {
        ProbeRuns runs;
        const Classified nothing = ClassifyProbeRuns(runs);
        CHECK(nothing.model.model == Model::Unknown);
        CHECK(!nothing.model.witnessHonoured);
    }
    {
        // One run, however clean, is not a measurement: a library that tears down on its own thread makes one read wrong.
        const Classified one = ClassifyProbeRuns(Runs(true, false, 1));
        CHECK(one.model.model == Model::Unknown);
        const Classified two = ClassifyProbeRuns(Runs(true, false, 2));
        CHECK(two.model.model == Model::Unknown);
    }
    {
        // Every run saw the window free after the destroy: the destroy ended the object.
        const Classified m1 = ClassifyProbeRuns(Runs(true, false));
        CHECK(m1.model.model == Model::M1);
    }
    {
        // Bound after every destroy, free after every pin release, witness alive in every run: the context held the
        // reference.
        const Classified m2 = ClassifyProbeRuns(Witnessed(Runs(false, true)));
        CHECK(m2.model.model == Model::M2);
        CHECK(m2.model.witnessHonoured);
    }
    {
        // Bound even after the pin release: neither model explains it. The witness is in place, so only that answer
        // decides here.
        CHECK(ClassifyProbeRuns(Witnessed(Runs(false, false))).model.model == Model::Unknown);
    }
    {
        // A run whose reads never settled makes the whole measurement unknown, in either direction.
        ProbeRuns flapping = Witnessed(Runs(false, true));
        flapping.runs[1].windowStableAfterDestroy = false;
        CHECK(ClassifyProbeRuns(flapping).model.model == Model::Unknown);
        ProbeRuns flappingPin = Witnessed(Runs(false, true));
        flappingPin.runs[2].windowStableAfterPin = false;
        CHECK(ClassifyProbeRuns(flappingPin).model.model == Model::Unknown);
    }
    {
        // The runs disagree: M2 is never inferred from a mixed measurement.
        ProbeRuns mixed = Runs(true, false);
        mixed.runs[2].windowFreeAfterDestroy = false;
        mixed.runs[2].windowFreeAfterPin = true;
        CHECK(ClassifyProbeRuns(mixed).model.model == Model::Unknown);
    }
    {
        // A row measured in the pin-first order: the reference released first, the object still alive (the context held
        // it), then exactly one destroy that frees it. That is the order the product uses for such a row, and the only
        // one it may use.
        const Classified pinned = ClassifyProbeRuns(Witnessed(PinFirstRuns()));
        CHECK(pinned.model.model == Model::M2);
        CHECK(pinned.model.witnessHonoured);
        CHECK(pinned.pinFirst);

        // The witness still there after the single destroy: the destroy refused, the object leaked, so no row.
        ProbeRuns leaked = Witnessed(PinFirstRuns());
        leaked.runs[2].witnessAliveAfterDestroy = true;
        CHECK(ClassifyProbeRuns(leaked).model.model == Model::Unknown);

        // The reference was the last one: the object died with it, so the row does not describe a context-held chain.
        ProbeRuns died = Witnessed(PinFirstRuns());
        died.runs[1].witnessAliveBeforePin = false;
        CHECK(ClassifyProbeRuns(died).model.model == Model::Unknown);

        // A second destroy anywhere means the run was not the pin-first order alone.
        ProbeRuns twice = Witnessed(PinFirstRuns());
        twice.runs[0].destroyAfterPinAttempted = true;
        CHECK(ClassifyProbeRuns(twice).model.model == Model::Unknown);

        // The library's own destroy answer must be a success.
        ProbeRuns refused = Witnessed(PinFirstRuns());
        refused.runs[0].firstDestroyCode = 29;
        CHECK(ClassifyProbeRuns(refused).model.model == Model::Unknown);

        // The window still bound after the single destroy: the destroy did not free it.
        ProbeRuns bound = Witnessed(PinFirstRuns());
        bound.runs[0].windowFreeAfterDestroy = false;
        CHECK(ClassifyProbeRuns(bound).model.model == Model::Unknown);

        // Two orders in one measurement are two measurements, not one row.
        ProbeRuns mixedOrder = Witnessed(PinFirstRuns());
        mixedOrder.runs[1].pinFirst = false;
        CHECK(ClassifyProbeRuns(mixedOrder).model.model == Model::Unknown);
        // A pin-first row without a witness the library accepted has no cross-check for the release it makes first.
        ProbeRuns noWitness = PinFirstRuns();
        CHECK(ClassifyProbeRuns(noWitness).model.model == Model::Unknown);
    }
    std::printf("    [ok] M1 when every run frees the window at the destroy, M2 only when every run stays bound, unknown "
                "otherwise\n");

    std::printf("  M2 needs the witness to agree in every run\n");
    {
        // The witness was accepted and stayed alive in every run: the dangerous direction is measured on both signals.
        ProbeRuns runs = Witnessed(Runs(false, true));
        CHECK(ClassifyProbeRuns(runs).model.model == Model::M2);

        // Bound and freed after the pin, but the witness died before the pin: the two signals disagree, so no row.
        ProbeRuns gone = runs;
        gone.runs[1].witnessAliveBeforePin = false;
        const Classified unknown = ClassifyProbeRuns(gone);
        CHECK(unknown.model.model == Model::Unknown);
        CHECK(std::string_view(unknown.evidence).find("witness") != std::string_view::npos);

        // A run whose witness said the object was gone skipped the release: the measurement cannot be used at all.
        ProbeRuns skipped = runs;
        skipped.runs[0].releaseSkipped = true;
        CHECK(ClassifyProbeRuns(skipped).model.model == Model::Unknown);

        // M1 with a witness that outlived the destroy is a contradiction too.
        ProbeRuns alive = Runs(true, false);
        alive.witnessAttempted = true;
        alive.witnessAccepted = true;
        alive.witnessReleasedWithOuter = true;
        alive.runs[2].witnessAliveBeforePin = true;
        CHECK(ClassifyProbeRuns(alive).model.model == Model::Unknown);

        // A witness the library refused is not a witness: M2 on the window alone is the guess this table must never
        // make, so the measurement is unknown. M1 releases nothing and does not need one.
        ProbeRuns refused = Runs(false, true);
        refused.witnessAttempted = true;
        refused.witnessAccepted = false;
        const Classified rejected = ClassifyProbeRuns(refused);
        CHECK(rejected.model.model == Model::Unknown);
        CHECK(std::string_view(rejected.evidence).find("witness") != std::string_view::npos);
        CHECK(!rejected.model.witnessHonoured);

        // The same rule when no witness was placed at all, with every other M2 signal present: the window was bound and
        // a release freed it, and the run even carries an alive sample - but no accepted interface, so there is no
        // cross-check and no row.
        ProbeRuns absent = Witnessed(Runs(false, true));
        absent.witnessAttempted = false;
        absent.witnessAccepted = false;
        CHECK(ClassifyProbeRuns(absent).model.model == Model::Unknown);
        ProbeRuns m1Absent = Runs(true, false);
        m1Absent.witnessAttempted = false;
        CHECK(ClassifyProbeRuns(m1Absent).model.model == Model::M1);

        // A witness that outlived the outer chain was forwarded to an inner one: it says nothing about the object the
        // pin release touches.
        ProbeRuns forwarded = Witnessed(Runs(false, true));
        forwarded.witnessReleasedWithOuter = false;
        CHECK(ClassifyProbeRuns(forwarded).model.model == Model::Unknown);
    }
    std::printf("    [ok] M2 only with the window bound, the pin freeing it and the witness alive in every run\n");

    std::printf("  the witness answer is recorded, not assumed\n");
    {
        ProbeRuns runs = Runs(false, true);
        runs.witnessAttempted = true;
        runs.witnessAccepted = true;
        runs.witnessReleasedWithOuter = true;
        CHECK(ClassifyProbeRuns(runs).model.witnessHonoured);

        ProbeRuns forwarded = runs;
        forwarded.witnessReleasedWithOuter = false; // the interface went to an inner chain
        CHECK(!ClassifyProbeRuns(forwarded).model.witnessHonoured);

        ProbeRuns refused = runs;
        refused.witnessAccepted = false;
        refused.witnessReleasedWithOuter = true;
        CHECK(!ClassifyProbeRuns(refused).model.witnessHonoured);

        ProbeRuns never = runs;
        never.witnessAttempted = false;
        never.witnessAccepted = true;
        never.witnessReleasedWithOuter = true;
        CHECK(!ClassifyProbeRuns(never).model.witnessHonoured);
    }
    std::printf("    [ok] honoured only when the interface was accepted and died with the outer chain\n");
}

void Identities()
{
    std::printf("  a library build is compared by name, size and PE timestamp\n");
    const LibraryIdentity fsr = Identity("amd_fidelityfx_framegeneration_dx12.dll", 12345678, 0x65F1A2B3u);
    const LibraryIdentity same = Identity("amd_fidelityfx_framegeneration_dx12.dll", 12345678, 0x65F1A2B3u);
    CHECK(SameLibrary(fsr, same));
    CHECK(!SameLibrary(fsr, Identity("amd_fidelityfx_framegeneration_dx12.dll", 12345679, 0x65F1A2B3u)));
    CHECK(!SameLibrary(fsr, Identity("libxess_fg.dll", 12345678, 0x65F1A2B3u)));
    CHECK(!SameLibrary(fsr, Identity("amd_fidelityfx_framegeneration_dx12.dll", 12345678, 0x65F1A2B4u)));
    CHECK(!fsr.Empty());
    CHECK(LibraryIdentity {}.Empty());

    char text[160] {};
    IdentityText(fsr, text);
    const std::string_view line(text);
    CHECK(line.find("amd_fidelityfx_framegeneration_dx12.dll") != std::string_view::npos);
    CHECK(line.find("12345678") != std::string_view::npos);
    CHECK(line.find("65F1A2B3") != std::string_view::npos);
    std::printf("    [ok] %s\n", text);
}

void Table()
{
    std::printf("  the table refuses an unmeasured build instead of guessing\n");
    ModelTable table;
    CHECK(table.Count() == 0);
    // The bound is part of the rule: a table that grows without a limit turns a measurement file into unbounded data.
    CHECK(ModelTable::kMaxRows == 16);

    const LibraryIdentity fsr = Identity("amd_fidelityfx_framegeneration_dx12.dll", 111, 0x10u);
    const LibraryIdentity xefg = Identity("libxess_fg.dll", 222, 0x20u);

    const LibraryIdentity xell = Identity("libxell.dll", 444, 0x40u);

    ModelRow row;
    row.cls = Class::FsrFg;
    row.library = fsr;
    row.model = LibraryModel { Model::M2, true };
    row.standstillRefs = 2;
    std::snprintf(row.config, sizeof(row.config), "%s", ConfigText(Class::FsrFg));
    CHECK(table.Add(row));
    CHECK(table.Count() == 1);

    ModelRow other;
    other.cls = Class::XeFg;
    other.library = xefg;
    other.companion = xell;
    other.model = LibraryModel { Model::M1, false };
    other.standstillRefs = 2;
    std::snprintf(other.config, sizeof(other.config), "%s", ConfigText(Class::XeFg));
    CHECK(table.Add(other));

    LibraryModel found;
    CHECK(table.Find(Class::FsrFg, fsr, found));
    CHECK(found.model == Model::M2 && found.witnessHonoured);
    CHECK(found.standstillRefs == 2); // the row carries the count the preflight compares against
    CHECK(table.Find(Class::XeFg, xefg, xell, found));
    CHECK(found.model == Model::M1 && !found.witnessHonoured);

    // The same build under another class is not known.
    CHECK(!table.Find(Class::XeFg, fsr, xell, found));
    // Another build of the same library is not known.
    CHECK(!table.Find(Class::FsrFg, Identity("amd_fidelityfx_framegeneration_dx12.dll", 112, 0x10u), found));
    // Another companion build is another pair: the XeFG row is refused.
    CHECK(!table.Find(Class::XeFg, xefg, Identity("libxell.dll", 445, 0x40u), found));
    CHECK(!table.Find(Class::XeFg, xefg, LibraryIdentity {}, found));
    // A caller that cannot name the companion gets no XeFG row (the 3-argument overload), and the same answer for FSR FG
    // does not depend on it.
    CHECK(!table.Find(Class::XeFg, xefg, found));
    CHECK(table.Find(Class::FsrFg, fsr, LibraryIdentity {}, found));
    // Plain has no library.
    CHECK(!table.Find(Class::Plain, fsr, found));
    // A build with no name is not a build.
    CHECK(!table.Find(Class::FsrFg, LibraryIdentity {}, found));

    // One build has one answer: a second probe does not overwrite the first.
    CHECK(!table.Add(row));
    // An unusable row is refused.
    ModelRow empty;
    empty.cls = Class::FsrFg;
    empty.model = LibraryModel { Model::M1, false };
    CHECK(!table.Add(empty));
    // A row measured in the configuration the product does not run is not a row: an XeFG context without the XeLL context
    // is exactly the setup whose reference count the pin release depends on.
    ModelRow wrongConfig;
    wrongConfig.cls = Class::XeFg;
    wrongConfig.library = Identity("libxess_fg.dll", 333, 0x30u);
    wrongConfig.companion = xell;
    wrongConfig.model = LibraryModel { Model::M2, true };
    wrongConfig.standstillRefs = 2;
    std::snprintf(wrongConfig.config, sizeof(wrongConfig.config), "%s", ConfigText(Class::FsrFg));
    CHECK(!table.Add(wrongConfig));
    // An XeFG row with no companion build is not a row either: the pair is what was measured.
    ModelRow noCompanion = wrongConfig;
    std::snprintf(noCompanion.config, sizeof(noCompanion.config), "%s", ConfigText(Class::XeFg));
    noCompanion.companion = LibraryIdentity {};
    CHECK(!noCompanion.Usable());
    CHECK(!table.Add(noCompanion));
    // And a row with no standstill count cannot answer the preflight's comparison.
    ModelRow noRefs = row;
    noRefs.library = Identity("build.dll", 5, 1);
    noRefs.standstillRefs = 0;
    CHECK(!noRefs.Usable());
    CHECK(!table.Add(noRefs));
    ModelRow unknown = row;
    unknown.model = LibraryModel {};
    CHECK(!table.Add(unknown));

    // M2 without a witness the library accepted is not a row: it is the one direction that releases a pin on the
    // table's word alone, and the witness is what the teardown cross-checks that release against.
    ModelRow m2NoWitness = row;
    m2NoWitness.library = Identity("build.dll", 6, 1);
    m2NoWitness.model = LibraryModel { Model::M2, false };
    CHECK(!m2NoWitness.Usable());
    CHECK(!table.Add(m2NoWitness));
    // M1 does not need the witness, so the same row with M1 is admitted.
    ModelRow m1NoWitness = m2NoWitness;
    m1NoWitness.model = LibraryModel { Model::M1, false };
    CHECK(m1NoWitness.Usable());
    CHECK(table.Add(m1NoWitness));

    // The table fills up and then says no instead of dropping a row.
    ModelTable full;
    for (int i = 0; i < static_cast<int>(ModelTable::kMaxRows); ++i)
    {
        ModelRow many;
        many.cls = Class::FsrFg;
        many.library = Identity("build.dll", static_cast<uint64_t>(i + 1), 1);
        many.model = LibraryModel { Model::M2, true };
        many.standstillRefs = 2;
        std::snprintf(many.config, sizeof(many.config), "%s", ConfigText(Class::FsrFg));
        CHECK(full.Add(many));
    }
    ModelRow over;
    over.cls = Class::XeFg;
    over.library = Identity("other.dll", 999, 1);
    over.companion = xell;
    over.model = LibraryModel { Model::M2, true };
    over.standstillRefs = 2;
    std::snprintf(over.config, sizeof(over.config), "%s", ConfigText(Class::XeFg));
    CHECK(!full.Add(over));
    CHECK(full.Count() == ModelTable::kMaxRows);
    std::printf("    [ok] add and find, duplicates and unusable rows refused, never a wrong answer\n");

    ModelRow logged;
    logged.cls = Class::XeFg;
    logged.library = xefg;
    logged.companion = xell;
    logged.model = LibraryModel { Model::M1, true };
    logged.standstillRefs = 2;
    std::snprintf(logged.config, sizeof(logged.config), "%s", ConfigText(Class::XeFg));
    char text[512] {};
    FormatRow(logged, "[probe]", text);
    const std::string_view line(text);
    CHECK(line.find("[probe] model class=xefg") == 0);
    CHECK(line.find("libxess_fg.dll") != std::string_view::npos);
    CHECK(line.find("verdict=M1") != std::string_view::npos);
    CHECK(line.find("witness=honoured") != std::string_view::npos);
    CHECK(line.find("config=xell") != std::string_view::npos);
    CHECK(line.find("refs=2") != std::string_view::npos);
    CHECK(line.find("companionFile=libxell.dll") != std::string_view::npos);
    std::printf("    [ok] %s\n", text);

    // One spelling for one answer: the data file takes `no`, so the host prints `no` and not "not honoured".
    ModelRow denied = logged;
    denied.model.witnessHonoured = false;
    char deniedText[512] {};
    FormatRow(denied, "fg-probe", deniedText);
    const std::string_view deniedLine(deniedText);
    CHECK(deniedLine.find("witness=no") != std::string_view::npos);
    CHECK(deniedLine.find("not honoured") == std::string_view::npos);
    std::printf("    [ok] %s\n", deniedText);

    // The same kind of library with a different build of the companion is not the pair that was measured.
    ModelRow otherCompanion = other;
    otherCompanion.companion = Identity("libxell.dll", 446, 0x40u);
    CHECK(!SameRow(otherCompanion, xefg, xell));
    CHECK(SameRow(other, xefg, xell));
    CHECK(SameRow(row, fsr, LibraryIdentity {})); // FSR FG has no companion
}

void ExpectedReferenceCount()
{
    std::printf("  the row carries the count the preflight compares against\n");
    LibraryModel row { Model::M2, true, false, 2 };
    CHECK(ExpectedRefs(row, 0) == 2);  // nothing measured live: the row's own count
    CHECK(ExpectedRefs(row, 3) == 3);  // the live probe's count wins when it ran
    CHECK(ExpectedRefs(LibraryModel {}, 0) == 0); // nothing known: the comparison stays off
    std::printf("    [ok] a row without a live count still gives the switch something to compare\n");
}

// ---- the data file the generated header comes from ---------------------------------------------------------------

struct ParsedRow
{
    Class cls = Class::Plain;
    std::string file;
    uint64_t size = 0;
    uint32_t stamp = 0;
    std::string companionFile;
    uint64_t companionSize = 0;
    uint32_t companionStamp = 0;
    std::string config;
    Model model = Model::Unknown;
    bool witnessHonoured = false;
    bool pinFirst = false;
    uint32_t refs = 0;
    std::string ctx = "unknown";
    std::string orderB = "unknown";
    int32_t vramDeltaMb = 0;
};

bool ParseClass(const std::string& text, Class& out)
{
    if (text == "fsrfg")
        out = Class::FsrFg;
    else if (text == "xefg")
        out = Class::XeFg;
    else
        return false;
    return true;
}

// One row per line: class=<fsrfg|xefg> file=<name> size=<bytes> stamp=<0x...> config=<direct|xell> verdict=<M1|M2>
// witness=<honoured|no>.
// '#' starts a comment, blank lines are skipped. Anything else is an error, never a silently dropped row.
bool ParseRows(const std::string& text, std::vector<ParsedRow>& out, std::string& error)
{
    size_t lineNumber = 0;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNumber;

        const size_t comment = line.find('#');
        if (comment != std::string::npos)
            line = line.substr(0, comment);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            ++start;
        line = line.substr(start);
        if (line.empty())
            continue;

        ParsedRow row;
        bool haveClass = false, haveFile = false, haveSize = false, haveStamp = false, haveConfig = false,
             haveVerdict = false, haveWitness = false, haveOrder = false, haveRefs = false, haveCompanion = false;
        size_t field = 0;
        bool bad = false;
        while (field <= line.size())
        {
            size_t space = line.find(' ', field);
            if (space == std::string::npos)
                space = line.size();
            const std::string token = line.substr(field, space - field);
            field = space + 1;
            if (token.empty())
                continue;

            const size_t eq = token.find('=');
            if (eq == std::string::npos)
            {
                bad = true;
                continue;
            }
            const std::string key = token.substr(0, eq);
            const std::string value = token.substr(eq + 1);
            if (key == "class")
            {
                haveClass = ParseClass(value, row.cls);
                bad = bad || !haveClass;
            }
            else if (key == "file")
            {
                row.file = value;
                haveFile = !value.empty();
                bad = bad || !haveFile;
            }
            else if (key == "size")
            {
                row.size = std::strtoull(value.c_str(), nullptr, 10);
                haveSize = row.size != 0;
                bad = bad || !haveSize;
            }
            else if (key == "stamp")
            {
                row.stamp = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 16));
                haveStamp = row.stamp != 0;
                bad = bad || !haveStamp;
            }
            else if (key == "config")
            {
                row.config = value;
                haveConfig = ConfigUsable(row.cls, value.c_str());
                bad = bad || !haveConfig;
            }
            else if (key == "verdict")
            {
                row.model = value == "M1" ? Model::M1 : value == "M2" ? Model::M2 : Model::Unknown;
                haveVerdict = row.model != Model::Unknown;
                bad = bad || !haveVerdict;
            }
            else if (key == "witness")
            {
                haveWitness = value == "honoured" || value == "no";
                row.witnessHonoured = value == "honoured";
                bad = bad || !haveWitness;
            }
            else if (key == "order")
            {
                haveOrder = value == "pinfirst" || value == "destroyfirst";
                row.pinFirst = value == "pinfirst";
                // A pin-first row releases a reference before the destroy, which only an M2 row with an accepted witness
                // may do.
                bad = bad || !haveOrder || (row.pinFirst && (row.model != Model::M2 || !row.witnessHonoured));
            }
            else if (key == "refs")
            {
                row.refs = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                haveRefs = row.refs != 0;
                bad = bad || !haveRefs;
            }
            else if (key == "companionFile")
            {
                row.companionFile = value;
                haveCompanion = !value.empty();
                bad = bad || !haveCompanion;
            }
            else if (key == "companionSize")
            {
                row.companionSize = std::strtoull(value.c_str(), nullptr, 10);
                bad = bad || row.companionSize == 0;
            }
            else if (key == "companionStamp")
            {
                row.companionStamp = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 16));
                bad = bad || row.companionStamp == 0;
            }
            else if (key == "ctx")
            {
                row.ctx = value;
                bad = bad || (value != "destroyed" && value != "refused" && value != "unknown");
            }
            else if (key == "orderB")
            {
                row.orderB = value;
                bad = bad || (value != "clean" && value != "leaked" && value != "unknown");
            }
            else if (key == "vramDeltaMb")
            {
                row.vramDeltaMb = static_cast<int32_t>(std::strtol(value.c_str(), nullptr, 10));
            }
            else
            {
                bad = true;
            }
        }

        if (bad || !haveClass || !haveFile || !haveSize || !haveStamp || !haveConfig || !haveVerdict || !haveWitness ||
            !haveOrder || !haveRefs || (row.cls == Class::XeFg && !haveCompanion) ||
            (row.cls == Class::XeFg && (row.companionSize == 0 || row.companionStamp == 0)))
        {
            char message[128] {};
            std::snprintf(message, sizeof(message), "line %zu", lineNumber);
            error = message;
            return false;
        }
        out.push_back(row);
    }
    return true;
}

std::string ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void DataFile(const char* path)
{
    std::printf("  the generated rows describe exactly the data file\n");

    // The parser itself, on a fixture, so this check still covers it while the data file is empty.
    {
        const std::string fixture =
            "# a comment\n"
            "class=fsrfg file=amd_fidelityfx_framegeneration_dx12.dll size=1234 stamp=65F1A2B3 config=direct verdict=M2 "
            "witness=honoured order=destroyfirst refs=2\n"
            "\n"
            "class=xefg file=libxess_fg.dll size=5678 stamp=1234ABCD companionFile=libxell.dll companionSize=9012 "
            "companionStamp=76543210 config=xell verdict=M1 witness=no order=destroyfirst refs=3 ctx=refused "
            "orderB=leaked vramDeltaMb=-4\n";
        std::vector<ParsedRow> rows;
        std::string error;
        CHECK(ParseRows(fixture, rows, error));
        CHECK(rows.size() == 2);
        CHECK(rows[0].cls == Class::FsrFg && rows[0].file == "amd_fidelityfx_framegeneration_dx12.dll");
        CHECK(rows[0].size == 1234 && rows[0].stamp == 0x65F1A2B3u);
        CHECK(rows[0].model == Model::M2 && rows[0].witnessHonoured);
        CHECK(rows[0].config == "direct" && rows[1].config == "xell");
        CHECK(rows[1].cls == Class::XeFg && rows[1].model == Model::M1 && !rows[1].witnessHonoured);
        CHECK(rows[0].refs == 2 && rows[1].refs == 3);
        CHECK(rows[1].companionFile == "libxell.dll" && rows[1].companionSize == 9012 && rows[1].companionStamp == 0x76543210u);
        CHECK(rows[1].ctx == "refused" && rows[1].orderB == "leaked" && rows[1].vramDeltaMb == -4);
        CHECK(!rows[0].pinFirst && !rows[1].pinFirst);

        // A broken line is an error, never a dropped row.
        std::vector<ParsedRow> brokenRows;
        CHECK(!ParseRows("class=fsrfg file=x.dll size=0 stamp=1 config=direct verdict=M2 witness=honoured\n", brokenRows,
                         error));
        CHECK(!ParseRows("class=dlsSG file=x.dll size=1 stamp=1 config=direct verdict=M2 witness=honoured\n", brokenRows,
                         error));
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 config=direct verdict=M3 witness=no refs=2\n", brokenRows, error));
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 config=direct verdict=M2 refs=2\n", brokenRows, error));
        // The configuration has to be the one the product runs for that class.
        CHECK(!ParseRows("class=xefg file=x.dll size=1 stamp=1 config=direct verdict=M2 witness=no refs=2\n", brokenRows, error));
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 verdict=M2 witness=no refs=2\n", brokenRows, error));
        // The standstill count is required, and an XeFG row without its companion build is not a row.
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 config=direct verdict=M2 witness=honoured\n", brokenRows, error));
        CHECK(!ParseRows("class=xefg file=x.dll size=1 stamp=1 config=xell verdict=M2 witness=honoured order=destroyfirst "
                         "refs=2\n",
                         brokenRows, error));
        // The order is required, must be one of the two words, and a pin-first row needs an M2 row with a witness.
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 config=direct verdict=M2 witness=honoured refs=2\n", brokenRows,
                         error));
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 config=direct verdict=M2 witness=honoured order=first refs=2\n",
                         brokenRows, error));
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 config=direct verdict=M2 witness=no order=pinfirst refs=2\n",
                         brokenRows, error));
        CHECK(!ParseRows("class=fsrfg file=x.dll size=1 stamp=1 config=direct verdict=M1 witness=honoured order=pinfirst refs=2\n",
                         brokenRows, error));
    }
    std::printf("    [ok] the parser reads two rows and refuses a bad class, size, verdict or missing field\n");

    // The real data file and the compiled table.
    const std::string data = ReadFile(path);
    std::vector<ParsedRow> rows;
    std::string error;
    if (!ParseRows(data, rows, error))
    {
        std::printf("FAIL: %s: %s\n", path, error.c_str());
        ++g_failed;
        return;
    }

    CHECK(Seeded().Count() == rows.size());
    for (size_t i = 0; i < rows.size() && i < Seeded().Count(); ++i)
    {
        const ModelRow& compiled = Seeded().At(i);
        CHECK(compiled.cls == rows[i].cls);
        CHECK(std::string_view(compiled.library.file) == rows[i].file);
        CHECK(compiled.library.size == rows[i].size);
        CHECK(compiled.library.stamp == rows[i].stamp);
        CHECK(std::string_view(compiled.config) == rows[i].config);
        CHECK(compiled.model.model == rows[i].model);
        CHECK(compiled.model.witnessHonoured == rows[i].witnessHonoured);
        CHECK(compiled.pinFirst == rows[i].pinFirst);
        CHECK(compiled.standstillRefs == rows[i].refs);
        CHECK(std::string_view(compiled.companion.file) == rows[i].companionFile);
        CHECK(compiled.companion.size == rows[i].companionSize);
        CHECK(compiled.companion.stamp == rows[i].companionStamp);
        CHECK(std::string_view(compiled.context) == rows[i].ctx);
        CHECK(std::string_view(compiled.orderB) == rows[i].orderB);
        CHECK(compiled.vramDeltaMb == rows[i].vramDeltaMb);
    }
    std::printf("    [ok] %zu row(s) in %s, %zu compiled in\n", rows.size(), path, Seeded().Count());
}
} // namespace

int main(int argc, char** argv)
{
    Classify();
    Identities();
    Table();
    ExpectedReferenceCount();
    if (argc > 1)
        DataFile(argv[1]);
    else
    {
        std::printf("FAIL: no data file path given\n");
        ++g_failed;
    }

    if (g_failed == 0)
        std::printf("ALL MODEL TABLE SCENARIOS PASS\n");
    else
        std::printf("%d CHECK(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
