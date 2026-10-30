// The golden test over the real executable (analysis\patch-20261016\ffxiv_dx11.exe, build C, 52 MB, never committed).
//
// A loop over the product registry (BuildRegistry()): every seam the anchor groups register is resolved here too and compared, so this test
// grows without being edited. For every bound GameBuild field the value the resolver finds on the executable must equal the table:
// RVAs, globals, derived offsets and the guard bytes (read from the file at the resolved RVA). Also measured and printed: the whole
// resolve time on the full 52 MB image, and the offline uniqueness margin of every scanned anchor.
//
// If the executable is not there the test prints a line starting "SKIP:" and exits 0 (tools\test.ps1 has no skip state: the gate log of a
// writer must not contain "SKIP:"). The committed-bytes twin of this test is resolve_chara_b_fixture (no executable needed).
#include "resolve_test_util.h"

#include "Margin.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace rt;
using namespace ext::ffxiv::resolve::offline;

namespace
{
double MillisSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        out.push_back(line);
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string path = argc > 1 ? argv[1] : "ffxiv_dx11.exe";
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe)
        {
            std::printf("SKIP: exe not found: %s\n", path.c_str());
            return 0;
        }
    }

    const auto loadStart = std::chrono::steady_clock::now();
    std::string error;
    const auto image = DenseImage::FromFile(path, &error);
    if (!image)
    {
        std::printf("FAIL: cannot map %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    std::printf("loaded %s (SizeOfImage 0x%x) in %.0f ms\n", path.c_str(), image->SizeOfImage(), MillisSince(loadStart));

    const ext::ffxiv::GameBuild& table = ext::ffxiv::kCurrentBuild;
    if (image->TimeDateStamp() != table.timeDateStamp || image->SizeOfImage() != table.sizeOfImage)
    {
        // another build is not a golden test: the table describes build C only
        std::printf("SKIP: %s is not the table's build (stamp 0x%x, size 0x%x; the table has 0x%x / 0x%x)\n", path.c_str(), image->TimeDateStamp(),
                    image->SizeOfImage(), table.timeDateStamp, table.sizeOfImage);
        return 0;
    }

    const Registry registry = BuildRegistry();
    CHECK(ValidateRegistry(registry).empty());

    // The resolve time: best and median of five runs over the full image (the first one logs).
    LogCapture log;
    ResolveOptions options;
    options.knownTable = &table;
    options.sink = log.Sink();
    std::vector<double> wall;
    ResolvedBuild rb;
    for (int run = 0; run < 5; ++run)
    {
        const auto t0 = std::chrono::steady_clock::now();
        rb = ResolveImage(*image, registry, run == 0 ? options : ResolveOptions { &table, {} });
        wall.push_back(MillisSince(t0));
    }
    std::sort(wall.begin(), wall.end());
    for (const auto& l : log.lines)
        std::printf("    %s\n", l.second.c_str());
    std::printf("RESOLVE TIME on the %u MB image: best %.1f ms, median %.1f ms, worst %.1f ms (last run: scan %.1f ms, %zu seams, %zu anchors)\n",
                image->SizeOfImage() >> 20, wall.front(), wall[wall.size() / 2], wall.back(), static_cast<double>(rb.scanMicros) / 1000.0, rb.seams.size(),
                [&]
                {
                    std::size_t n = 0;
                    for (const SeamSpec& s : registry.seams)
                        n += s.anchors.size();
                    return n;
                }());
    CHECK(wall.back() < 10000.0); // a pathological scan, not a tuning target

    // 1. every registered seam resolved to exactly one value on the build the table describes, and agrees with the table (this run HAD the
    //    table, as the game does: a disagreement is visible in `agreement`, but the table value then overwrote the resolved one in rb.build)
    for (const SeamResult& s : rb.seams)
    {
        if (s.status != SeamStatus::Found)
            std::printf("FAIL: seam %s is %s: %s\n", s.id.c_str(), StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::Found);
        CHECK(s.agreement == Agreement::Agree || !s.hasTable); // a seam with a table binding must agree
        CHECK(s.agreement != Agreement::Disagree);
        CHECK(!s.fellBackToTable);
    }

    // Steps 2 and 3 compare the PURE pattern-search answer: a resolve WITHOUT the table. With the table, CrossCheck copies the table value
    // over every disagreeing field, so the entry and the fields of `rb` equal the table by construction and would test nothing.
    const ResolvedBuild pure = ResolveImage(*image, registry);
    CHECK(!pure.knownBuild);
    for (const SeamResult& s : pure.seams)
    {
        if (s.status != SeamStatus::Found)
            std::printf("FAIL: seam %s is %s without the table: %s\n", s.id.c_str(), StatusName(s.status), s.reason.c_str());
        CHECK(s.status == SeamStatus::Found);
    }

    // 2. the golden comparison: the generated entry equals the table text for every bound field
    const std::string got = FormatEntry(pure, registry), want = FormatTableEntry(table, registry);
    if (got != want)
    {
        const auto g = Lines(got), w = Lines(want);
        for (const std::string& line : w)
        {
            if (std::find(g.begin(), g.end(), line) == g.end())
                std::printf("FAIL: the table has    %s\n", line.c_str());
        }
        for (const std::string& line : g)
        {
            if (std::find(w.begin(), w.end(), line) == w.end())
                std::printf("FAIL: the resolver has %s\n", line.c_str());
        }
    }
    CHECK(got == want);
    std::printf("entry (%zu lines):\n%s", Lines(got).size(), got.c_str());

    // 3. field by field (RVAs, offsets and guard bytes), not only through the text
    for (const Binding& b : registry.bindings)
    {
        const FieldRow* row = FindField(b.key);
        if (row == nullptr)
        {
            CHECK(row != nullptr);
            continue;
        }
        if (row->kind == FieldKind::Guard)
            CHECK(std::memcmp(FieldBytes(pure.build, *row), FieldBytes(table, *row), row->size) == 0);
        else
            CHECK(*FieldU32(pure.build, *row) == *FieldU32(table, *row));
    }
    CHECK(pure.build.timeDateStamp == table.timeDateStamp && pure.build.sizeOfImage == table.sizeOfImage);

    // 4. guards: a table guard array equals the bytes of the executable at its table RVA (the existing pin, here for the bound ones)
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind != FieldKind::Guard)
            continue;
        // the RVA field of the same name: "...Instructions" -> "...Rva" (settingsInstructions has "settingsInstructionsRva")
        const std::string name = row.name;
        const std::string rvaName = name + "Rva";
        std::string alt = name.substr(0, name.size() - std::string("Instructions").size()) + "Rva";
        const FieldRow* rva = nullptr;
        for (const FieldRow& r : kFieldTable)
        {
            if (std::string(r.group) == row.group && r.kind == FieldKind::Rva && (rvaName == r.name || alt == r.name))
                rva = &r;
        }
        if (rva == nullptr)
            continue;
        std::vector<std::uint8_t> bytes(row.size);
        if (image->Read(*FieldU32(table, *rva), bytes.data(), bytes.size()))
            CHECK(std::memcmp(bytes.data(), FieldBytes(table, row), row.size) == 0);
    }

    // 5. the uniqueness margin of every scanned anchor, offline
    std::printf("margins (distance in fixed bytes to the nearest other site; 'low' = below the anchor's minMargin):\n");
    std::size_t waived = 0;
    for (std::size_t i = 0; i < registry.seams.size(); ++i)
    {
        const SeamSpec& seam = registry.seams[i];
        for (std::size_t a = 0; a < seam.anchors.size(); ++a)
        {
            const AnchorSpec& spec = seam.anchors[a];
            if (!spec.fromSeam.empty())
                continue;
            waived += spec.minMargin == 0 ? 1 : 0;
            const auto m = AnchorMargin(*image, spec, rb.seams[i].anchors[a]);
            if (m.found)
                std::printf("    %s/%s distance=%u at 0x%x (%u sites) required=%u%s\n", seam.id.c_str(), spec.name.c_str(), m.distance, m.rva, m.sitesAtDistance,
                            spec.minMargin, MarginOk(m, spec.minMargin) ? "" : "  LOW");
            else
                std::printf("    %s/%s distance>%u required=%u\n", seam.id.c_str(), spec.name.c_str(), kDefaultMarginCap, spec.minMargin);
            CHECK(MarginOk(m, spec.minMargin));
        }
    }

    std::printf("margin waived (minMargin 0, the predicates are the disambiguator): %zu anchor%s\n", waived, waived == 1 ? "" : "s");
    const auto unbound = UnboundFields(registry);
    std::printf("%zu GameBuild Rva/Guard fields are not bound to a seam yet (seam work shrinks this list; the gate must not read them from a resolved build)\n",
                unbound.size());
    return TEST_RESULT("resolve_golden_exe");
}
