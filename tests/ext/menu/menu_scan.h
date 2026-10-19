#pragma once
// Text scanning shared by the menu tests: upstream's card table and tab list (OptiScaler/menu/menu_common.cpp) and the
// fork card registrations under OptiScaler/ext. The menu is ImGui glue and cannot run offline, so the tests read the
// source (comments stripped, string literals kept) and build the final card order from the same pure code the product uses.
#include <algorithm>
#include <cctype>
#include <cstdlib>
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

// True when `c` can be part of an identifier
inline bool IsIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Removes // and /* */ comments; string and character literals are kept as they are (a '"' or a '//' inside a literal does
// not start anything). Raw string literals (R"delim( ... )delim") are NOT supported: `rawString` is set and the output is
// not reliable, so a test that scans such a file must fail instead of reading mis-stripped text.
inline std::string StripCommentsChecked(const std::string& s, bool& rawString)
{
    rawString = false;
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
            // R"delim(  : a raw string (prefixes u8R, uR, UR, LR end in R as well)
            if (i > 0 && s[i - 1] == 'R' && (i < 2 || !IsIdentChar(s[i - 2]) || s[i - 2] == '8' || s[i - 2] == 'u' ||
                                             s[i - 2] == 'U' || s[i - 2] == 'L'))
                rawString = true;
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
        else if (s[i] == '\'' && !(i > 0 && std::isxdigit(static_cast<unsigned char>(s[i - 1])) && i + 1 < s.size() &&
                                   std::isxdigit(static_cast<unsigned char>(s[i + 1]))))
        {
            // a character literal: '"', '\'', '\', '{'. (A digit separator such as 1'000 sits between two hex digits.)
            out += s[i++];
            while (i < s.size() && s[i] != '\'' && s[i] != '\n')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            if (i < s.size() && s[i] == '\'')
                out += s[i++];
        }
        else
            out += s[i++];
    }
    return out;
}

// Removes the lines of `#if 0` ... `#endif` blocks (an `#else`/`#elif` branch of an `#if 0` is live code again). Run after
// StripComments. Only the plain `#if 0` form is understood.
inline std::string StripIfZero(const std::string& s)
{
    std::string out;
    std::istringstream in(s);
    std::string line;
    int mode = 0; // 0 = live, 1 = inside the dead branch of an `#if 0`, 2 = inside its `#else` branch (live)
    int nest = 0; // #if depth inside mode 1 or 2
    while (std::getline(in, line))
    {
        size_t b = 0;
        while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b])))
            ++b;
        std::string d = b < line.size() && line[b] == '#' ? line.substr(b + 1) : std::string();
        while (!d.empty() && std::isspace(static_cast<unsigned char>(d.front())))
            d.erase(d.begin());
        const bool isIf = d.rfind("if", 0) == 0; // if, ifdef, ifndef
        const bool isEndif = d.rfind("endif", 0) == 0;
        const bool isElse = d.rfind("else", 0) == 0 || d.rfind("elif", 0) == 0;
        const bool isZero = d.rfind("if 0", 0) == 0 && (d.size() == 4 || !IsIdentChar(d[4]));
        if (mode == 0)
        {
            if (isZero)
            {
                mode = 1;
                nest = 0;
                continue;
            }
            out += line + "\n";
        }
        else if (mode == 1)
        {
            if (isIf)
                ++nest;
            else if (isEndif)
            {
                if (nest > 0)
                    --nest;
                else
                    mode = 0;
            }
            else if (isElse && nest == 0)
                mode = 2;
        }
        else // mode 2: the live `#else` branch; its `#else` and `#endif` lines are dropped, nested ones are kept
        {
            if (isIf)
                ++nest;
            if (isEndif && nest == 0)
                mode = 0;
            else if (!(isElse && nest == 0))
            {
                out += line + "\n";
                if (isEndif && nest > 0)
                    --nest;
            }
        }
    }
    return out;
}

