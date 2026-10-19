#pragma once
// Text scanning shared by the menu tests: upstream's card table and tab list (OptiScaler/menu/menu_common.cpp) and the
// fork card registrations under OptiScaler/ext. The menu is ImGui glue and cannot run offline, so the tests read the
// source (comments stripped, string literals kept) and build the final card order from the same pure code the product uses.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace menuscan
{
inline std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Removes // and /* */ comments; string literals are kept as they are
inline std::string StripComments(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size();)
    {
        if (s.compare(i, 2, "//") == 0)
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
        }
        else if (s.compare(i, 2, "/*") == 0)
        {
            const size_t e = s.find("*/", i + 2);
            i = e == std::string::npos ? s.size() : e + 2;
        }
        else if (s[i] == '"')
        {
            out += s[i++];
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            if (i < s.size())
                out += s[i++];
        }
        else
            out += s[i++];
    }
    return out;
}

inline std::string Squash(const std::string& s)
{
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

inline size_t Count(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

inline std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

// "text" -> text; anything else -> empty
inline std::string Unquote(const std::string& s)
{
    const std::string t = Trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        return t.substr(1, t.size() - 2);
    return {};
}

// Index after the string literal that starts at s[i] (a quote)
inline size_t SkipString(const std::string& s, size_t i)
{
    ++i;
    while (i < s.size() && s[i] != '"')
        i += (s[i] == '\\' && i + 1 < s.size()) ? 2 : 1;
    return i + 1;
}

// The rows of a brace list: `open` is the index of its '{'; returns the text inside each top-level { ... } of the list
inline std::vector<std::string> ListRows(const std::string& code, size_t open)
{
    std::vector<std::string> rows;
    int depth = 0;
    size_t rowStart = 0;
    for (size_t i = open; i < code.size();)
    {
        const char c = code[i];
        if (c == '"')
        {
            i = SkipString(code, i);
            continue;
        }
        if (c == '{')
        {
            if (++depth == 2)
                rowStart = i + 1;
        }
        else if (c == '}')
        {
            if (depth == 2)
                rows.push_back(code.substr(rowStart, i - rowStart));
            if (--depth == 0)
                break;
        }
        ++i;
    }
    return rows;
}

// A row split at its top-level commas (outside strings and (), [], {})
inline std::vector<std::string> Fields(const std::string& row)
{
    std::vector<std::string> fields;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < row.size();)
    {
        const char c = row[i];
        if (c == '"')
        {
            i = SkipString(row, i);
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == ',' && depth == 0)
        {
            fields.push_back(Trim(row.substr(start, i - start)));
            start = i + 1;
        }
        ++i;
    }
    const std::string last = Trim(row.substr(start));
    if (!last.empty())
        fields.push_back(last);
    return fields;
}

// One box of upstream's card table (MenuCommon::GetMenuBoxes)
struct UpstreamBox
{
    std::string id;
    std::string tab;
    std::string source; // the shared draw function, empty for a box with its own draw
};

// `code` = comment-stripped menu_common.cpp. A box id that appears twice (the LOW_LATENCY_INPUTS branches) counts once.
inline std::vector<UpstreamBox> ParseUpstreamBoxes(const std::string& code)
{
    std::vector<UpstreamBox> boxes;
    const size_t decl = code.find("MenuBox boxes[]");
    if (decl == std::string::npos)
        return boxes;
    const size_t open = code.find('{', code.find('=', decl));

    for (const std::string& row : ListRows(code, open))
    {
        const auto f = Fields(row);
        if (f.size() < 4 || Unquote(f[0]).empty())
            continue;

        UpstreamBox box { Unquote(f[0]), Unquote(f[1]), {} };
        if (f[3] == "nullptr" && f.size() > 4 && f[4].rfind("Render", 0) == 0)
            box.source = f[4];

        const bool known = std::any_of(boxes.begin(), boxes.end(), [&](const UpstreamBox& b) { return b.id == box.id; });
        if (!known)
            boxes.push_back(box);
    }
    return boxes;
}

// One row of upstream's tab list (MenuCommon::RenderMainMenuTabs)
struct TabRow
{
    std::string group;
    std::string label;
    std::vector<std::string> fields; // group, label, description, draw, hideLabelInMenu, visible, displayLabel
};

inline std::vector<TabRow> ParseTabs(const std::string& code)
{
    std::vector<TabRow> tabs;
    const size_t decl = code.find("Tab tabs[]");
    if (decl == std::string::npos)
        return tabs;
    const size_t open = code.find('{', code.find('=', decl));

    for (const std::string& row : ListRows(code, open))
    {
        auto f = Fields(row);
        if (f.size() < 2)
            continue;
        tabs.push_back({ Unquote(f[0]), Unquote(f[1]), f });
    }
    return tabs;
}

// A fork card registration found in the ext sources
struct Registration
{
    std::string id;
    std::string tab;
    std::string file; // path relative to OptiScaler/ext
    // The card's `visible` argument (6th field of the Card: id, tab, name, draw, after, visible): "" when the registration
    // stops before it, "nullptr" when it is null, else the name of the predicate
    std::string visible;
    // The body of `bool <visible>(...) { ... }` in the same file, "" when it is not found
    std::string visibleBody;
};

inline bool IsWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The body (from its '{' to the matching '}') of `bool Name(...)` in comment-stripped `code`; "" if there is none
inline std::string BoolFunctionBody(const std::string& code, const std::string& name)
{
    if (name.empty())
        return {};
    for (size_t p = code.find(name); p != std::string::npos; p = code.find(name, p + 1))
    {
        const size_t e = p + name.size();
        if ((p > 0 && IsWordChar(code[p - 1])) || (e < code.size() && IsWordChar(code[e])))
            continue;
        size_t b = p;
        while (b > 0 && std::isspace(static_cast<unsigned char>(code[b - 1])))
            --b;
        if (b < 4 || code.compare(b - 4, 4, "bool") != 0 || (b > 4 && IsWordChar(code[b - 5])))
            continue;
        size_t q = e;
        while (q < code.size() && std::isspace(static_cast<unsigned char>(code[q])))
            ++q;
        if (q >= code.size() || code[q] != '(')
            continue;
        const size_t close = code.find(')', q);
        const size_t open = close == std::string::npos ? close : code.find('{', close);
        if (open == std::string::npos)
            continue;
        int depth = 0;
        for (size_t i = open; i < code.size();)
        {
            if (code[i] == '"')
            {
                i = SkipString(code, i);
                continue;
            }
            if (code[i] == '{')
                ++depth;
            else if (code[i] == '}' && --depth == 0)
                return code.substr(open, i - open + 1);
            ++i;
        }
    }
    return {};
}

// True when the card is registered with a visible predicate whose own body tests the game build. A card without the
// argument, with nullptr, or whose predicate does not look at ext::game::Current() draws wherever its tab is drawn.
inline bool TestsGameBuild(const Registration& r)
{
    return !r.visible.empty() && r.visible != "nullptr" && r.visibleBody.find("game::Current(") != std::string::npos;
}

// The registrations in one (comment-stripped) source text; `rel` is its path relative to OptiScaler/ext. `tokens` counts
// the registration tokens seen.
inline void ScanRegistrationText(const std::string& code, const std::string& rel, size_t& tokens, std::vector<Registration>& found)
{
    std::vector<size_t> starts;
    for (size_t p = code.find("menu::Register("); p != std::string::npos; p = code.find("menu::Register(", p + 1))
        starts.push_back(p + 15);
    for (size_t p = code.find("AutoRegister"); p != std::string::npos; p = code.find("AutoRegister", p + 1))
    {
        const bool wordStart = p == 0 || !IsWordChar(code[p - 1]);
        const bool wordEnd = p + 12 >= code.size() || !IsWordChar(code[p + 12]);
        if (wordStart && wordEnd)
            starts.push_back(p + 12);
    }

    for (size_t from : starts)
    {
        ++tokens;
        const size_t end = code.find(';', from);
        const size_t q1 = code.find('"', from);
        if (q1 == std::string::npos || q1 > end)
            continue;
        const size_t e1 = SkipString(code, q1);
        const size_t q2 = code.find('"', e1);
        if (q2 == std::string::npos || q2 > end)
            continue;
        const size_t e2 = SkipString(code, q2);

        Registration r { code.substr(q1 + 1, e1 - q1 - 2), code.substr(q2 + 1, e2 - q2 - 2), rel };

        // the Card's braces: from the '{' before the id to its match
        const size_t open = code.rfind('{', q1);
        if (open != std::string::npos)
        {
            int depth = 0;
            for (size_t i = open; i < code.size();)
            {
                if (code[i] == '"')
                {
                    i = SkipString(code, i);
                    continue;
                }
                if (code[i] == '{')
                    ++depth;
                else if (code[i] == '}' && --depth == 0)
                {
                    const auto fields = Fields(code.substr(open + 1, i - open - 1));
                    if (fields.size() > 5)
                        r.visible = fields[5];
                    break;
                }
                ++i;
            }
        }
        r.visibleBody = BoolFunctionBody(code, r.visible);
        found.push_back(r);
    }
}

// Scans OptiScaler/ext for `AutoRegister name { { "id", "tab", ...` and `menu::Register({ "id", "tab", ...`. `tokens` is
// the number of registration tokens seen (the definitions in core/ are skipped): it equals the result size when every
// token was understood.
inline std::vector<Registration> ScanRegistrations(const std::string& repo, size_t& tokens)
{
    namespace fs = std::filesystem;
    std::vector<Registration> found;
    tokens = 0;

    const fs::path root = fs::path(repo) / "OptiScaler" / "ext";
    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h")
            continue;

        const std::string rel = fs::relative(entry.path(), root).generic_string();
        if (rel == "core/MenuRegistry.h" || rel == "core/MenuRegistry.cpp" || rel == "core/MenuLayout.h" ||
            rel == "core/SourceTracker.h")
            continue;

        ScanRegistrationText(StripComments(ReadFile(entry.path().string())), rel, tokens, found);
    }
    std::sort(found.begin(), found.end(),
              [](const Registration& a, const Registration& b) { return a.id != b.id ? a.id < b.id : a.file < b.file; });
    return found;
}
} // namespace menuscan
