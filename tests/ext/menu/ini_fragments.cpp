// Ini hygiene of the menu redesign (WP4 item 6): every config key the fork binds (`b.Add("Section", "Key", ...)` in the comment-
// stripped sources under OptiScaler/ext) has a documented line `Key=auto` in the ini fragment named after its section
// (OptiScaler/ext/ini/<Section>.ini, merged into the shipped OptiScaler.ini by tools/package.ps1: the section is created when
// upstream has none). The keys that have no menu control on purpose (MenuManifest.h kIniOnlyKeys) say so in their comment.
//
// The comment requirement skips the NR fragment, which documents grouped keys with one shared comment.
// The generated NR Pass4..Pass30 keys are built by a loop in NrConfig.cpp, not by a literal b.Add, so they are outside this rule.
// Each rule also runs against deliberately broken models, so it is mutation-checked by construction.
#include "ext/core/MenuManifest.h"
#include "menu_scan.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

namespace fs = std::filesystem;

struct Bound
{
    std::string section, key, file;
    std::string member; // the config member bound to the key: the last identifier of the third argument ("nr.passes" -> "passes")
    std::string dir;    // directory of the source file that binds it
};

// key -> the comment block right above its line
using Fragments = std::map<std::string, std::map<std::string, std::string>>;

std::vector<Bound> ScanBound(const std::string& extDir)
{
    std::vector<Bound> found;
    for (const auto& entry : fs::recursive_directory_iterator(extDir))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h")
            continue;
        const std::string text = menuscan::StripComments(menuscan::ReadFile(entry.path().string()));
        for (const char* call : { "b.Add(\"", "b.AddEnum(\"" })
        {
            for (size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1))
            {
                const size_t s0 = at + std::string(call).size();
                const size_t s1 = text.find('"', s0);
                const size_t k0 = text.find('"', s1 + 1);
                const size_t k1 = k0 == std::string::npos ? k0 : text.find('"', k0 + 1);
                if (s1 == std::string::npos || k1 == std::string::npos)
                    continue;
                // the third argument: the bound member (up to the next ',' or ')' outside brackets)
                std::string arg;
                {
                    size_t i = k1 + 1;
                    while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ','))
                        ++i;
                    int depth = 0;
                    for (; i < text.size(); ++i)
                    {
                        if (text[i] == '(' || text[i] == '<' || text[i] == '[')
                            ++depth;
                        else if ((text[i] == ')' || text[i] == '>' || text[i] == ']') && depth > 0)
                            --depth;
                        else if (depth == 0 && (text[i] == ',' || text[i] == ')'))
                            break;
                        arg += text[i];
                    }
                    arg = menuscan::Trim(arg);
                }
                std::string member = arg;
                for (const char sep : { '.', '>', ':', '&', '*' })
                    if (const size_t p = member.rfind(sep); p != std::string::npos)
                        member = member.substr(p + 1);
                found.push_back({ text.substr(s0, s1 - s0), text.substr(k0 + 1, k1 - k0 - 1), entry.path().filename().string(), member,
                                  entry.path().parent_path().generic_string() });
            }
        }
    }
    return found;
}

Fragments LoadFragments(const std::string& iniDir)
{
    Fragments frags;
    for (const auto& entry : fs::directory_iterator(iniDir))
    {
        if (entry.path().extension() != ".ini")
            continue;
        auto& keys = frags[entry.path().stem().string()];
        std::string block;
        std::string all = menuscan::ReadFile(entry.path().string());
        size_t pos = 0;
        while (pos <= all.size())
        {
            size_t end = all.find('\n', pos);
            if (end == std::string::npos)
                end = all.size();
            std::string line = all.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty())
            {
                block.clear();
                continue;
            }
            if (line[0] == ';')
            {
                block += line + "\n";
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = line.substr(0, eq);
            while (!key.empty() && key.back() == ' ')
                key.pop_back();
            keys[key] = block;
            block.clear();
        }
    }
    return frags;
}

// ---- documented default ("Default (auto) is X" in the fragment comment) against the default the code declares ---------------
//
// For every bound key whose member is a `CustomOptional<bool|int|float...> name { default }` the comment of its ini line must
// state that same default. The declaration is found by member name (the declaration next to the binder wins when a name occurs
// in several config structs); a default written as a named constant is resolved from `constexpr NAME = value;`. Keys that
// cannot be compared (string and enum keys, a default that is not a literal or a known constant, a comment that does not state
// "Default (auto) is") are named in kNotCompared below with the reason: the test fails for an unlisted one, and for a listed one
// that became comparable, so the list cannot rot.