// StripComments for the tests: raw string literals stop the test run (it prints which text and exits), so a file that has
// one can never be read mis-stripped. `#if 0` blocks are removed as well.
inline std::string StripComments(const std::string& s)
{
    bool raw = false;
    std::string out = StripCommentsChecked(s, raw);
    if (raw)
    {
        std::printf("FAIL menu_scan: a raw string literal (R\"...\") in a scanned file is not supported by StripComments\n");
        std::exit(3);
    }
    return StripIfZero(out);
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
    // The picker name: the third field of the Card ("" when it is not a plain string literal)
    std::string name;
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
                    if (fields.size() > 2)
                        r.name = Unquote(fields[2]);
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

// ---------------------------------------------------------------------------------------------------------------------
// Function bodies and ImGui stack balance (text based; the menu is ImGui glue and cannot run offline). All of this works on
// comment-stripped code that still has its blanks (StripComments output), not on Squash output.
// ---------------------------------------------------------------------------------------------------------------------

// Index after the character literal that starts at s[i] (a single quote)
inline size_t SkipChar(const std::string& s, size_t i)
{
    ++i;
    while (i < s.size() && s[i] != '\'' && s[i] != '\n')
        i += (s[i] == '\\' && i + 1 < s.size()) ? 2 : 1;
    return i + 1;
}

// True when s[i] (a single quote) starts a character literal and not a digit separator (1'000)
inline bool StartsCharLiteral(const std::string& s, size_t i)
{
    return !(i > 0 && std::isxdigit(static_cast<unsigned char>(s[i - 1])) && i + 1 < s.size() &&
             std::isxdigit(static_cast<unsigned char>(s[i + 1])));
}

// Index of the '}' matching the '{' at `open` (strings and character literals skipped); npos when it never closes
inline size_t MatchBrace(const std::string& s, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < s.size();)
    {
        if (s[i] == '"')
        {
            i = SkipString(s, i);
            continue;
        }
        if (s[i] == '\'' && StartsCharLiteral(s, i))
        {
            i = SkipChar(s, i);
            continue;
        }
        if (s[i] == '{')
            ++depth;
        else if (s[i] == '}' && --depth == 0)
            return i;
        ++i;
    }
    return std::string::npos;
}

struct FuncBody
{
    std::string name; // unqualified; "" for a lambda assigned at namespace scope
    size_t open = 0;  // index of the body's '{'
    size_t close = 0; // index of the matching '}'
};

// The outermost function definitions in `code` (namespace, class and struct scopes are entered, function bodies are not).
// Data braces (`T x { ... };`, lists) are skipped.
inline std::vector<FuncBody> FunctionBodies(const std::string& code)
{
    std::vector<FuncBody> out;
    size_t headerStart = 0;
    for (size_t i = 0; i < code.size();)
    {
        const char c = code[i];
        if (c == '"')
        {
            i = SkipString(code, i);
            continue;
        }
        if (c == '\'' && StartsCharLiteral(code, i))
        {
            i = SkipChar(code, i);
            continue;
        }
        if (c == ';' || c == '}')
        {
            headerStart = ++i;
            continue;
        }
        if (c != '{')
        {
            ++i;
            continue;
        }

        // the statement head before this '{', without preprocessor lines
        std::string head;
        {
            std::istringstream in(code.substr(headerStart, i - headerStart));
            std::string line;
            while (std::getline(in, line))
            {
                const std::string t = Trim(line);
                if (!t.empty() && t[0] != '#')
                    head += t + " ";
            }
        }
        head = Trim(head);

        const size_t close = MatchBrace(code, i);
        if (close == std::string::npos)
            break;

        bool container = false;
        if (head.find('(') == std::string::npos)
            for (const char* kw : { "namespace", "struct", "class", "enum", "union", "extern" })
            {
                const size_t k = head.find(kw);
                const size_t e = k + std::string(kw).size();
                if (k != std::string::npos && (k == 0 || !IsIdentChar(head[k - 1])) && (e >= head.size() || !IsIdentChar(head[e])))
                    container = true;
            }
        if (container)
        {
            headerStart = ++i; // enter the scope; its closing '}' resets the head
            continue;
        }

        // trailing qualifiers, then the closing ')' of the parameter list
        std::string h = head;
        for (bool again = true; again;)
        {
            again = false;
            for (const char* q : { "const", "noexcept", "override", "final", "mutable" })
            {
                const std::string qs = q;
                if (h.size() >= qs.size() && h.compare(h.size() - qs.size(), qs.size(), qs) == 0 &&
                    (h.size() == qs.size() || !IsIdentChar(h[h.size() - qs.size() - 1])))
                {
                    h = Trim(h.substr(0, h.size() - qs.size()));
                    again = true;
                }
            }
        }
        if (!h.empty() && h.back() == ')')
        {
            int depth = 0;
            size_t p = h.size();
            while (p > 0)
            {
                --p;
                if (h[p] == ')')
                    ++depth;
                else if (h[p] == '(' && --depth == 0)
                    break;
            }
            size_t e = p;
            while (e > 0 && std::isspace(static_cast<unsigned char>(h[e - 1])))
                --e;
            size_t b = e;
            while (b > 0 && (IsIdentChar(h[b - 1]) || h[b - 1] == '~'))
                --b;
            out.push_back({ h.substr(b, e - b), i, close });
        }
        i = close + 1;
        headerStart = i;
    }
    return out;
}

