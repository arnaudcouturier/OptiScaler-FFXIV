// Ember menu style: the ImGui adapter (ext/menu/MenuStyleSeams.cpp) assigns every colour the library declares and every field of the
// palette. The palette test proves the data; this proves nothing is dropped between the data and the ImGuiStyle: a colour the
// adapter forgets keeps its classic, accent-tinted value, which is exactly the muddy look Ember replaces.
//
// Source-level, on the text with comments removed. The negative controls rerun each rule on a copy with one assignment removed.
#include "menu_scan.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

namespace
{
// The enumerators of `enum ImGuiCol_ { ... ImGuiCol_COUNT }`, without COUNT.
std::vector<std::string> LibraryColours(const std::string& imguiH)
{
    std::vector<std::string> names;
    // The definition, not the "-> enum ImGuiCol_" comment of the typedef above it: the occurrence followed by a brace.
    size_t start = imguiH.find("enum ImGuiCol_");
    while (start != std::string::npos && imguiH.find_first_not_of(" \t\r\n", start + 14) != imguiH.find('{', start + 14))
        start = imguiH.find("enum ImGuiCol_", start + 1);
    const size_t end = start == std::string::npos ? start : imguiH.find("ImGuiCol_COUNT", start);
    if (start == std::string::npos || end == std::string::npos)
        return names;
    const size_t brace = imguiH.find('{', start);
    const std::string body = menuscan::StripComments(imguiH.substr(brace, end - brace));
    for (size_t at = body.find("ImGuiCol_"); at != std::string::npos; at = body.find("ImGuiCol_", at + 1))
    {
        if (at > 0 && menuscan::IsIdentChar(body[at - 1]))
            continue;
        size_t stop = at;
        while (stop < body.size() && menuscan::IsIdentChar(body[stop]))
            ++stop;
        names.push_back(body.substr(at, stop - at));
    }
    return names;
}

// The member names of a struct body: `Rgba a, b, c;` and `float x[2], y;` lines -> a, b, c, x, y.
std::set<std::string> StructMembers(const std::string& header, const std::string& structName)
{
    std::set<std::string> members;
    // The checkout may use CRLF (core.autocrlf on Windows): read the header as LF text.
    std::string text = menuscan::StripComments(header);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    const size_t open = text.find("struct " + structName + "\n");
    if (open == std::string::npos)
        return members;
    const size_t brace = text.find('{', open);
    const size_t close = text.find("};", brace);
    const std::string body = text.substr(brace + 1, close - brace - 1);
    std::string statement;
    for (char c : body)
    {
        if (c != ';')
        {
            statement += c;
            continue;
        }
        // drop the type (first word), split the rest on commas, strip array brackets
        const size_t space = statement.find_first_not_of(" \t\r\n");
        const size_t typeEnd = statement.find_first_of(" \t", space);
        std::string rest = statement.substr(typeEnd);
        std::string name;
        for (char r : rest + ",")
        {
            if (r == ',')
            {
                if (!name.empty())
                    members.insert(name);
                name.clear();
            }
            else if (menuscan::IsIdentChar(r))
                name += r;
            else if (r == '[')
            {
                // skip the array size: the name is complete
                if (!name.empty())
                {
                    members.insert(name);
                    name.clear();
                }
                name = "";
            }
            else if (r == ']' || r == ' ' || r == '\t' || r == '\r' || r == '\n')
            {
                // array sizes (digits) were added to `name` only if they followed '[': they did not, see above
            }
        }
        statement.clear();
    }
    // A size inside [] was appended as a name by the loop above: remove purely numeric entries.
    for (auto it = members.begin(); it != members.end();)
        it = (!it->empty() && it->find_first_not_of("0123456789") == std::string::npos) ? members.erase(it) : std::next(it);
    return members;
}

bool Assigns(const std::string& cpp, const std::string& colour) { return cpp.find("[" + colour + "] =") != std::string::npos; }
bool Reads(const std::string& cpp, const std::string& prefix, const std::string& member)
{
    // `c.name)` / `m.name;` / `m.name)` : the member is read off the palette
    for (const char* tail : { ")", ";", "," })
        if (cpp.find(prefix + member + tail) != std::string::npos)
            return true;
    return false;
}

std::string Without(std::string text, const std::string& part)
{
    const size_t at = text.find(part);
    if (at != std::string::npos)
        text.erase(at, part.size());
    return text;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string imguiH = menuscan::ReadFile(repo + "/OptiScaler/include/imgui/imgui.h");
    const std::string adapter = menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/menu/MenuStyleSeams.cpp"));
    const std::string palette = menuscan::ReadFile(repo + "/OptiScaler/ext/menu/EmberPalette.h");

    const std::vector<std::string> colours = LibraryColours(imguiH);
    CHECK(colours.size() > 50); // the scan saw the enum (it would pass vacuously on an empty one)
    CHECK(!adapter.empty() && !palette.empty());

    // Every library colour is assigned by the adapter.
    for (const std::string& colour : colours)
    {
        if (!Assigns(adapter, colour))
            std::printf("not assigned: %s\n", colour.c_str());
        CHECK(Assigns(adapter, colour));
    }

    // Every palette member is read by the adapter.
    const std::set<std::string> colourMembers = StructMembers(palette, "EmberColors");
    const std::set<std::string> metricMembers = StructMembers(palette, "EmberMetrics");
    CHECK(colourMembers.size() >= colours.size() - 10);
    CHECK(metricMembers.size() >= 18);
    for (const std::string& member : colourMembers)
    {
        if (!Reads(adapter, "c.", member))
            std::printf("colour member not copied: %s\n", member.c_str());
        CHECK(Reads(adapter, "c.", member));
    }
    for (const std::string& member : metricMembers)
    {
        if (!Reads(adapter, "m.", member) && !Reads(adapter, "(m.", member))
            std::printf("metric member not copied: %s\n", member.c_str());
        CHECK(Reads(adapter, "m.", member));
    }

    // Negative controls: with one assignment removed the rules above would fail.
    CHECK(!Assigns(Without(adapter, "k[ImGuiCol_TabSelectedOverline] ="), "ImGuiCol_TabSelectedOverline"));
    CHECK(!Reads(Without(adapter, "c.checkMark"), "c.", "checkMark"));
    CHECK(!Reads(Without(adapter, "m.windowRounding"), "m.", "windowRounding"));

    return g_failed ? 1 : 0;
}