struct Decl
{
    std::string type, name, init, file, dir;
};

std::vector<Decl> ScanDecls(const std::string& extDir)
{
    std::vector<Decl> out;
    for (const auto& entry : fs::recursive_directory_iterator(extDir))
    {
        const std::string ext = entry.path().extension().string();
        if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h"))
            continue;
        const std::string text = menuscan::StripComments(menuscan::ReadFile(entry.path().string()));
        const std::string tag = "CustomOptional<";
        for (size_t p = text.find(tag); p != std::string::npos; p = text.find(tag, p + 1))
        {
            size_t i = p + tag.size();
            int depth = 1;
            while (i < text.size() && depth > 0)
            {
                if (text[i] == '<')
                    ++depth;
                else if (text[i] == '>')
                    --depth;
                ++i;
            }
            std::string targs = text.substr(p + tag.size(), i - 1 - (p + tag.size()));
            std::string type = menuscan::Trim(targs.substr(0, targs.find(',')));
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                ++i;
            std::string name;
            while (i < text.size() && menuscan::IsIdentChar(text[i]))
                name += text[i++];
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                ++i;
            if (name.empty() || i >= text.size() || (text[i] != '{' && text[i] != '('))
                continue;
            const char close = text[i] == '{' ? '}' : ')';
            const size_t from = i + 1;
            int d = 1;
            for (++i; i < text.size() && d > 0; ++i)
            {
                if (text[i] == '{' || text[i] == '(')
                    ++d;
                else if (text[i] == '}' || text[i] == ')')
                    --d;
            }
            (void) close;
            out.push_back({ type, name, menuscan::Trim(text.substr(from, i - 1 - from)), entry.path().filename().string(),
                            entry.path().parent_path().generic_string() });
        }
    }
    return out;
}

std::map<std::string, std::string> ScanConsts(const std::string& extDir)
{
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(extDir))
    {
        const std::string ext = entry.path().extension().string();
        if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h"))
            continue;
        const std::string text = menuscan::StripComments(menuscan::ReadFile(entry.path().string()));
        for (size_t p = text.find("constexpr "); p != std::string::npos; p = text.find("constexpr ", p + 1))
        {
            const size_t eq = text.find('=', p);
            const size_t semi = text.find(';', p);
            if (eq == std::string::npos || semi == std::string::npos || eq > semi)
                continue;
            std::string head = menuscan::Trim(text.substr(p + 10, eq - p - 10));
            std::string name;
            for (size_t i = head.size(); i > 0 && menuscan::IsIdentChar(head[i - 1]); --i)
                name.insert(name.begin(), head[i - 1]);
            if (!name.empty())
                out[name] = menuscan::Trim(text.substr(eq + 1, semi - eq - 1));
        }
    }
    return out;
}

enum class Kind { Bool, Int, Float, Other };

Kind KindOf(const std::string& type)
{
    for (const char* t : { "bool" })
        if (type == t)
            return Kind::Bool;
    for (const char* t : { "int", "int32_t", "uint32_t", "int64_t", "uint64_t", "uint8_t", "uint16_t", "int16_t", "unsigned", "size_t", "long" })
        if (type == t)
            return Kind::Int;
    for (const char* t : { "float", "double" })
        if (type == t)
            return Kind::Float;
    return Kind::Other;
}

struct Value
{
    bool ok = false;
    double v = 0; // true = 1, false = 0
};

// A literal (true/false, a number with an optional f/u/L suffix) or a constant that resolves to one
Value ParseValue(std::string s, const std::map<std::string, std::string>& consts, int depth = 0)
{
    s = menuscan::Trim(s);
    if (s == "true")
        return { true, 1 };
    if (s == "false")
        return { true, 0 };
    if (s.empty())
        return {};
    std::string num = s;
    while (!num.empty() && (num.back() == 'f' || num.back() == 'F' || num.back() == 'u' || num.back() == 'U' || num.back() == 'L'))
        num.pop_back();
    char* end = nullptr;
    const double v = std::strtod(num.c_str(), &end);
    if (!num.empty() && end == num.c_str() + num.size())
        return { true, v };
    const auto c = consts.find(s);
    if (c != consts.end() && depth < 3)
        return ParseValue(c->second, consts, depth + 1);
    return {};
}