// The body (from '{' to '}') of the one function called `name`; "" when there is none or more than one
inline std::string FunctionBody(const std::string& code, const std::string& name)
{
    std::string found;
    int n = 0;
    for (const FuncBody& f : FunctionBodies(code))
        if (f.name == name)
        {
            found = code.substr(f.open, f.close - f.open + 1);
            ++n;
        }
    return n == 1 ? found : std::string {};
}

// Names called (`name(`) in `body`, outside strings and character literals
inline std::vector<std::string> CalledNames(const std::string& body)
{
    std::vector<std::string> names;
    for (size_t i = 0; i < body.size();)
    {
        if (body[i] == '"')
        {
            i = SkipString(body, i);
            continue;
        }
        if (body[i] == '\'' && StartsCharLiteral(body, i))
        {
            i = SkipChar(body, i);
            continue;
        }
        if (IsIdentChar(body[i]) && !std::isdigit(static_cast<unsigned char>(body[i])))
        {
            size_t e = i;
            while (e < body.size() && IsIdentChar(body[e]))
                ++e;
            size_t q = e;
            while (q < body.size() && std::isspace(static_cast<unsigned char>(body[q])))
                ++q;
            if (q < body.size() && body[q] == '(')
                names.push_back(body.substr(i, e - i));
            i = e;
            continue;
        }
        ++i;
    }
    return names;
}

// True when `target` is `entry` or is called, directly or through other functions of the same file, from `entry`. A function
// that nothing reaches (dead code) is not reachable, so a widget in it does not count.
inline bool Reaches(const std::string& code, const std::string& entry, const std::string& target)
{
    const auto funcs = FunctionBodies(code);
    std::vector<std::string> seen { entry };
    for (size_t k = 0; k < seen.size(); ++k)
        for (const FuncBody& f : funcs)
            if (f.name == seen[k])
                for (const std::string& n : CalledNames(code.substr(f.open, f.close - f.open + 1)))
                    if (std::find(seen.begin(), seen.end(), n) == seen.end() &&
                        std::any_of(funcs.begin(), funcs.end(), [&](const FuncBody& g) { return g.name == n; }))
                        seen.push_back(n);
    return std::find(seen.begin(), seen.end(), target) != seen.end();
}

// One ImGui push/pop family
struct StackPair
{
    const char* open;  // call that opens, with its '(' e.g. "ImGui::BeginDisabled("
    const char* close; // call that closes
    bool countArg;     // the close takes a count (PopStyleColor(2)); an empty argument list means 1
};

inline const std::vector<StackPair>& StackPairs()
{
    static const std::vector<StackPair> pairs = {
        { "ImGui::BeginDisabled(", "ImGui::EndDisabled(", false },
        { "ImGui::PushItemWidth(", "ImGui::PopItemWidth(", false },
        { "ImGui::PushStyleColor(", "ImGui::PopStyleColor(", true },
        { "ImGui::PushStyleVar(", "ImGui::PopStyleVar(", true },
        { "ImGui::Indent(", "ImGui::Unindent(", false },
        { "ImGui::PushID(", "ImGui::PopID(", false },
        { "ImGui::PushTextWrapPos(", "ImGui::PopTextWrapPos(", false },
        { "ImGui::BeginGroup(", "ImGui::EndGroup(", false },
        { "ImGui::PushFont(", "ImGui::PopFont(", false },
    };
    return pairs;
}

