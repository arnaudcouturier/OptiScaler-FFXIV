// The codegen of the known-build entry (tools/ffxiv-resolve/EmitGameBuild.h, `ffxiv-resolve --emit-gamebuild`), on committed text and committed bytes:
//   * idempotence: the committed GameBuild.h and a resolved build equal to its table produce the identical text,
//   * round trip: perturbed RVAs, an offset, a guard byte, the stamp and the size come back to the exact original text,
//   * scoping: two structs that spell a member alike (settingsPointerRva of NativeQualityBuild and of JitterSourceBuild) are rewritten apart,
//   * shape: array lengths and line counts are preserved, a header that does not fit the field table is an ERROR and produces nothing,
//   * the refuse path, --allow-partial and the marks, on a real resolve over a synthetic dense image with the seam removed,
//   * the previous build (B, bytes read from the B file) as a real second build,
//   * the partition: every field row is either rewritten (verified) or listed as not re-verified, never both, never neither.
// The command line (never writes the source, stdout/--out, exit codes) is run_ffxiv_resolve_cli.ps1's job.
#include "old_build_b_quality.h"

#include "EmitGameBuild.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

using namespace rt;
using namespace oldb;
using namespace ext::ffxiv::resolve::offline;

namespace
{
std::string g_repo = ".";
std::string g_header; // the committed GameBuild.h, byte for byte

bool ReadFile(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size())
    {
        std::size_t e = text.find('\n', pos);
        if (e == std::string::npos)
            e = text.size();
        out.push_back(text.substr(pos, e - pos));
        pos = e + 1;
    }
    return out;
}

// the lines of `b` that differ from the line of `a` at the same index (the two must have the same line count)
std::vector<std::pair<std::string, std::string>> DiffLines(const std::string& a, const std::string& b)
{
    std::vector<std::pair<std::string, std::string>> out;
    const auto la = SplitLines(a), lb = SplitLines(b);
    if (la.size() != lb.size())
    {
        out.emplace_back("(line count)", std::to_string(la.size()) + " vs " + std::to_string(lb.size()));
        return out;
    }
    for (std::size_t i = 0; i < la.size(); ++i)
    {
        if (la[i] != lb[i])
            out.emplace_back(la[i], lb[i]);
    }
    return out;
}

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// text equality that says WHERE two texts differ (the first few lines), so a failing round trip is readable
void CheckSame(const char* what, const std::string& got, const std::string& want)
{
    if (got == want)
        return;
    ++g_failures;
    std::printf("FAIL: %s differs\n", what);
    int shown = 0;
    for (const auto& d : DiffLines(want, got))
    {
        if (++shown > 6)
            break;
        std::printf("    want: %s\n    got:  %s\n", d.first.c_str(), d.second.c_str());
    }
}

// ---- test-side perturbation of the header text (naive string search on purpose: it must not share code with the parser under test) -------------

std::size_t StructBegin(const std::string& text, const std::string& type)
{
    const std::size_t s = text.find("struct " + type);
    if (s == std::string::npos)
    {
        std::printf("FAIL: struct %s not found in the header\n", type.c_str());
        ++g_failures;
    }
    return s;
}

std::size_t MemberAt(const std::string& text, const std::string& type, const std::string& member)
{
    const std::size_t s = StructBegin(text, type);
    if (s == std::string::npos)
        return s;
    const std::size_t e = text.find("\n};", s);
    for (std::size_t m = text.find(member, s); m != std::string::npos && m < e; m = text.find(member, m + 1))
    {
        const bool before = m == 0 || !(std::isalnum(static_cast<unsigned char>(text[m - 1])) || text[m - 1] == '_');
        const char next = text[m + member.size()];
        const bool after = !(std::isalnum(static_cast<unsigned char>(next)) || next == '_');
        if (before && after)
            return m;
    }
    std::printf("FAIL: member %s::%s not found in the header\n", type.c_str(), member.c_str());
    ++g_failures;
    return std::string::npos;
}

// `std::uint32_t member = <old>;` -> `<literal>`
std::string SetLiteral(std::string text, const std::string& type, const std::string& member, const std::string& literal)
{
    const std::size_t m = MemberAt(text, type, member);
    if (m == std::string::npos)
        return text;
    const std::size_t eq = text.find('=', m);
    std::size_t b = text.find_first_not_of(' ', eq + 1);
    const std::size_t e = text.find(';', b);
    text.replace(b, e - b, literal);
    return text;
}

// the `index`-th byte literal of a guard array
std::string SetGuardByte(std::string text, const std::string& type, const std::string& member, std::size_t index, const std::string& literal)
{
    const std::size_t m = MemberAt(text, type, member);
    if (m == std::string::npos)
        return text;
    std::size_t at = text.find('{', m);
    for (std::size_t i = 0; i <= index; ++i)
        at = text.find("0x", at + (i ? 1 : 0));
    text.replace(at, 4, literal);
    return text;
}