// The default a fragment comment states: the word after "Default (auto) is " ("true (on) while ..." -> true, "4 (Lanczos3)" -> 4)
std::string DocumentedDefault(const std::string& comment)
{
    const std::string tag = "Default (auto) is ";
    const size_t at = comment.find(tag);
    if (at == std::string::npos)
        return {};
    std::string tok;
    for (size_t i = at + tag.size(); i < comment.size() && !std::isspace(static_cast<unsigned char>(comment[i])) && comment[i] != ',' &&
                                     comment[i] != ';' && comment[i] != ')';
         ++i)
        tok += comment[i];
    while (!tok.empty() && tok.back() == '.')
        tok.pop_back();
    return tok;
}

// Documented word as a value for the key's kind: on/off/yes/no for bools, numbers for the rest
Value DocValue(const std::string& word, Kind kind)
{
    if (kind == Kind::Bool)
    {
        if (word == "true" || word == "on" || word == "yes")
            return { true, 1 };
        if (word == "false" || word == "off" || word == "no")
            return { true, 0 };
        return {};
    }
    char* end = nullptr;
    const double v = std::strtod(word.c_str(), &end);
    return (!word.empty() && end == word.c_str() + word.size()) ? Value { true, v } : Value {};
}

// Keys that cannot be compared, with the reason (the reasons are checked: a key listed here must still fail to compare)
struct NotCompared
{
    const char* key; // "[Section] Key"
    const char* reason;
};
const NotCompared kNotCompared[] = {
    { "[DlssNr] ", "the NR fragment documents grouped keys with one shared comment, so most of its comments carry no "
                   "'Default (auto) is' (the ones that do are compared); the generated Pass2/Pass3 keys have no named member; "
                   "ToggleKey's default is the UnboundKey constant of Config.h, outside ext" },
    { "[HDR] Screenshot", "the screenshot keys describe their default in prose (-1 = unbound, formats by name)" },
    { "[Mipmap] MipmapBiasAuto:", "declared without a default on purpose (CustomOptional<bool, NoDefault>): the default depends on the game (on for FFXIV, off elsewhere) and an explicit false must survive a save" },
    { "[Mipmap] AutomaticFFXIV", "the alias of MipmapBiasAuto, declared without a default for the same reason" },
    { "[LowLatency] ", "upstream enum members (LowLatencyInput/Output) declared in Config.h, not in ext" },
    { "[FramePacing] Trace:", "declared without a default on purpose (CustomOptional<bool, NoDefault>): unset means auto (on only with file logging at Debug or Trace), and an explicit false must survive a save" },
    { "[Window] Mode", "the member name 'mode' is declared in several config structs (HDR, HudLess, Window) and the Window binder sits in ExtConfig.cpp, so the declaration cannot be told apart by name; it is a string key" },
};

struct DefaultsReport
{
    std::vector<std::string> mismatches; // the documented default differs from the code
    std::vector<std::string> skipped;    // "[Section] Key: reason" for keys that could not be compared
    size_t compared = 0;
    size_t otherKinds = 0; // string / enum / wstring keys: not bool, int or float
};

