// The product registry and the field table, with no executable:
//   * BuildRegistry() is sound (unique ids and binding keys, known dependencies, no cycles, bindings that fit their seam)
//   * FieldTable.h still describes GameBuild.h: the test reads GameBuild.h and fails when a member has no row or a row has no member, so a
//     reshaped table cannot drift silently (P3 changes GameBuild.h; this test tells it to regenerate the rows)
//   * the accessors of the field table read and write the real members
//   * an empty image resolves to NotFound everywhere and fills nothing
#include "resolve_test_util.h"

#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace rt;

namespace
{
struct Member
{
    std::string group;
    std::string name;
    bool isArray = false;
    std::uint32_t size = 0;
};

// The members of the five table structs, parsed from the text of GameBuild.h.
std::vector<Member> ParseGameBuild(const std::string& path, bool& ok)
{
    ok = false;
    std::vector<Member> out;
    std::ifstream in(path);
    if (!in)
        return out;
    const std::map<std::string, std::string> groups = { { "NativeQualityBuild", "nativeQuality" },
                                                        { "CompanionBuild", "companion" },
                                                        { "CharaViewBuild", "charaView" },
                                                        { "JitterBuild", "jitter" },
                                                        { "JitterSourceBuild", "jitterSource" } };
    std::string group;
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("struct ", 0) == 0)
        {
            std::istringstream is(line.substr(7));
            std::string name;
            is >> name;
            const auto it = groups.find(name);
            group = it == groups.end() ? std::string() : it->second;
            continue;
        }
        if (group.empty() || line.rfind("    ", 0) != 0 || (line.size() > 4 && line[4] == ' '))
            continue;
        const std::string body = line.substr(4);
        if (body.rfind("std::uint32_t ", 0) == 0)
        {
            std::istringstream is(body.substr(14));
            Member m;
            is >> m.name;
            m.group = group;
            m.size = 4;
            out.push_back(m);
        }
        else if (body.rfind("std::array<std::uint8_t, ", 0) == 0)
        {
            const std::size_t close = body.find('>');
            Member m;
            m.group = group;
            m.isArray = true;
            m.size = static_cast<std::uint32_t>(std::stoul(body.substr(25, close - 25)));
            std::istringstream is(body.substr(close + 1));
            is >> m.name;
            out.push_back(m);
        }
    }
    ok = !out.empty();
    return out;
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void TestProductRegistry()
{
    const Registry reg = BuildRegistry();
    const auto errors = ValidateRegistry(reg);
    for (const std::string& e : errors)
        std::printf("registry error: %s\n", e.c_str());
    CHECK(errors.empty());

    // CharaView is registered (P1's worked example); the companion and jitter skeletons register nothing until their P2 writers fill them
    CHECK(std::any_of(reg.seams.begin(), reg.seams.end(), [](const SeamSpec& s) { return std::string_view(s.id) == "chara.accessor"; }));

    // every seam id is "group.name", every anchor has a name and a parseable pattern (ValidateRegistry parsed them; this pins the style)
    for (const SeamSpec& s : reg.seams)
    {
        const std::string id = s.id;
        CHECK(id.find('.') != std::string::npos && id.find(' ') == std::string::npos);
        for (const AnchorSpec& a : s.anchors)
            CHECK(!a.name.empty());
    }

    // the three group files each add through their own function: calling one alone must not touch the seams of another
    Registry quality, companion, jitter;
    AddQualityAnchors(quality);
    AddCompanionAnchors(companion);
    AddJitterAnchors(jitter);
    CHECK(quality.seams.size() + companion.seams.size() + jitter.seams.size() == reg.seams.size());
    CHECK(quality.bindings.size() + companion.bindings.size() + jitter.bindings.size() == reg.bindings.size());
    // seam id prefixes keep three parallel writers from colliding: companion.*, jitterSource.*, everything else belongs to quality
    for (const SeamSpec& s : companion.seams)
        CHECK(std::string_view(s.id).rfind("companion.", 0) == 0);
    for (const SeamSpec& s : jitter.seams)
        CHECK(std::string_view(s.id).rfind("jitterSource.", 0) == 0);
    for (const SeamSpec& s : quality.seams)
        CHECK(std::string_view(s.id).rfind("companion.", 0) != 0 && std::string_view(s.id).rfind("jitterSource.", 0) != 0);

    // an empty image resolves to NotFound everywhere, fills no field and never throws
    auto empty = MakeImage();
    const ResolvedBuild rb = ResolveImage(*empty, reg);
    CHECK(rb.seams.size() == reg.seams.size() && rb.Count(SeamStatus::NotFound) == reg.seams.size() && rb.registryErrors.empty());
    CHECK(rb.build.charaView.accessorRva == 0 && rb.build.charaView.managerPointerRva == 0);
}