std::string LiteralOf(const std::string& text, const std::string& type, const std::string& member)
{
    const std::size_t m = MemberAt(text, type, member);
    if (m == std::string::npos)
        return "";
    const std::size_t eq = text.find('=', m);
    const std::size_t b = text.find_first_not_of(' ', eq + 1);
    return text.substr(b, text.find(';', b) - b);
}

// ---- a resolved build equal to the table: every seam the registry binds is plain Found ---------------------------------------------------------
ResolvedBuild TableBuild(const Registry& reg)
{
    ResolvedBuild rb;
    rb.build = ext::ffxiv::kCurrentBuild;
    rb.timeDateStamp = ext::ffxiv::kCurrentBuild.timeDateStamp;
    rb.sizeOfImage = ext::ffxiv::kCurrentBuild.sizeOfImage;
    for (const SeamSpec& spec : reg.seams) // every seam of the registry, bound or not (strengthFloor and ndcSignMask have no field)
    {
        SeamResult s;
        s.id = spec.id;
        s.status = SeamStatus::Found;
        s.hasValue = true;
        rb.seams.push_back(s);
    }
    return rb;
}

SeamResult* Seam(ResolvedBuild& rb, const std::string& id)
{
    for (SeamResult& s : rb.seams)
    {
        if (s.id == id)
            return &s;
    }
    std::printf("FAIL: no seam %s\n", id.c_str());
    ++g_failures;
    return nullptr;
}

std::string SeamOf(const Registry& reg, const std::string& key)
{
    for (const Binding& b : reg.bindings)
    {
        if (b.key == key)
            return b.seam;
    }
    std::printf("FAIL: no binding for %s\n", key.c_str());
    ++g_failures;
    return "";
}

std::set<std::string> KeysOf(const std::vector<ChangedField>& v)
{
    std::set<std::string> out;
    for (const auto& c : v)
        out.insert(c.key);
    return out;
}

// ---- tests ------------------------------------------------------------------------------------------------------------------------------------

void TestIdempotence()
{
    const Registry reg = BuildRegistry();
    const ResolvedBuild rb = TableBuild(reg);
    const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    CHECK(r.errors.empty() && r.unresolved.empty());
    CheckSame("the idempotent emit", r.text, g_header); // byte for byte, including the CRLF of the checkout
    CHECK(r.changed.empty() && r.marked.empty() && r.staleMentions.empty());
    CHECK(r.boundFields == reg.bindings.size() && r.confirmed.size() == reg.bindings.size());
    CHECK(r.boundFields > 45); // the 51 bindings of the registry (a registry that lost its bindings would pass idempotence trivially)
}