DefaultsReport CompareDefaults(const std::vector<Bound>& bound, const Fragments& frags, const std::vector<Decl>& decls,
                               const std::map<std::string, std::string>& consts)
{
    DefaultsReport rep;
    for (const Bound& b : bound)
    {
        const std::string label = "[" + b.section + "] " + b.key;
        std::vector<const Decl*> cands;
        for (const Decl& d : decls)
            if (d.name == b.member)
                cands.push_back(&d);
        if (cands.size() > 1)
        {
            std::vector<const Decl*> near;
            for (const Decl* d : cands)
                if (d->dir == b.dir)
                    near.push_back(d);
            if (!near.empty())
                cands = near;
        }
        if (cands.empty())
        {
            rep.skipped.push_back(label + ": no declaration of the member '" + b.member + "'");
            continue;
        }
        if (cands.size() > 1 && std::all_of(cands.begin(), cands.end(), [](const Decl* d) { return KindOf(d->type) == Kind::Other; }))
        {
            ++rep.otherKinds; // the same string/enum member name in several config structs: not a bool, int or float key
            continue;
        }
        if (cands.size() > 1)
        {
            rep.skipped.push_back(label + ": the member name '" + b.member + "' is declared in several config structs");
            continue;
        }
        const Kind kind = KindOf(cands[0]->type);
        if (kind == Kind::Other)
        {
            ++rep.otherKinds;
            continue;
        }
        const auto f = frags.find(b.section);
        const auto k = f == frags.end() ? decltype(f->second.end()) {} : f->second.find(b.key);
        if (f == frags.end() || k == f->second.end())
            continue; // missing lines are the other rule's finding
        const std::string word = DocumentedDefault(k->second);
        if (word.empty())
        {
            rep.skipped.push_back(label + ": the comment does not state \"Default (auto) is\"");
            continue;
        }
        const Value code = ParseValue(cands[0]->init, consts);
        if (!code.ok)
        {
            rep.skipped.push_back(label + ": the declared default '" + cands[0]->init + "' is not a literal or a known constant");
            continue;
        }
        const Value doc = DocValue(word, kind);
        if (!doc.ok)
        {
            rep.mismatches.push_back(label + ": the documented default '" + word + "' is not a " +
                                     (kind == Kind::Bool ? "bool" : "number"));
            continue;
        }
        ++rep.compared;
        if (std::abs(doc.v - code.v) > 1e-6)
            rep.mismatches.push_back(label + ": documented default '" + word + "' but the code declares '" + cands[0]->init + "' (" +
                                     cands[0]->file + ")");
    }
    return rep;
}


// What is wrong with a report: mismatches, skipped keys that no kNotCompared entry covers, entries that cover nothing
std::vector<std::string> DefaultsProblems(const DefaultsReport& rep)
{
    std::vector<std::string> p = rep.mismatches;
    std::vector<size_t> used(sizeof(kNotCompared) / sizeof(kNotCompared[0]), 0);
    for (const std::string& s : rep.skipped)
    {
        bool covered = false;
        for (size_t i = 0; i < used.size(); ++i)
            if (s.rfind(kNotCompared[i].key, 0) == 0)
            {
                ++used[i];
                covered = true;
            }
        if (!covered)
            p.push_back("not compared and not listed in kNotCompared: " + s);
    }
    for (size_t i = 0; i < used.size(); ++i)
        if (used[i] == 0)
            p.push_back(std::string("stale kNotCompared entry (nothing skipped under it any more): ") + kNotCompared[i].key);
    return p;
}

