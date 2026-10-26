// The pass/fail decisions of the patch-day CLI (tools/ffxiv-resolve/Verdict.h), on synthetic results: a DISAGREE cannot be produced on the
// real executable (its table is its own build), so the real-exe CLI run can never prove that --strict / --compare-table fail on one.
#include "resolve_test_util.h"

#include "Verdict.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <algorithm>

using namespace rt;
using namespace ext::ffxiv::resolve::offline;

namespace
{
SeamResult Seam(const char* id, SeamStatus status, Agreement agreement = Agreement::NotChecked)
{
    SeamResult s;
    s.id = id;
    s.status = status;
    s.hasValue = HasValue(status);
    s.agreement = agreement;
    return s;
}

bool Has(const std::vector<std::string>& v, const std::string& needle)
{
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

void TestStrict()
{
    ResolvedBuild rb;
    rb.seams = { Seam("a.one", SeamStatus::Found, Agreement::Agree), Seam("a.two", SeamStatus::Found) };
    CHECK(StrictFailures(rb).empty() && DisagreeingSeams(rb).empty());

    // every status that is not plain Found fails --strict
    const SeamStatus bad[] = { SeamStatus::FoundSingleAnchor, SeamStatus::NotFound, SeamStatus::Ambiguous, SeamStatus::Conflict, SeamStatus::EntryPatched };
    for (SeamStatus st : bad)
    {
        ResolvedBuild b;
        b.seams = { Seam("a.one", SeamStatus::Found), Seam("a.bad", st) };
        const auto f = StrictFailures(b);
        CHECK(f.size() == 1 && Has(f, "a.bad") && Has(f, StatusName(st)));
    }

    // a Found seam that DISAGREES with the table (the table won, the entry text equals the table) fails --strict and --compare-table
    ResolvedBuild d;
    d.seams = { Seam("a.one", SeamStatus::Found, Agreement::Agree), Seam("a.two", SeamStatus::Found, Agreement::Disagree) };
    const auto f = StrictFailures(d);
    CHECK(f.size() == 1 && Has(f, "a.two") && Has(f, "DISAGREES"));
    const auto dis = DisagreeingSeams(d);
    CHECK(dis.size() == 1 && dis[0] == "a.two");

    // a registry error fails --strict even when every seam looks fine
    ResolvedBuild e;
    e.seams = { Seam("a.one", SeamStatus::Found) };
    e.registryErrors = { "duplicate seam id 'x'" };
    CHECK(Has(StrictFailures(e), "registry error"));

    // a real result: the two statuses of a resolved synthetic image
    Registry r;
    AnchorSpec a;
    a.name = "a";
    a.pattern = "11 22 33 44";
    r.seams.push_back({ "t.a", SeamKind::Function, { a } });
    r.bindings = { { "charaView.accessorRva", "t.a", 0 } };
    auto img = MakeImage(0x6ad10bfd);
    Plant(*img, 0x2000, "11223344");
    GameBuild table;
    table.timeDateStamp = 0x6ad10bfd;
    table.sizeOfImage = kImageSize;
    table.charaView.accessorRva = 0x2000;
    ResolveOptions o;
    o.knownTable = &table;
    CHECK(StrictFailures(ResolveImage(*img, r, o)).empty());
    table.charaView.accessorRva = 0x2004; // the table disagrees
    const ResolvedBuild withTable = ResolveImage(*img, r, o);
    CHECK(StrictFailures(withTable).size() == 1 && DisagreeingSeams(withTable).size() == 1);
    // the entry of that run equals the table (the table won): comparing it would prove nothing, which is why the CLI compares a run WITHOUT the table
    CHECK(FormatEntry(withTable, r) == FormatTableEntry(table, r));
    const ResolvedBuild pure = ResolveImage(*img, r);
    CHECK(FormatEntry(pure, r) != FormatTableEntry(table, r) && DisagreeingSeams(pure).empty());
}

void TestStripComments()
{
    CHECK(StripEntryComments("a=0x1\nb=0x2 # single-anchor\nc=0x3 # via-weak-dependency\n# d unresolved: NotFound\n") ==
          "a=0x1\nb=0x2\nc=0x3\n# d unresolved: NotFound\n");
    CHECK(StripEntryComments("") == "" && StripEntryComments("x=1") == "x=1" && StripEntryComments("x=1 # c") == "x=1");
    CHECK(StripEntryComments("# whole line # with a second hash\n") == "# whole line # with a second hash\n"); // whole-line comments stay: an unresolved field must show
    // the real entry text round trip: marks removed, values untouched
    Registry r;
    AnchorSpec a;
    a.name = "a";
    a.pattern = "11 22 33 44";
    AnchorSpec b = a;
    b.name = "b";
    b.pattern = "77 77 77 77";
    r.seams.push_back({ "t.a", SeamKind::Function, { a, b } });
    r.bindings = { { "charaView.accessorRva", "t.a", 0 } };
    auto img = MakeImage(0x6ad10bfd);
    Plant(*img, 0x2000, "11223344");
    const ResolvedBuild rb = ResolveImage(*img, r);
    const std::string text = FormatEntry(rb, r);
    CHECK(text.find("# single-anchor") != std::string::npos);
    GameBuild table;
    table.timeDateStamp = 0x6ad10bfd;
    table.sizeOfImage = kImageSize;
    table.charaView.accessorRva = 0x2000;
    CHECK(StripEntryComments(text) == FormatTableEntry(table, r));
}
} // namespace

int main()
{
    TestStrict();
    TestStripComments();
    return TEST_RESULT("resolve_cli_verdict");
}