void TestRoundTrip()
{
    const Registry reg = BuildRegistry();
    const ResolvedBuild rb = TableBuild(reg);
    std::string h = g_header;
    // five literals, two guard bytes, the stamp and the size
    h = SetLiteral(h, "NativeQualityBuild", "querySizeRva", "0x352351");
    h = SetLiteral(h, "CharaViewBuild", "accessorRva", "0x26abd1"); // the header spells it 0x26abd0
    h = SetLiteral(h, "JitterSourceBuild", "settingsPointerRva", "0x290e471");
    h = SetLiteral(h, "JitterBuild", "cameraGlobalRva", "0x290dba1"); // the header spells it 0x290dba0
    h = SetLiteral(h, "NativeQualityBuild", "managerPointerRva", "0x290e499"); // the header spells it 0x290e498
    h = SetGuardByte(h, "JitterSourceBuild", "strengthInstructions", 100, "0xaa");
    h = SetGuardByte(h, "NativeQualityBuild", "settingsInstructions", 0, "0x00");
    h = SetLiteral(h, "GameBuild", "timeDateStamp", "0x6ac3dc96");
    h = SetLiteral(h, "GameBuild", "sizeOfImage", "0x380a000");
    CHECK(h != g_header);
    CHECK(DiffLines(g_header, h).size() == 9); // nine edited lines, nothing else (the guard bytes sit on different lines)

    const EmitResult r = EmitGameBuild(h, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    CheckSame("the restored header", r.text, g_header); // restored exactly, `0x08` and `0x00` included
    const std::set<std::string> want = { "nativeQuality.querySizeRva", "charaView.accessorRva", "jitterSource.settingsPointerRva", "nativeQuality.managerPointerRva",
                                         "jitter.cameraGlobalRva", "jitterSource.strengthInstructions",
                                         "nativeQuality.settingsInstructions", "timeDateStamp", "sizeOfImage" };
    CHECK(KeysOf(r.changed) == want);
    CHECK(r.confirmed.size() == reg.bindings.size() - 7); // seven bound fields changed; the stamp and the size are not bindings

    // a second pass over the restored text changes nothing
    const EmitResult again = EmitGameBuild(r.text, rb, reg, false);
    CHECK(again.outcome == EmitResult::Outcome::Emitted && again.text == g_header && again.changed.empty());
}

const char* TypeOfGroup(const std::string& group)
{
    if (group == "nativeQuality")
        return "NativeQualityBuild";
    if (group == "charaView")
        return "CharaViewBuild";
    if (group == "jitter")
        return "JitterBuild";
    if (group == "latency")
        return "LatencyBuild";
    return "JitterSourceBuild";
}

// EVERY bound field of the header wrong at once (each Rva/Offset literal, the first and last byte of each guard, the stamp and the size): one pass
// of the codegen brings back the committed text.
void TestEveryBoundFieldRestored()
{
    const Registry reg = BuildRegistry();
    std::string h = g_header;
    for (const Binding& b : reg.bindings)
    {
        const FieldRow* row = FindField(b.key);
        REQUIRE(row != nullptr);
        if (row->kind == FieldKind::Guard)
        {
            h = SetGuardByte(h, TypeOfGroup(row->group), row->name, 0, "0xee");
            h = SetGuardByte(h, TypeOfGroup(row->group), row->name, row->size - 1, "0xdd");
        }
        else
            h = SetLiteral(h, TypeOfGroup(row->group), row->name, "0x7ffffff");
    }
    h = SetLiteral(h, "GameBuild", "timeDateStamp", "0x1");
    h = SetLiteral(h, "GameBuild", "sizeOfImage", "0x2");
    CHECK(DiffLines(g_header, h).size() >= reg.bindings.size()); // at least one line per bound field (a guard counts once or twice)
    const EmitResult r = EmitGameBuild(h, TableBuild(reg), reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    CheckSame("the header after every bound field was perturbed", r.text, g_header);
    CHECK(r.changed.size() == reg.bindings.size() + 2 && r.confirmed.empty());
}

void TestScopedByStruct()
{
    // the same member spelling in two structs: the header's values are replaced by DIFFERENT resolved values, each in its own struct
    const Registry reg = BuildRegistry();
    ResolvedBuild rb = TableBuild(reg);
    rb.build.nativeQuality.settingsPointerRva = 0x1111111;
    rb.build.jitterSource.settingsPointerRva = 0x2222222;
    rb.build.nativeQuality.managerPointerRva = 0x3333333;
    rb.build.charaView.managerPointerRva = 0x4444444;
    rb.build.nativeQuality.settingsModeOffset = 0x77;
    rb.build.jitterSource.settingsModeOffset = 0x88;
    const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    CHECK(LiteralOf(r.text, "NativeQualityBuild", "settingsPointerRva") == "0x1111111");
    CHECK(LiteralOf(r.text, "JitterSourceBuild", "settingsPointerRva") == "0x2222222");
    CHECK(LiteralOf(r.text, "NativeQualityBuild", "managerPointerRva") == "0x3333333");
    CHECK(LiteralOf(r.text, "CharaViewBuild", "managerPointerRva") == "0x4444444");
    CHECK(LiteralOf(r.text, "NativeQualityBuild", "settingsModeOffset") == "0x77");
    CHECK(LiteralOf(r.text, "JitterSourceBuild", "settingsModeOffset") == "0x88");
    CHECK(DiffLines(g_header, r.text).size() == 6);
    const std::set<std::string> want = { "nativeQuality.settingsPointerRva", "jitterSource.settingsPointerRva", "nativeQuality.managerPointerRva",
                                         "charaView.managerPointerRva",      "nativeQuality.settingsModeOffset", "jitterSource.settingsModeOffset" };
    CHECK(KeysOf(r.changed) == want);

    // one perturbed field comes back alone: the other struct's line of the same name is not touched
    const std::string h = SetLiteral(g_header, "JitterSourceBuild", "settingsPointerRva", "0x5555555");
    const EmitResult back = EmitGameBuild(h, TableBuild(reg), reg, false);
    CHECK(back.outcome == EmitResult::Outcome::Emitted && back.text == g_header);
    CHECK(KeysOf(back.changed) == std::set<std::string>({ "jitterSource.settingsPointerRva" }));
}

void TestShapePreserved()
{
    const Registry reg = BuildRegistry();
    ResolvedBuild rb = TableBuild(reg);
    // every guard of the resolved build differs in its first and last byte
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind != FieldKind::Guard)
            continue;
        std::uint8_t* b = FieldBytes(rb.build, row);
        b[0] ^= 0x5a;
        b[row.size - 1] ^= 0xa5;
    }
    const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    CHECK(SplitLines(r.text).size() == SplitLines(g_header).size());
    CHECK(r.text.size() == g_header.size()); // `0x%02x` replaces `0x%02x`: a guard rewrite never changes the length
    // every std::array<std::uint8_t, N> keeps its N and its N literals
    std::size_t guards = 0;
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind != FieldKind::Guard)
            continue;
        ++guards;
        const std::string head = std::string("std::array<std::uint8_t, ") + std::to_string(row.size) + "> " + row.name;
        CHECK(Contains(r.text, head));
        const std::size_t at = r.text.find(head);
        REQUIRE(at != std::string::npos);
        const std::size_t open = r.text.find('{', at), close = r.text.find('}', open);
        std::size_t literals = 0;
        for (std::size_t p = r.text.find("0x", open); p != std::string::npos && p < close; p = r.text.find("0x", p + 1))
            ++literals;
        CHECK(literals == row.size);
    }
    CHECK(guards == 11); // 4 native quality, 7 jitter source
    // the first byte changed in every guard
    CHECK(Contains(r.text, std::string("0x") + "12")); // 0x48 ^ 0x5a, e.g. the first byte of querySizeInstructions
}