// The calls of `call` ("ImGui::PopStyleColor(") in `body`, with their argument text (trimmed)
inline std::vector<std::string> CallArgs(const std::string& body, const std::string& call)
{
    std::vector<std::string> args;
    for (size_t p = body.find(call); p != std::string::npos; p = body.find(call, p + call.size()))
    {
        int depth = 1;
        size_t i = p + call.size();
        const size_t from = i;
        while (i < body.size() && depth > 0)
        {
            if (body[i] == '"')
            {
                i = SkipString(body, i);
                continue;
            }
            if (body[i] == '(')
                ++depth;
            else if (body[i] == ')' && --depth == 0)
                break;
            ++i;
        }
        args.push_back(Trim(body.substr(from, i - from)));
    }
    return args;
}

// Opens and closes of one family in `body` (a count argument such as PopStyleColor(2) is added up; any other argument
// counts as 1). Returns false and a text when the totals differ.
inline bool StackBalanced(const std::string& body, const StackPair& pair, std::string& problem)
{
    const size_t opened = CallArgs(body, pair.open).size();
    size_t closed = 0;
    for (const std::string& a : CallArgs(body, pair.close))
    {
        const bool number =
            !a.empty() && std::all_of(a.begin(), a.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        closed += (pair.countArg && number) ? static_cast<size_t>(std::stoul(a)) : 1;
    }
    if (opened != closed)
        problem = std::string(pair.open) + " x" + std::to_string(opened) + " but " + pair.close + " x" + std::to_string(closed);
    return opened == closed;
}

// True when a `return` can run between a push of the family and its pop in `body` (textual order, branches included). A
// return before the first push, or after the last pop, is fine. Returns inside a nested lambda body are not counted.
inline bool ReturnInsideStack(const std::string& body, const StackPair& pair, size_t& at)
{
    const size_t openLen = std::string(pair.open).size(), closeLen = std::string(pair.close).size();
    int open = 0;
    std::vector<size_t> lambdaEnds; // closing positions of the lambda bodies we are inside
    for (size_t i = 0; i < body.size();)
    {
        if (body[i] == '"')
        {
            i = SkipString(body, i);
            continue;
        }
        if (body[i] == '\'' && StartsCharLiteral(body, i))
        {
            i = SkipChar(body, i);
            continue;
        }
        while (!lambdaEnds.empty() && i > lambdaEnds.back())
            lambdaEnds.pop_back();
        if (body[i] == '{')
        {
            // a lambda body: `](...) {` or `] {`
            size_t k = i;
            while (k > 0 && std::isspace(static_cast<unsigned char>(body[k - 1])))
                --k;
            if (k > 0 && body[k - 1] == ')')
            {
                int depth = 0;
                while (k > 0)
                {
                    --k;
                    if (body[k] == ')')
                        ++depth;
                    else if (body[k] == '(' && --depth == 0)
                        break;
                }
                while (k > 0 && std::isspace(static_cast<unsigned char>(body[k - 1])))
                    --k;
            }
            if (k > 0 && body[k - 1] == ']')
            {
                const size_t end = MatchBrace(body, i);
                if (end != std::string::npos)
                    lambdaEnds.push_back(end);
            }
        }
        const bool wordStart = i == 0 || !IsIdentChar(body[i - 1]);
        if (wordStart && body.compare(i, openLen, pair.open) == 0)
            ++open;
        else if (wordStart && body.compare(i, closeLen, pair.close) == 0)
            open = open > 0 ? open - 1 : 0;
        else if (open > 0 && lambdaEnds.empty() && wordStart && body.compare(i, 6, "return") == 0 &&
                 (i + 6 >= body.size() || !IsIdentChar(body[i + 6])))
        {
            at = i;
            return true;
        }
        ++i;
    }
    return false;
}

} // namespace menuscan