std::vector<std::string> Problems(const std::vector<Bound>& bound, const Fragments& frags)
{
    std::vector<std::string> p;
    for (const Bound& b : bound)
    {
        const auto f = frags.find(b.section);
        if (f == frags.end())
        {
            p.push_back("[" + b.section + "] " + b.key + " (" + b.file + "): no ini fragment " + b.section + ".ini");
            continue;
        }
        const auto k = f->second.find(b.key);
        if (k == f->second.end())
            p.push_back("[" + b.section + "] " + b.key + " (" + b.file + "): no line in " + b.section + ".ini");
        else if (k->second.empty() && b.section != "DlssNr") // the NR fragment documents grouped keys with one shared comment
            p.push_back("[" + b.section + "] " + b.key + ": the ini line has no comment above it");
    }
    // keys without a menu control on purpose say so
    for (const char* entry : ext::menu::manifest::kIniOnlyKeys)
    {
        const std::string text = entry; // "[Section] Key"
        const size_t close = text.find(']');
        const std::string section = text.substr(1, close - 1), key = text.substr(close + 2);
        const auto f = frags.find(section);
        const auto k = f == frags.end() ? decltype(f->second.end()) {} : f->second.find(key);
        if (f == frags.end() || k == f->second.end())
            p.push_back(text + ": ini-only key without an ini line");
        else if (k->second.find("No menu control") == std::string::npos)
            p.push_back(text + ": the ini comment does not say \"No menu control\"");
    }
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const auto bound = ScanBound(repo + "/OptiScaler/ext");
    const auto frags = LoadFragments(repo + "/OptiScaler/ext/ini");

    // the scan sees the keys it should (it would pass vacuously on an empty scan)
    auto has = [&](const char* s, const char* k)
    {
        for (const Bound& b : bound)
            if (b.section == s && b.key == k)
                return true;
        return false;
    };
    CHECK(bound.size() >= 140);
    CHECK(has("Companion", "HudReplacement") && has("Companion", "HudInterpolation"));
    CHECK(has("HudLess", "Diagnostics") && has("HudLess", "ReShadeProbe") && has("HudLess", "ReShadePresets"));
    CHECK(has("DLSSG", "UIRecomposition") && has("XeFG", "UnlockMFG") && has("DlssNr", "ScanExposure"));
    CHECK(frags.count("Companion") == 1 && frags.count("HudLess") == 1 && frags.count("DlssNr") == 1);

    // the two ini-only developer keys of the HUD-less guard and the HDR paste ship as auto (the code default applies) and say which default
    // that is (string keys: the comparison below does not read them)
    {
        const std::string hudlessIni = menuscan::ReadFile(repo + "/OptiScaler/ext/ini/HudLess.ini");
        auto hasLine = [&](const std::string& text)
        { return hudlessIni.find("\n" + text + "\r\n") != std::string::npos || hudlessIni.find("\n" + text + "\n") != std::string::npos; };
        CHECK(hasLine("GuardMode=auto") && hasLine("HdrPaste=auto"));
        // the two developer probes ship switched off: the key line of each is there once, and it says auto (the code default is false)
        auto keyLines = [&](const std::string& key)
        {
            std::vector<std::string> found;
            const std::string needle = "\n" + key + "=";
            for (size_t at = hudlessIni.find(needle); at != std::string::npos; at = hudlessIni.find(needle, at + 1))
            {
                const size_t end = hudlessIni.find_first_of("\r\n", at + 1);
                found.push_back(hudlessIni.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1));
            }
            return found;
        };
        CHECK(keyLines("PlateDiagnostics") == std::vector<std::string> { "PlateDiagnostics=auto" });
        CHECK(keyLines("PlateOverlay") == std::vector<std::string> { "PlateOverlay=auto" });
        CHECK(hasLine("PlateDiagnostics=auto") && hasLine("PlateOverlay=auto"));
        CHECK(frags.at("HudLess").at("GuardMode").find("legacy or density - Default (auto) is legacy") != std::string::npos);
        CHECK(frags.at("HudLess").at("HdrPaste").find("off or on - Default (auto) is off") != std::string::npos);
    }

    // the per-axis model scale keys and the depth-aware switch ship as auto (the code default applies): the shipped VALUE
    // matters, not just the key presence (a fragment that shipped 0.5 or true would change every default user, and the
    // key-presence rule cannot see a value).
    {
        const std::string nrIni = menuscan::ReadFile(repo + "/OptiScaler/ext/ini/DlssNr.ini");
        auto hasNrLine = [&](const std::string& text)
        { return nrIni.find("\n" + text + "\r\n") != std::string::npos || nrIni.find("\n" + text + "\n") != std::string::npos; };
        CHECK(hasNrLine("WorkingScaleX=auto") && hasNrLine("WorkingScaleY=auto") && hasNrLine("DepthAwareResolve=auto"));
    }

    // the documented default of every bool/int/float key equals the default the code declares
    {
        const auto decls = ScanDecls(repo + "/OptiScaler/ext");
        const auto consts = ScanConsts(repo + "/OptiScaler/ext");
        const auto rep = CompareDefaults(bound, frags, decls, consts);
        std::printf("defaults: compared %zu keys, %zu string/enum keys not compared by kind, %zu listed in kNotCompared\n",
                    rep.compared, rep.otherKinds, rep.skipped.size());
        const auto dp = DefaultsProblems(rep);
        for (const auto& s : dp)
            std::printf("  %s\n", s.c_str());
        CHECK(dp.empty());
        CHECK(rep.compared >= 80); // the comparison saw the keys (it would pass vacuously on an empty scan)

        // spot checks: these pairs are the ones that once disagreed or have a named constant behind them
        auto declaredDefault = [&](const char* member) -> std::string
        {
            for (const Decl& d : decls)
                if (d.name == member)
                    return d.init;
            return {};
        };
        CHECK(declaredDefault("uiRecomposition") == "true");                 // [DLSSG] UIRecomposition: unset = on
        CHECK(declaredDefault("xefgMaxInterpolatedFrames") == "kXeFgMaxInterpolations" && consts.count("kXeFgMaxInterpolations") == 1);

        // negative controls: each rule complains about its own breakage
        auto flipDoc = [&](const char* section, const char* key, const char* from, const char* to) -> DefaultsReport
        {
            Fragments broken = frags;
            std::string& c = broken[section][key];
            const size_t at = c.find(from);
            CHECK(at != std::string::npos);
            if (at != std::string::npos)
                c.replace(at, std::string(from).size(), to);
            return CompareDefaults(bound, broken, decls, consts);
        };
        CHECK(!DefaultsProblems(flipDoc("DLSSG", "UIRecomposition", "Default (auto) is true", "Default (auto) is false")).empty()); // bool, documented wrongly
        CHECK(!DefaultsProblems(flipDoc("HudLess", "MaskThreshold", "Default (auto) is 0.008", "Default (auto) is 0.009")).empty()); // float
        CHECK(!DefaultsProblems(flipDoc("XeFG", "MaxInterpolatedFrames", "Default (auto) is 7", "Default (auto) is 5")).empty());   // int behind a constant
        CHECK(!DefaultsProblems(flipDoc("DLSSG", "UIRecomposition", "Default (auto) is true", "Default (auto) is maybe")).empty());  // not a bool
        CHECK(!DefaultsProblems(flipDoc("DLSSG", "UIRecomposition", "Default (auto) is true", "Default is true")).empty());          // phrase lost: now uncovered
        {
            auto broken = decls; // the code default changes, the ini text does not
            for (Decl& d : broken)
                if (d.name == "uiRecomposition")
                    d.init = "false";
            CHECK(!DefaultsProblems(CompareDefaults(bound, frags, broken, consts)).empty());
        }
        {
            auto broken = consts; // the named constant changes
            broken["kXeFgMaxInterpolations"] = "5";
            CHECK(!DefaultsProblems(CompareDefaults(bound, frags, decls, broken)).empty());
        }
        {
            auto broken = decls; // a member that nobody declares any more is a finding, not a silent skip
            for (Decl& d : broken)
                if (d.name == "uiRecomposition")
                    d.name = "uiRecompositionGone";
            CHECK(!DefaultsProblems(CompareDefaults(bound, frags, broken, consts)).empty());
        }
    }
    const auto problems = Problems(bound, frags);
    for (const auto& s : problems)
        std::printf("  %s\n", s.c_str());
    CHECK(problems.empty());

    // the new fragment documents both Companion keys with their default and their requirement
    {
        auto doc = [&](const char* s, const char* k) -> std::string
        {
            const auto f = frags.find(s);
            if (f == frags.end())
                return {};
            const auto d = f->second.find(k);
            return d == f->second.end() ? std::string {} : d->second;
        };
        const auto companion = frags.find("Companion");
        CHECK(companion != frags.end() && companion->second.size() == 2);
        CHECK(doc("Companion", "HudReplacement").find("OptiFG DLSS-G or XeFG") != std::string::npos);
        CHECK(doc("Companion", "HudReplacement").find("Default (auto) is false") != std::string::npos);
        CHECK(doc("Companion", "HudInterpolation").find("Requires HudReplacement") != std::string::npos);
        CHECK(doc("Companion", "HudInterpolation").find("Default (auto) is false") != std::string::npos);
    }

    // negative controls: each rule complains about its own breakage
    {
        Fragments broken = frags;
        broken["Companion"].erase("HudInterpolation");
        CHECK(!Problems(bound, broken).empty()); // a bound key lost its ini line
    }
    {
        Fragments broken = frags;
        broken.erase("Companion");
        CHECK(!Problems(bound, broken).empty()); // a section lost its fragment
    }
    {
        Fragments broken = frags;
        broken["HudLess"]["Diagnostics"] = "; a developer switch\n";
        CHECK(!Problems(bound, broken).empty()); // the ini-only key forgot to say so
    }
    {
        Fragments broken = frags;
        broken["HudLess"]["ReShadeProbe"] = "";
        CHECK(!Problems(bound, broken).empty()); // an ini line without a comment
    }
    {
        Fragments broken = frags;
        broken["HudLess"].erase("ReShadeProbe");
        CHECK(!Problems(bound, broken).empty()); // an ini-only key without a line
    }
    {
        auto more = bound;
        more.push_back({ "Companion", "NoSuchKey", "test" });
        CHECK(!Problems(more, frags).empty()); // a new bound key without a line
    }

    if (failures)
    {
        std::printf("ini_fragments: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("ini_fragments: all passed\n");
    return 0;
}