void TestHeaderDoesNotFit()
{
    const Registry reg = BuildRegistry();
    const ResolvedBuild rb = TableBuild(reg);

    // a guard with one literal missing: ERROR, no text, the key and the counts are named
    {
        std::string h = g_header;
        const std::size_t m = MemberAt(h, "NativeQualityBuild", "resizeCallbacksInstructions");
        const std::size_t first = h.find("0x", h.find('{', m));
        h.erase(first, h.find("0x", first + 1) - first);
        const EmitResult r = EmitGameBuild(h, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty());
        CHECK(!r.errors.empty() && Contains(r.errors[0], "nativeQuality.resizeCallbacksInstructions") && Contains(r.errors[0], "19 literals"));
    }
    // N in std::array<..., N> disagrees with the table
    {
        std::string h = g_header;
        const std::size_t at = h.find("std::array<std::uint8_t, 20> resizeCallbacksInstructions");
        REQUIRE(at != std::string::npos);
        h.replace(h.find("20", at), 2, "21");
        const EmitResult r = EmitGameBuild(h, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty() && !r.errors.empty() && Contains(r.errors[0], "N=21"));
    }
    // a bound member that is not in the header (renamed)
    {
        std::string h = g_header;
        const std::size_t at = h.find("querySizeRva");
        h.replace(at, 12, "querySizeRvx");
        const EmitResult r = EmitGameBuild(h, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty() && !r.errors.empty() && Contains(r.errors[0], "querySizeRva"));
    }
    // an initialiser that is an expression, not a literal
    {
        const std::string h = SetLiteral(g_header, "NativeQualityBuild", "rendererUpdateRva", "0x290e491 + 1");
        const EmitResult r = EmitGameBuild(h, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty() && !r.errors.empty() && Contains(r.errors[0], "nativeQuality.rendererUpdateRva"));
    }
    // a member declared twice in its struct
    {
        std::string h = g_header;
        const std::size_t at = h.find("    std::uint32_t querySizeRva");
        REQUIRE(at != std::string::npos);
        h.insert(at, "    std::uint32_t querySizeRva = 0x1;\n");
        const EmitResult r = EmitGameBuild(h, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty() && !r.errors.empty() && Contains(r.errors[0], "2 times"));
    }
    // a struct defined twice
    {
        const EmitResult r = EmitGameBuild(g_header + "\nstruct JitterBuild { std::uint32_t x = 1; };\n", rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty() && !r.errors.empty() && Contains(r.errors[0], "2 times"));
    }
    // a result without an image identity
    {
        ResolvedBuild empty = rb;
        empty.timeDateStamp = 0;
        empty.sizeOfImage = 0;
        const EmitResult r = EmitGameBuild(g_header, empty, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty() && !r.errors.empty() && Contains(r.errors[0], "image identity"));
    }
    // text with no structs at all
    {
        const EmitResult r = EmitGameBuild("// nothing here\n", rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Error && r.text.empty() && r.errors.size() > 45); // one error per binding that has no struct to read
    }
}