void TestFieldTable(const std::string& repo)
{
    // keys are unique, rows lie inside GameBuild, kinds follow the naming rule
    std::vector<std::string> keys;
    for (const FieldRow& row : kFieldTable)
    {
        char key[96];
        REQUIRE(FieldKey(row, key, sizeof(key)) != 0);
        CHECK(std::find(keys.begin(), keys.end(), key) == keys.end());
        keys.emplace_back(key);
        CHECK(row.offset + row.size <= sizeof(GameBuild));
        CHECK(FindField(key) == &row);
        const std::string name = row.name;
        if (row.isArray)
            CHECK(row.kind == (EndsWith(name, "Instructions") ? FieldKind::Guard : FieldKind::Pattern));
        else if (EndsWith(name, "Rva"))
            CHECK(row.kind == FieldKind::Rva);
        else if (EndsWith(name, "Offset") || EndsWith(name, "Offset0") || EndsWith(name, "Offset1"))
            CHECK(row.kind == FieldKind::Offset);
        else
            CHECK(row.kind == FieldKind::Constant);
    }
    CHECK(FindField("nope") == nullptr && FindField("nope.nothing") == nullptr && FindField("charaView.") == nullptr && FindField(".x") == nullptr);

    // the rows against the members of GameBuild.h, in both directions
    bool ok = false;
    const auto members = ParseGameBuild(repo + "/OptiScaler/ext/ffxiv/GameBuild.h", ok);
    REQUIRE(ok);
    CHECK(members.size() == kFieldCount);
    for (const Member& m : members)
    {
        const std::string key = m.group + "." + m.name;
        const FieldRow* row = FindField(key);
        if (row == nullptr)
        {
            std::printf("FAIL: GameBuild.h member %s has no row in FieldTable.h (regenerate the rows)\n", key.c_str());
            CHECK(row != nullptr);
            continue;
        }
        CHECK(row->isArray == m.isArray && row->size == m.size);
    }
    for (const FieldRow& row : kFieldTable)
    {
        const bool known = std::any_of(members.begin(), members.end(), [&](const Member& m) { return m.group == row.group && m.name == row.name; });
        if (!known)
            std::printf("FAIL: FieldTable.h row %s.%s names no member of GameBuild.h\n", row.group, row.name);
        CHECK(known);
    }

    // the accessors address the real members
    GameBuild b;
    *FieldU32(b, *FindField("charaView.accessorRva")) = 0x1234;
    *FieldU32(b, *FindField("jitterSource.deviceSizeRva")) = 0x5678;
    *FieldU32(b, *FindField("companion.nativeContextOffset")) = 0x9abc;
    CHECK(b.charaView.accessorRva == 0x1234 && b.jitterSource.deviceSizeRva == 0x5678 && b.companion.nativeContextOffset == 0x9abc);
    FieldBytes(b, *FindField("nativeQuality.querySizeInstructions"))[27] = 0x77;
    CHECK(b.nativeQuality.querySizeInstructions[27] == 0x77 && FindField("nativeQuality.querySizeInstructions")->size == 28);
    FieldBytes(b, *FindField("jitterSource.strengthInstructions"))[156] = 0x66;
    CHECK(b.jitterSource.strengthInstructions[156] == 0x66 && FindField("jitterSource.strengthInstructions")->size == 157);
    CHECK(FieldU32(b, *FindField("nativeQuality.querySizeInstructions")) == nullptr); // an array has no u32 accessor
    const GameBuild t;
    CHECK(*FieldU32(t, *FindField("charaView.targetsOffset")) == 0x2c8 && *FieldU32(t, *FindField("jitter.cameraGlobalRva")) == 0x290dba0);

    // counts: how many fields of each kind (a change here is a change of GameBuild.h and deserves a look)
    std::size_t rva = 0, guard = 0;
    for (const FieldRow& row : kFieldTable)
    {
        rva += row.kind == FieldKind::Rva ? 1 : 0;
        guard += row.kind == FieldKind::Guard ? 1 : 0;
    }
    CHECK(rva == 28 && guard == 14);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    TestProductRegistry();
    TestFieldTable(repo);
    return TEST_RESULT("resolve_registry");
}