// Comments, strings and look-alike text must not be parsed; decimal and suffixed literals keep their spelling; the line ending is kept.
void TestLexer()
{
    Registry reg;
    reg.seams.push_back({ "t.a", SeamKind::Function, {} });
    reg.seams.push_back({ "t.b", SeamKind::Offset, {} });
    reg.bindings.push_back({ "nativeQuality.settingsPointerRva", "t.a", 0 });
    reg.bindings.push_back({ "nativeQuality.settingsModeOffset", "t.b", 0 });
    reg.seams.push_back({ "t.c", SeamKind::Offset, {} });
    reg.bindings.push_back({ "nativeQuality.settingsQualityOffset", "t.c", 0 });
    const std::string eol = "\r\n";
    const std::string mini =
        "#pragma once // struct NativeQualityBuild { std::uint32_t settingsPointerRva = 0x9; };" + eol +
        "// struct NativeQualityBuild { std::uint32_t settingsPointerRva = 0x1; }; (a comment, not a struct)" + eol +
        "/* struct NativeQualityBuild { std::uint32_t settingsPointerRva = 0x2; };" + eol +
        "   std::uint32_t settingsModeOffset = 0x3; ; } { */" + eol +
        "const char* kText = \"struct NativeQualityBuild { std::uint32_t settingsPointerRva = 0x4; };\";" + eol +
        "namespace ext::ffxiv {" + eol +
        "struct NativeQualityBuild" + eol +
        "{" + eol +
        "    std::uint32_t settingsPointerRva = 1000u; // decimal with a suffix; { ; } in a comment" + eol +
        "    std::uint32_t settingsModeOffset = 0x05;" + eol +
        "    std::uint32_t settingsQualityOffset = 0X000A; // an odd spelling of the value the resolver finds: not touched" + eol +
        "    std::uint32_t other = 7;" + eol +
        "};" + eol +
        "struct GameBuild" + eol +
        "{" + eol +
        "    std::uint32_t timeDateStamp = 0x6ad10bfd;" + eol +
        "    std::uint32_t sizeOfImage = 0x381f000;" + eol +
        "    NativeQualityBuild nativeQuality {};" + eol +
        "};" + eol +
        "}" + eol;
    ResolvedBuild rb;
    rb.timeDateStamp = 0x6ad10bfd;
    rb.sizeOfImage = 0x381f000;
    rb.build.nativeQuality.settingsPointerRva = 2000;
    rb.build.nativeQuality.settingsModeOffset = 0x0a;
    rb.build.nativeQuality.settingsQualityOffset = 0x0a;
    for (const char* id : { "t.a", "t.b", "t.c" })
    {
        SeamResult s;
        s.id = id;
        s.status = SeamStatus::Found;
        s.hasValue = true;
        rb.seams.push_back(s);
    }
    const EmitResult r = EmitGameBuild(mini, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    const auto diff = DiffLines(mini, r.text);
    CHECK(diff.size() == 2);
    REQUIRE(diff.size() == 2);
    CHECK(diff[0].second == "    std::uint32_t settingsPointerRva = 2000u; // decimal with a suffix; { ; } in a comment\r"); // decimal stays decimal, the suffix and the comment stay
    CHECK(diff[1].second == "    std::uint32_t settingsModeOffset = 0x0a;\r");
    CHECK(Contains(r.text, "settingsQualityOffset = 0X000A; // an odd spelling")); // an equal value keeps the header's own spelling
    CHECK(r.text.find("\r\n") != std::string::npos && r.text.find("\n") == r.text.find("\r\n") + 1); // CRLF kept
    // the look-alikes in comments and in the string are untouched
    CHECK(Contains(r.text, "settingsPointerRva = 0x9;") && Contains(r.text, "= 0x1; }") && Contains(r.text, "= 0x2;") && Contains(r.text, "= 0x4;"));

    // a trailing comment survives a mark and the mark is not stacked
    ResolvedBuild part = rb;
    part.seams[0].status = SeamStatus::NotFound;
    part.seams[0].hasValue = false;
    const EmitResult p1 = EmitGameBuild(mini, part, reg, true);
    REQUIRE(p1.outcome == EmitResult::Outcome::EmittedPartial);
    CHECK(Contains(p1.text, "settingsPointerRva = 1000u; // decimal with a suffix; { ; } in a comment // NOT re-verified for this build\r\n"));
    const EmitResult p2 = EmitGameBuild(p1.text, part, reg, true);
    CHECK(p2.text == p1.text);
    const EmitResult p3 = EmitGameBuild(p1.text, rb, reg, false); // later fully resolved: the stale mark is removed, the value written
    CHECK(p3.outcome == EmitResult::Outcome::Emitted && p3.text == r.text);

    // a second declaration on the same line cannot take a mark
    std::string crowded = mini;
    crowded.replace(crowded.find("= 0x05;"), 7, "= 0x05; std::uint32_t z = 1;");
    ResolvedBuild part2 = rb;
    part2.seams[1].status = SeamStatus::NotFound;
    part2.seams[1].hasValue = false;
    const EmitResult c = EmitGameBuild(crowded, part2, reg, true);
    CHECK(c.outcome == EmitResult::Outcome::Error && c.text.empty() && !c.errors.empty() && Contains(c.errors[0], "same line"));
    const EmitResult c2 = EmitGameBuild(crowded, rb, reg, false); // a plain Found value on that line needs no mark: fine
    CHECK(c2.outcome == EmitResult::Outcome::Emitted && Contains(c2.text, "settingsModeOffset = 0x0a; std::uint32_t z = 1;"));
}

void TestUnusableSeamsRefuse()
{
    const Registry reg = BuildRegistry();
    const SeamStatus bad[] = { SeamStatus::NotFound, SeamStatus::Ambiguous, SeamStatus::Conflict, SeamStatus::EntryPatched };
    for (SeamStatus st : bad)
    {
        ResolvedBuild rb = TableBuild(reg);
        const std::string id = SeamOf(reg, "nativeQuality.querySizeRva");
        SeamResult* s = Seam(rb, id);
        REQUIRE(s != nullptr);
        s->status = st;
        s->hasValue = HasValue(st);
        s->reason = "synthetic";
        const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Refused && r.text.empty());
        REQUIRE(!r.unresolved.empty());
        CHECK(r.unresolved[0].key == "nativeQuality.querySizeRva" && r.unresolved[0].seam == id && r.unresolved[0].status == StatusName(st));
        std::set<std::string> keys;
        for (const auto& u : r.unresolved)
            keys.insert(u.key);
        std::set<std::string> want;
        for (const Binding& b : reg.bindings)
        {
            if (b.seam == id)
                want.insert(b.key);
        }
        CHECK(keys == want); // every field the seam fills is unresolved, no other
        // with --allow-partial: the old text is kept, marked, and the report says so
        const EmitResult part = EmitGameBuild(g_header, rb, reg, true);
        CHECK(part.outcome == EmitResult::Outcome::EmittedPartial);
        const auto diff = DiffLines(g_header, part.text);
        CHECK(diff.size() == want.size());
        for (const auto& d : diff)
            CHECK(d.second == d.first + " // NOT re-verified for this build" || d.second == d.first.substr(0, d.first.size() - 1) + " // NOT re-verified for this build\r");
        CHECK(std::set<std::string>(part.marked.begin(), part.marked.end()) == want);
    }
    // the registry names a seam the result does not have: unresolved, status "missing"
    {
        ResolvedBuild rb = TableBuild(reg);
        const std::string id = SeamOf(reg, "nativeQuality.querySizeRva");
        rb.seams.erase(std::remove_if(rb.seams.begin(), rb.seams.end(), [&](const SeamResult& s) { return s.id == id; }), rb.seams.end());
        const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Refused && !r.unresolved.empty() && r.unresolved[0].status == "missing");
    }
}

void TestMarksForWeakValues()
{
    const Registry reg = BuildRegistry();
    for (int variant = 0; variant < 2; ++variant)
    {
        ResolvedBuild rb = TableBuild(reg);
        const std::string id = SeamOf(reg, "jitterSource.settingsPointerRva");
        SeamResult* s = Seam(rb, id);
        REQUIRE(s != nullptr);
        s->status = SeamStatus::FoundSingleAnchor;
        s->viaWeakDependency = variant == 1;
        const EmitResult r = EmitGameBuild(g_header, rb, reg, false); // usable: no refusal, the value is written and marked
        REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
        const auto diff = DiffLines(g_header, r.text);
        REQUIRE(diff.size() == 1);
        const char* mark = variant == 1 ? kMarkWeakDependency : kMarkSingleAnchor;
        CHECK(Contains(diff[0].second, "settingsPointerRva") && Contains(diff[0].second, std::string(" ") + mark));
        CHECK(r.marked.size() == 1 && r.marked[0] == "jitterSource.settingsPointerRva");
        // a second pass keeps one mark
        const EmitResult again = EmitGameBuild(r.text, rb, reg, false);
        CHECK(again.text == r.text);
        // once plain Found again the mark goes
        const EmitResult clean = EmitGameBuild(r.text, TableBuild(reg), reg, false);
        CHECK(clean.text == g_header);
    }
}

// A real resolve over a synthetic dense image: the CharaView seams only (the image holds that one accessor row).
Registry CharaOnly()
{
    Registry reg = BuildRegistry();
    reg.seams.erase(std::remove_if(reg.seams.begin(), reg.seams.end(), [](const SeamSpec& sp) { return sp.id.rfind("chara.", 0) != 0; }), reg.seams.end());
    reg.bindings.erase(std::remove_if(reg.bindings.begin(), reg.bindings.end(), [](const Binding& b) { return b.key.rfind("charaView.", 0) != 0; }),
                       reg.bindings.end());
    reg.mustDiffer.clear();
    return reg;
}

void TestRealResolveRefuseAndPartial()
{
    const Registry reg = CharaOnly();
    REQUIRE(ValidateRegistry(reg).empty());
    const std::size_t bound = reg.bindings.size();
    CHECK(bound >= 2);

    // the accessor row removed: the seams are NotFound
    {
        auto img = MakeCImage();
        REQUIRE(img != nullptr);
        const ResolvedBuild rb = ResolveImage(*img, reg);
        CHECK(rb.Find("chara.accessor") != nullptr && rb.Find("chara.accessor")->status == SeamStatus::NotFound);
        const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Refused && r.text.empty() && r.unresolved.size() == bound);
        const EmitResult part = EmitGameBuild(g_header, rb, reg, true);
        REQUIRE(part.outcome == EmitResult::Outcome::EmittedPartial);
        const auto diff = DiffLines(g_header, part.text);
        CHECK(diff.size() == bound); // the marked lines; the stamp and size of the synthetic image equal the header's
        for (const auto& d : diff)
            CHECK(Contains(d.second, "// NOT re-verified for this build") && (Contains(d.second, "accessorRva") || Contains(d.second, "managerPointerRva") || Contains(d.second, "targetsOffset")));
        CHECK(part.marked.size() == bound);
        // the old values were not replaced by zeros
        CHECK(Contains(part.text, "accessorRva = 0x26abd0;") && Contains(part.text, "managerPointerRva = 0x290e490;")); // the old values stay
    }
    // the same image with the accessor planted: found, idempotent against the header
    {
        auto img = MakeCImage();
        REQUIRE(img != nullptr);
        CHECK(PlantRow(*img, kCharaAccessorC));
        const ResolvedBuild rb = ResolveImage(*img, reg);
        const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
        CHECK(r.outcome == EmitResult::Outcome::Emitted && r.text == g_header && r.changed.empty());
    }
}

void TestOldBuildBIsARealSecondBuild()
{
    const Registry reg = CharaOnly();
    auto img = MakeBImage();
    REQUIRE(img != nullptr);
    CHECK(PlantBRow(*img, kCharaAccessorB));
    const ResolvedBuild rb = ResolveImage(*img, reg);
    const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    CHECK(KeysOf(r.changed) == std::set<std::string>({ "charaView.accessorRva", "charaView.managerPointerRva", "timeDateStamp", "sizeOfImage" }));
    CHECK(LiteralOf(r.text, "CharaViewBuild", "accessorRva") == "0x26ab40");
    CHECK(LiteralOf(r.text, "CharaViewBuild", "managerPointerRva") == "0x28f9490");
    CHECK(LiteralOf(r.text, "GameBuild", "timeDateStamp") == "0x6ac3dc96");
    CHECK(LiteralOf(r.text, "GameBuild", "sizeOfImage") == "0x380a000");
    CHECK(DiffLines(g_header, r.text).size() == 4);
    // the rest of the header is the input text: the other groups' lines are untouched
    CHECK(LiteralOf(r.text, "NativeQualityBuild", "managerPointerRva") == "0x290e498");
    // the report for the maintainer names every change
    const std::string rep = FormatEmitReport(r, rb, false);
    CHECK(Contains(rep, "charaView.accessorRva: 0x26abd0 -> 0x26ab40") && Contains(rep, "timeDateStamp: 0x6ad10bfd -> 0x6ac3dc96"));
    // the comment of CharaViewBuild quotes no RVA, but the JitterBuild one quotes the camera update's old value only for changed fields: none here
    // (a changed field whose old value a comment quotes is the stale-comment case, tested below)
}

void TestStaleComment()
{
    const Registry reg = BuildRegistry();
    ResolvedBuild rb = TableBuild(reg);
    rb.build.jitterSource.generatorRva = 0x2ebd91; // the header's comments quote 0x2ebd90 (the generator)
    const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    bool found = false;
    for (const std::string& m : r.staleMentions)
        found = found || (Contains(m, "0x2ebd90") && Contains(m, "jitterSource.generatorRva"));
    CHECK(found);
    CHECK(Contains(FormatEmitReport(r, rb, false), "stale comment: line "));

    // a short old value (0x4c is quoted in the same comments, e.g. `settings+0x4c`) is NOT reported: that would be noise on every run
    ResolvedBuild shortValue = TableBuild(reg); // not `small`: windows headers define that as a macro
    shortValue.build.jitterSource.settingsScaleOffset = 0x4d;
    const EmitResult rs = EmitGameBuild(g_header, shortValue, reg, false);
    REQUIRE(rs.outcome == EmitResult::Outcome::Emitted);
    CHECK(KeysOf(rs.changed) == std::set<std::string>({ "jitterSource.settingsScaleOffset" }) && rs.staleMentions.empty());
}

void TestPartition()
{
    const Registry reg = BuildRegistry();
    ResolvedBuild rb = TableBuild(reg);
    EmitResult r = EmitGameBuild(g_header, rb, reg, false);
    REQUIRE(r.outcome == EmitResult::Outcome::Emitted);
    std::set<std::string> bound;
    for (const Binding& b : reg.bindings)
        bound.insert(b.key);
    std::set<std::string> notVerified;
    for (const NotVerifiedField& v : r.notVerified)
    {
        CHECK(notVerified.insert(v.key).second); // no row twice
        CHECK(bound.count(v.key) == 0);          // never a verified one
    }
    std::size_t rows = 0;
    for (const FieldRow& row : kFieldTable)
    {
        char key[96];
        REQUIRE(FieldKey(row, key, sizeof(key)) != 0);
        ++rows;
        CHECK((bound.count(key) != 0) != (notVerified.count(key) != 0)); // exactly one side
    }
    CHECK(rows == kFieldCount && bound.size() + notVerified.size() == kFieldCount);
    // every Rva/Guard row no binding covers is listed (UnboundFields), every Offset row without a binding, every Constant and Pattern row
    for (const std::string& key : UnboundFields(reg))
        CHECK(notVerified.count(key) == 1);
    std::size_t constants = 0, patterns = 0;
    for (const NotVerifiedField& v : r.notVerified)
    {
        constants += v.kind == "Constant" ? 1 : 0;
        patterns += v.kind == "Pattern" ? 1 : 0;
        CHECK(v.value != "(not in the header)"); // every table row exists in the committed header
    }
    CHECK(patterns == 2);
    std::size_t wantConstants = 0;
    for (const FieldRow& row : kFieldTable)
        wantConstants += row.kind == FieldKind::Constant ? 1 : 0;
    CHECK(constants == wantConstants);

    // an unresolved bound field moves from the verified side to the not-verified side
    SeamResult* s = Seam(rb, SeamOf(reg, "jitterSource.settingsScaleOffset"));
    REQUIRE(s != nullptr);
    s->status = SeamStatus::Conflict;
    r = EmitGameBuild(g_header, rb, reg, true);
    std::set<std::string> nv2;
    for (const NotVerifiedField& v : r.notVerified)
        nv2.insert(v.key);
    CHECK(nv2.count("jitterSource.settingsScaleOffset") == 1 && nv2.size() == notVerified.size() + 1);
    for (const NotVerifiedField& v : r.notVerified)
    {
        if (v.key == "jitterSource.settingsScaleOffset")
            CHECK(Contains(v.why, "did not resolve") && v.value == "0x4c");
    }
    // the two constants that a seam checks say so
    for (const NotVerifiedField& v : r.notVerified)
    {
        if (v.key == "jitterSource.strengthFloorBits")
            CHECK(Contains(v.why, "jitterSource.strengthFloor is Found"));
    }
}

void TestReportText()
{
    const Registry reg = BuildRegistry();
    ResolvedBuild rb = TableBuild(reg);
    SeamResult* s = Seam(rb, SeamOf(reg, "jitterSource.settingsPointerRva"));
    REQUIRE(s != nullptr);
    s->status = SeamStatus::NotFound;
    const EmitResult r = EmitGameBuild(g_header, rb, reg, false);
    const std::string rep = FormatEmitReport(r, rb, false);
    CHECK(Contains(rep, "REFUSED"));
    CHECK(Contains(rep, "== NOT SELF-HEALING / NOT RE-VERIFIED =="));
    CHECK(Contains(rep, "== SHADER CATALOGS: NOT VERIFIABLE OFFLINE =="));
    for (std::size_t c = 0; c < ext::ffxiv::catalog::kCatalogCount; ++c) // the catalog sizes come from the table, not from a literal here
        CHECK(Contains(rep, std::string(ext::ffxiv::catalog::kInfo[c].name) + " n/" + std::to_string(ext::ffxiv::catalog::kInfo[c].count)));
    CHECK(Contains(rep, "UI 6/7 and 'UI boundary ok'"));
    CHECK(Contains(rep, "jitterSource.settingsPointerRva  <- seam jitterSource.settingsPointer: NotFound"));
    CHECK(Contains(rep, "-- Offset fields (") && Contains(rep, "-- Constant fields (") && Contains(rep, "-- Pattern fields (2) --"));
    CHECK(Contains(rep, "-- seam status table"));
    // every not-re-verified key appears with its value
    for (const NotVerifiedField& v : r.notVerified)
        CHECK(Contains(rep, "  " + v.key + " = " + v.value));
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
        g_repo = argv[1];
    if (!ReadFile(g_repo + "/OptiScaler/ext/ffxiv/GameBuild.h", g_header) || g_header.size() < 5000)
    {
        std::printf("FAIL: cannot read %s/OptiScaler/ext/ffxiv/GameBuild.h\n", g_repo.c_str());
        return 1;
    }
    TestIdempotence();
    TestRoundTrip();
    TestEveryBoundFieldRestored();
    TestScopedByStruct();
    TestShapePreserved();
    TestHeaderDoesNotFit();
    TestLexer();
    TestUnusableSeamsRefuse();
    TestMarksForWeakValues();
    TestRealResolveRefuseAndPartial();
    TestOldBuildBIsARealSecondBuild();
    TestStaleComment();
    TestPartition();
    TestReportText();
    return TEST_RESULT("resolve_emit_gamebuild");
}
