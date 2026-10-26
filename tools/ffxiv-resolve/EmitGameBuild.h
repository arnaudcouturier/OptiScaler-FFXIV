#pragma once
// OFFLINE ONLY (lives under tools\, never included from OptiScaler\). The codegen of the known-build table entry:
// the text of GameBuild.h with the BOUND fields rewritten to what the resolver found on a new executable.
//
// What it does, in the order it does it:
//   1. Lexes the header text (comments, whitespace and preprocessor lines are skipped, nothing else is interpreted) and finds, struct by
//      struct, the member declarations it can rewrite:  `std::uint32_t name = <integer literal>;`  and
//      `std::array<std::uint8_t, N> name { <N integer literals> };`. The group of a field ("nativeQuality", "jitterSource", ...) is mapped to its
//      struct type through the member declarations inside `struct GameBuild`, so `settingsPointerRva` of NativeQualityBuild and the one of
//      JitterSourceBuild are two different fields, each rewritten inside its own struct only.
//   2. For every field a Binding of the registry names (Rva, Offset and Guard rows) takes the value out of the resolved build and replaces the
//      integer literal TOKEN(S) in a copy of the text. A literal whose value already equals the resolved value is not touched (so the output
//      does not depend on this code reproducing the header's spelling, `0x08` stays `0x08`); a changed one is written as lowercase hex (guard bytes
//      `0x%02x`, offsets at least two digits) or, when the old literal was decimal, as decimal. timeDateStamp and sizeOfImage are rewritten from
//      the image. Every comment, every line break, every other character is copied unchanged.
//   3. A bound field whose seam is not usable (NotFound, Ambiguous, Conflict, EntryPatched, or no seam at all) is UNRESOLVED. By default the
//      codegen REFUSES (nothing is produced). With allowPartial the unresolved fields keep their old text and get a trailing comment
//      `// NOT re-verified for this build`; a value found by a single anchor, or resting on a weak seam, gets a review mark. Marks are stripped
//      and re-derived every time, so a second run over an output never stacks them.
//   4. It lists what the resolver CANNOT derive (every Rva/Guard row no binding covers, every Offset row no usable seam fills, every Constant
//      and Pattern row): those values were copied from the input header, not checked against the new executable.
// It never touches a file: the caller decides where the text goes. Pure (no I/O), so tests feed it text and synthetic resolved builds.

#include "ext/ffxiv/catalog/CatalogTable.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/FieldTable.h"
#include "ext/ffxiv/resolve/ResolveTypes.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ext::ffxiv::resolve::offline
{
// The three marks the codegen writes (and the only text it ever removes from a header line).
inline constexpr const char* kMarkNotVerified = "// NOT re-verified for this build";
inline constexpr const char* kMarkSingleAnchor = "// single-anchor: review before pinning";
inline constexpr const char* kMarkWeakDependency = "// via weak dependency: review before pinning";

struct UnresolvedBinding
{
    std::string key;    // the GameBuild field
    std::string seam;   // the seam that should have filled it
    std::string status; // StatusName, or "missing" when the registry names a seam the result does not have
    std::string reason;
};

struct ChangedField
{
    std::string key;     // "nativeQuality.querySizeRva", "timeDateStamp"
    std::string oldText; // the literal in the input header; for a guard "N of M bytes differ"
    std::string newText;
};

struct NotVerifiedField
{
    std::string key;
    std::string kind;  // "Rva", "Guard", "Offset", "Constant", "Pattern"
    std::string value; // the text of the input header (kept), or "(not in the header)"
    std::string why;
};

struct EmitResult
{
    enum class Outcome : std::uint8_t
    {
        Emitted,        // every bound field was re-verified and rewritten: `text` is the new header
        EmittedPartial, // allowPartial and some bound field is unresolved: `text` marks them
        Refused,        // some bound field is unresolved and allowPartial is off: no text
        Error,          // the header does not fit the field table, or the result is unusable: no text
    };
    Outcome outcome = Outcome::Error;
    std::string text;
    std::vector<std::string> errors;
    std::vector<UnresolvedBinding> unresolved;
    std::vector<ChangedField> changed;
    std::vector<std::string> confirmed;   // bound fields whose resolved value equals the header's
    std::vector<std::string> marked;      // fields that carry one of the three marks in the output
    std::vector<NotVerifiedField> notVerified;
    std::vector<std::string> staleMentions; // comment lines of the OUTPUT that still quote the old value of a changed field
    std::size_t boundFields = 0;
};

namespace emit_detail
{
struct Token
{
    char kind; // 'i' identifier, 'n' number, 'p' punctuation (one character)
    std::size_t begin;
    std::size_t end;
};

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
inline bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
inline bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

inline std::vector<Token> Lex(const std::string& s)
{
    std::vector<Token> out;
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n)
    {
        const char c = s[i];
        if (IsSpace(c))
        {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/')
        {
            while (i < n && s[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*')
        {
            const std::size_t e = s.find("*/", i + 2);
            i = e == std::string::npos ? n : e + 2;
            continue;
        }
        if (c == '#')
        {
            while (i < n && s[i] != '\n')
                ++i;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            const char quote = c;
            ++i;
            while (i < n && s[i] != quote && s[i] != '\n')
            {
                if (s[i] == '\\' && i + 1 < n)
                    ++i;
                ++i;
            }
            if (i < n && s[i] == quote)
                ++i;
            continue;
        }
        if (IsIdentStart(c))
        {
            const std::size_t b = i;
            while (i < n && IsIdentChar(s[i]))
                ++i;
            out.push_back({ 'i', b, i });
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0)
        {
            const std::size_t b = i;
            while (i < n && (IsIdentChar(s[i]) || s[i] == '\'' || s[i] == '.'))
                ++i;
            out.push_back({ 'n', b, i });
            continue;
        }
        out.push_back({ 'p', i, i + 1 });
        ++i;
    }
    return out;
}

// An integer literal: decimal or 0x hex, optional digit separators, optional u/l suffix. false for anything else (a float, an expression part).
inline bool ParseNumber(const std::string& text, std::uint64_t& value, bool& hex, std::string& suffix)
{
    std::string t;
    for (char c : text)
    {
        if (c != '\'')
            t.push_back(c);
    }
    value = 0;
    hex = false;
    suffix.clear();
    std::size_t i = 0;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
    {
        hex = true;
        i = 2;
    }
    std::size_t digits = 0;
    for (; i < t.size(); ++i)
    {
        const char c = t[i];
        int v = -1;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (hex && c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        if (v < 0)
            break;
        if (digits >= (hex ? 16u : 19u)) // more digits than a 64 bit value holds: not a literal this codegen handles
            return false;
        value = hex ? value * 16 + static_cast<std::uint64_t>(v) : value * 10 + static_cast<std::uint64_t>(v);
        ++digits;
    }
    if (digits == 0)
        return false;
    suffix = t.substr(i);
    if (suffix.size() > 3)
        return false;
    for (char c : suffix)
    {
        if (c != 'u' && c != 'U' && c != 'l' && c != 'L')
            return false;
    }
    return true;
}

// The same spelling family as the old literal: hex stays hex (lowercase, at least `minDigits` digits), decimal stays decimal; a suffix is kept.
inline std::string FormatLike(bool hex, const std::string& suffix, std::uint64_t value, unsigned minDigits)
{
    char buf[40];
    if (hex)
        std::snprintf(buf, sizeof(buf), "0x%0*llx", static_cast<int>(minDigits), static_cast<unsigned long long>(value));
    else
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    return std::string(buf) + suffix;
}

struct Member
{
    bool simple = false;                 // the form the codegen can rewrite
    bool array = false;
    std::size_t declared = 0;            // array: the N of std::array<std::uint8_t, N>
    std::vector<std::size_t> numbers;    // token indices of the initialiser literal(s)
    std::size_t semi = 0;                // text offset just after the closing ';'
    std::size_t declBegin = 0;           // text offset of the first token of the declaration
    int count = 0;                       // declarations of this name inside the struct
};
using Members = std::map<std::string, Member>;

struct Parsed
{
    std::string text;
    std::vector<Token> toks;
    std::map<std::string, Members> structs;
    std::map<std::string, int> structCount;
    std::map<std::string, std::string> groupType; // member of struct GameBuild -> its struct type
};

inline std::string TokText(const Parsed& p, std::size_t i) { return p.text.substr(p.toks[i].begin, p.toks[i].end - p.toks[i].begin); }

inline void AnalyzeUnit(Parsed& p, const std::string& structName, Members& members, std::size_t a, std::size_t b)
{
    // the unit is tokens [a, b); `std ::` prefixes are dropped (the header spells std::uint32_t and std::array)
    std::vector<std::string> w;
    std::vector<std::size_t> idx;
    for (std::size_t k = a; k < b; ++k)
    {
        if (p.toks[k].kind == 'i' && TokText(p, k) == "std" && k + 2 < b && p.toks[k + 1].kind == 'p' && p.text[p.toks[k + 1].begin] == ':' &&
            p.toks[k + 2].kind == 'p' && p.text[p.toks[k + 2].begin] == ':')
        {
            k += 2;
            continue;
        }
        w.push_back(TokText(p, k));
        idx.push_back(k);
    }
    if (w.empty())
        return;
    const std::size_t semi = p.toks[b].end;
    auto isNum = [&](std::size_t k) { return k < idx.size() && p.toks[idx[k]].kind == 'n'; };
    auto isIdent = [&](std::size_t k) { return k < idx.size() && p.toks[idx[k]].kind == 'i'; };

    std::string name;
    Member m;
    m.semi = semi;
    m.declBegin = p.toks[a].begin;
    if (w[0] == "uint32_t" && w.size() >= 2 && isIdent(1))
    {
        name = w[1];
        if (w.size() == 4 && w[2] == "=" && isNum(3))
        {
            m.simple = true;
            m.numbers = { idx[3] };
        }
    }
    else if (w[0] == "array" && w.size() > 6 && w[1] == "<" && w[2] == "uint8_t" && w[3] == "," && isNum(4) && w[5] == ">" && isIdent(6))
    {
        name = w[6];
        m.array = true;
        std::uint64_t n = 0;
        bool hex = false;
        std::string sfx;
        if (ParseNumber(w[4], n, hex, sfx))
            m.declared = static_cast<std::size_t>(n);
        std::size_t k = 7;
        if (k < w.size() && w[k] == "=")
            ++k;
        if (k < w.size() && w[k] == "{")
        {
            ++k;
            bool ok = true;
            std::vector<std::size_t> nums;
            while (k < w.size() && w[k] != "}")
            {
                if (!isNum(k))
                {
                    ok = false;
                    break;
                }
                nums.push_back(idx[k]);
                ++k;
                if (k < w.size() && w[k] == ",")
                    ++k;
                else
                    break;
            }
            ok = ok && k + 1 == w.size() && w[k] == "}";
            if (ok)
            {
                m.simple = true;
                m.numbers = nums;
            }
        }
    }
    else if (structName == "GameBuild" && w.size() == 4 && isIdent(0) && isIdent(1) && w[2] == "{" && w[3] == "}")
    {
        p.groupType[w[1]] = w[0];
        name = w[1];
    }
    else
        return;
    Member& slot = members[name];
    const int count = slot.count + 1;
    slot = m;
    slot.count = count;
}

inline void ParseStruct(Parsed& p, std::size_t& i, const std::string& name)
{
    Members& members = p.structs[name];
    ++p.structCount[name];
    int depth = 0;
    std::size_t unitStart = i;
    for (; i < p.toks.size(); ++i)
    {
        const Token& t = p.toks[i];
        if (t.kind != 'p')
            continue;
        const char c = p.text[t.begin];
        if (c == '{')
            ++depth;
        else if (c == '}')
        {
            if (depth == 0)
            {
                ++i;
                return;
            }
            --depth;
        }
        else if (c == ';' && depth == 0)
        {
            if (i > unitStart)
                AnalyzeUnit(p, name, members, unitStart, i);
            unitStart = i + 1;
        }
    }
}

inline Parsed Parse(const std::string& text)
{
    Parsed p;
    p.text = text;
    p.toks = Lex(text);
    for (std::size_t i = 0; i + 2 < p.toks.size();)
    {
        if (p.toks[i].kind == 'i' && TokText(p, i) == "struct" && p.toks[i + 1].kind == 'i' && p.toks[i + 2].kind == 'p' && p.text[p.toks[i + 2].begin] == '{')
        {
            const std::string name = TokText(p, i + 1);
            std::size_t j = i + 3;
            ParseStruct(p, j, name);
            i = j;
        }
        else
            ++i;
    }
    return p;
}

inline std::string Hex(std::uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof(b), "0x%x", v);
    return b;
}

// The struct type that holds `group` ("" = GameBuild itself). Empty when the header has no such member.
inline std::string TypeOf(const Parsed& p, const std::string& group)
{
    if (group.empty())
        return "GameBuild";
    const auto it = p.groupType.find(group);
    return it == p.groupType.end() ? std::string() : it->second;
}

inline const Member* Lookup(const Parsed& p, const std::string& group, const std::string& name, std::string* why)
{
    const std::string type = TypeOf(p, group);
    if (type.empty())
    {
        if (why)
            *why = "struct GameBuild has no member `" + group + "` of a struct type";
        return nullptr;
    }
    const auto s = p.structs.find(type);
    if (s == p.structs.end())
    {
        if (why)
            *why = "struct " + type + " is not in the header";
        return nullptr;
    }
    if (p.structCount.at(type) != 1)
    {
        if (why)
            *why = "struct " + type + " is defined " + std::to_string(p.structCount.at(type)) + " times in the header";
        return nullptr;
    }
    const auto m = s->second.find(name);
    if (m == s->second.end())
    {
        if (why)
            *why = "struct " + type + " has no member `" + name + "`";
        return nullptr;
    }
    if (m->second.count != 1)
    {
        if (why)
            *why = "struct " + type + " declares `" + name + "` " + std::to_string(m->second.count) + " times";
        return nullptr;
    }
    return &m->second;
}

struct Edit
{
    std::size_t begin;
    std::size_t end;
    std::string text;
};

inline const char* KindName(FieldKind k)
{
    switch (k)
    {
    case FieldKind::Rva:
        return "Rva";
    case FieldKind::Guard:
        return "Guard";
    case FieldKind::Offset:
        return "Offset";
    case FieldKind::Constant:
        return "Constant";
    case FieldKind::Pattern:
        return "Pattern";
    }
    return "?";
}

// The text of a member's literal(s) as the header has it: the first literal, or the bytes as contiguous hex for an array.
inline std::string HeaderValue(const Parsed& p, const FieldRow& row, const std::string& group)
{
    const Member* m = Lookup(p, group, row.name, nullptr);
    if (m == nullptr)
        return "(not in the header)";
    if (!m->simple) // an initialiser that is not one literal (`128u << 20`): its text after the `=`
    {
        std::string decl = p.text.substr(m->declBegin, m->semi - 1 - m->declBegin);
        const std::size_t eq = decl.find('=');
        if (eq == std::string::npos)
            return "(not a literal)";
        decl.erase(0, decl.find_first_not_of(" \t", eq + 1));
        return decl;
    }
    if (!m->array)
        return TokText(p, m->numbers[0]);
    std::string out;
    for (std::size_t n : m->numbers)
    {
        std::uint64_t v = 0;
        bool hex = false;
        std::string sfx;
        char b[8];
        if (ParseNumber(TokText(p, n), v, hex, sfx))
            std::snprintf(b, sizeof(b), "%02llx", static_cast<unsigned long long>(v & 0xff));
        else
            std::snprintf(b, sizeof(b), "??");
        out += b;
    }
    return out;
}

} // namespace emit_detail

// Which Constant rows a seam cross-checks live (against the value compiled into the registry, which is not necessarily the header's).
struct ConstantSeam
{
    const char* key;
    const char* seam;
};
inline constexpr ConstantSeam kConstantSeams[] = {
    { "jitterSource.strengthFloorBits", "jitterSource.strengthFloor" },
    { "jitterSource.ndcSignMaskBits", "jitterSource.ndcSignMask" },
};

inline EmitResult EmitGameBuild(const std::string& header, const ResolvedBuild& rb, const Registry& registry, bool allowPartial)
{
    using namespace emit_detail;
    EmitResult r;
    const Parsed p = Parse(header);

    struct Plan
    {
        const FieldRow* row = nullptr; // nullptr: timeDateStamp / sizeOfImage
        std::string key;
        std::string group;
        const Member* member = nullptr;
        const Binding* binding = nullptr;
        const SeamResult* seam = nullptr;
        bool usable = false;
    };
    std::vector<Plan> plans;

    // ---- the two identity fields ---------------------------------------------------------------------------------------------------
    struct Identity
    {
        const char* name;
        std::uint32_t value;
    };
    const Identity identities[2] = { { "timeDateStamp", rb.timeDateStamp }, { "sizeOfImage", rb.sizeOfImage } };
    if (rb.timeDateStamp == 0 && rb.sizeOfImage == 0)
        r.errors.push_back("the resolve result carries no image identity (stamp and size are 0)");
    if (!rb.registryErrors.empty())
        r.errors.push_back("the registry has errors: nothing was resolved");

    // ---- validate every field the registry binds against the header ------------------------------------------------------------------
    std::set<std::string> boundKeys;
    for (const Binding& b : registry.bindings)
    {
        const FieldRow* row = FindField(b.key);
        if (row == nullptr)
        {
            r.errors.push_back("binding `" + b.key + "` names no GameBuild field");
            continue;
        }
        if (row->kind != FieldKind::Rva && row->kind != FieldKind::Guard && row->kind != FieldKind::Offset)
        {
            r.errors.push_back("binding `" + b.key + "` is a " + KindName(row->kind) + " field: the codegen rewrites Rva, Offset and Guard fields only");
            continue;
        }
        if (!boundKeys.insert(b.key).second)
        {
            r.errors.push_back("two bindings fill `" + b.key + "`");
            continue;
        }
        Plan pl;
        pl.row = row;
        pl.key = b.key;
        pl.group = row->group;
        pl.binding = &b;
        pl.seam = rb.Find(b.seam);
        pl.usable = pl.seam != nullptr && IsUsable(pl.seam->status);
        std::string why;
        pl.member = Lookup(p, pl.group, row->name, &why);
        if (pl.member == nullptr)
        {
            r.errors.push_back(b.key + ": " + why);
            continue;
        }
        if (row->kind == FieldKind::Guard)
        {
            if (!pl.member->simple || !pl.member->array || pl.member->declared != row->size || pl.member->numbers.size() != row->size)
            {
                r.errors.push_back(b.key + ": the header must declare std::array<std::uint8_t, " + std::to_string(row->size) + "> with exactly " +
                                   std::to_string(row->size) + " integer literals (found " + (pl.member->array ? "N=" + std::to_string(pl.member->declared) + ", " : std::string()) +
                                   std::to_string(pl.member->numbers.size()) + " literal" + (pl.member->numbers.size() == 1 ? "" : "s") + ")");
                continue;
            }
        }
        else if (!pl.member->simple || pl.member->array || pl.member->numbers.size() != 1)
        {
            r.errors.push_back(b.key + ": the header must declare `std::uint32_t " + row->name + " = <one integer literal>;`");
            continue;
        }
        plans.push_back(pl);
    }
    r.boundFields = plans.size();
    const Member* identityMember[2] = { nullptr, nullptr };
    for (int k = 0; k < 2; ++k)
    {
        std::string why;
        identityMember[k] = Lookup(p, "", identities[k].name, &why);
        if (identityMember[k] == nullptr)
            r.errors.push_back(std::string(identities[k].name) + ": " + why);
        else if (!identityMember[k]->simple || identityMember[k]->array)
            r.errors.push_back(std::string(identities[k].name) + ": the header must declare `std::uint32_t " + identities[k].name + " = <one integer literal>;`");
    }

    // ---- not verified list (computed for every outcome: a refusal shows what a partial emit would leave) --------------------------------
    std::set<std::string> verifiedKeys, unresolvedKeys;
    for (const Plan& pl : plans)
        (pl.usable ? verifiedKeys : unresolvedKeys).insert(pl.key);
    for (const FieldRow& row : kFieldTable)
    {
        char keyBuf[96];
        if (FieldKey(row, keyBuf, sizeof(keyBuf)) == 0)
            continue;
        const std::string key = keyBuf;
        if (verifiedKeys.count(key) != 0)
            continue;
        NotVerifiedField nv;
        nv.key = key;
        nv.kind = KindName(row.kind);
        nv.value = HeaderValue(p, row, row.group);
        if (unresolvedKeys.count(key) != 0)
            nv.why = "a seam is bound to it but did not resolve: the old text is kept";
        else if (row.kind == FieldKind::Rva || row.kind == FieldKind::Guard)
            nv.why = "no seam binds it: the resolver cannot derive it";
        else if (row.kind == FieldKind::Offset)
            nv.why = "fixed struct offset, NOT self-healing: no seam derives it";
        else if (row.kind == FieldKind::Pattern)
            nv.why = "the CharaView anchor itself (pattern/mask): never derived";
        else
        {
            nv.why = "size, count or limit: never derived";
            for (const ConstantSeam& cs : kConstantSeams)
            {
                if (key == cs.key)
                {
                    const SeamResult* s = rb.Find(cs.seam);
                    nv.why = std::string("a constant; seam ") + cs.seam + " is " + (s ? StatusName(s->status) : "missing") +
                             " (it checks the executable against the value compiled into the registry, not against this header)";
                }
            }
        }
        r.notVerified.push_back(nv);
    }

    for (const Plan& pl : plans)
    {
        if (pl.usable)
            continue;
        UnresolvedBinding u;
        u.key = pl.key;
        u.seam = pl.binding->seam;
        u.status = pl.seam != nullptr ? StatusName(pl.seam->status) : "missing";
        u.reason = pl.seam != nullptr ? pl.seam->reason : "the result has no such seam";
        r.unresolved.push_back(u);
    }

    if (!r.errors.empty())
    {
        r.outcome = EmitResult::Outcome::Error;
        return r;
    }
    if (!r.unresolved.empty() && !allowPartial)
    {
        r.outcome = EmitResult::Outcome::Refused;
        return r;
    }

    // ---- plan the edits ----------------------------------------------------------------------------------------------------------------
    std::vector<Edit> edits;
    std::vector<std::pair<std::string, std::string>> oldTexts; // key, old literal text, for the stale-comment search
    auto planNumber = [&](std::size_t tokenIndex, std::uint64_t wanted, unsigned minDigits, const std::string& key, std::string* oldOut, std::string* newOut) -> bool
    {
        const std::string old = TokText(p, tokenIndex);
        std::uint64_t value = 0;
        bool hex = false;
        std::string sfx;
        if (!ParseNumber(old, value, hex, sfx))
        {
            r.errors.push_back(key + ": the literal `" + old + "` is not an integer");
            return false;
        }
        if (value == wanted)
            return false;
        const std::string text = FormatLike(hex, sfx, wanted, minDigits);
        edits.push_back({ p.toks[tokenIndex].begin, p.toks[tokenIndex].end, text });
        if (oldOut)
            *oldOut = old;
        if (newOut)
            *newOut = text;
        oldTexts.emplace_back(key, old);
        return true;
    };

    for (int k = 0; k < 2; ++k)
    {
        std::string o, n;
        if (planNumber(identityMember[k]->numbers[0], identities[k].value, 1, identities[k].name, &o, &n))
            r.changed.push_back({ identities[k].name, o, n });
    }

    std::vector<std::pair<std::size_t, const char*>> tails; // semi offset, wanted mark (nullptr = none)
    std::vector<std::string> tailKeys;
    for (const Plan& pl : plans)
    {
        const char* mark = nullptr;
        if (!pl.usable)
            mark = kMarkNotVerified;
        else
        {
            if (pl.seam->viaWeakDependency)
                mark = kMarkWeakDependency;
            else if (pl.seam->status == SeamStatus::FoundSingleAnchor)
                mark = kMarkSingleAnchor;
            if (pl.row->kind == FieldKind::Guard)
            {
                const std::uint8_t* wantedBytes = FieldBytes(rb.build, *pl.row);
                std::size_t differ = 0;
                for (std::size_t i = 0; i < pl.member->numbers.size(); ++i)
                {
                    std::uint64_t v = 0;
                    bool hex = false;
                    std::string sfx;
                    if (!ParseNumber(TokText(p, pl.member->numbers[i]), v, hex, sfx) || v > 0xff)
                    {
                        r.errors.push_back(pl.key + ": byte " + std::to_string(i) + " `" + TokText(p, pl.member->numbers[i]) + "` is not a byte literal");
                        continue;
                    }
                    if (planNumber(pl.member->numbers[i], wantedBytes[i], 2, pl.key, nullptr, nullptr))
                        ++differ;
                }
                if (differ != 0)
                    r.changed.push_back({ pl.key, std::to_string(differ) + " of " + std::to_string(pl.member->numbers.size()) + " bytes differ", "the resolved bytes" });
                else
                    r.confirmed.push_back(pl.key);
            }
            else
            {
                std::string o, n;
                if (planNumber(pl.member->numbers[0], *FieldU32(rb.build, *pl.row), pl.row->kind == FieldKind::Offset ? 2u : 1u, pl.key, &o, &n))
                    r.changed.push_back({ pl.key, o, n });
                else
                    r.confirmed.push_back(pl.key);
            }
        }
        tails.emplace_back(pl.member->semi, mark);
        tailKeys.push_back(pl.key);
        if (mark != nullptr)
            r.marked.push_back(pl.key);
    }

    for (std::size_t t = 0; t < tails.size(); ++t)
    {
        const std::size_t semi = tails[t].first;
        std::size_t lineEnd = header.find_first_of("\r\n", semi);
        if (lineEnd == std::string::npos)
            lineEnd = header.size();
        const std::string tail = header.substr(semi, lineEnd - semi);
        std::string base = tail;
        for (const char* m : { kMarkNotVerified, kMarkSingleAnchor, kMarkWeakDependency })
        {
            const std::string needle = std::string(" ") + m;
            for (std::size_t pos = base.find(needle); pos != std::string::npos; pos = base.find(needle))
                base.erase(pos, needle.size());
        }
        std::string wanted = base;
        if (tails[t].second != nullptr)
            wanted += std::string(" ") + tails[t].second;
        if (wanted == tail)
            continue;
        const std::size_t firstText = base.find_first_not_of(" \t");
        if (firstText != std::string::npos && base.compare(firstText, 2, "//") != 0)
        {
            r.errors.push_back(tailKeys[t] + ": another declaration follows it on the same line, a trailing comment cannot be placed");
            continue;
        }
        edits.push_back({ semi, lineEnd, wanted });
    }

    if (!r.errors.empty())
    {
        r.outcome = EmitResult::Outcome::Error;
        r.changed.clear();
        return r;
    }

    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin < b.begin; });
    for (std::size_t i = 1; i < edits.size(); ++i)
    {
        if (edits[i - 1].end > edits[i].begin)
        {
            r.errors.push_back("internal error: two edits overlap");
            r.outcome = EmitResult::Outcome::Error;
            return r;
        }
    }
    std::string out = header;
    for (std::size_t i = edits.size(); i-- > 0;)
        out.replace(edits[i].begin, edits[i].end - edits[i].begin, edits[i].text);
    r.text = out;
    r.outcome = r.unresolved.empty() ? EmitResult::Outcome::Emitted : EmitResult::Outcome::EmittedPartial;
    std::sort(r.marked.begin(), r.marked.end());

    // ---- comments that still quote an old value --------------------------------------------------------------------------------------
    std::size_t lineNo = 1;
    for (std::size_t pos = 0; pos <= out.size();)
    {
        std::size_t e = out.find('\n', pos);
        if (e == std::string::npos)
            e = out.size();
        const std::string line = out.substr(pos, e - pos);
        const std::size_t slash = line.find("//");
        if (slash != std::string::npos)
        {
            const std::string comment = line.substr(slash);
            for (const auto& ot : oldTexts)
            {
                if (ot.second.size() < 5)
                    continue;
                for (std::size_t at = comment.find(ot.second); at != std::string::npos; at = comment.find(ot.second, at + 1))
                {
                    const bool before = at == 0 || !IsIdentChar(comment[at - 1]);
                    const bool after = at + ot.second.size() >= comment.size() || !IsIdentChar(comment[at + ot.second.size()]);
                    if (before && after)
                    {
                        r.staleMentions.push_back("line " + std::to_string(lineNo) + ": a comment quotes " + ot.second + ", the old value of " + ot.first);
                        break;
                    }
                }
            }
        }
        ++lineNo;
        pos = e + 1;
    }
    return r;
}

inline const char* OutcomeName(EmitResult::Outcome o)
{
    switch (o)
    {
    case EmitResult::Outcome::Emitted:
        return "EMITTED (every bound field re-verified)";
    case EmitResult::Outcome::EmittedPartial:
        return "EMITTED PARTIAL (--allow-partial): the unresolved fields keep their OLD text and are marked";
    case EmitResult::Outcome::Refused:
        return "REFUSED: bound fields are unresolved on this executable (--allow-partial emits anyway, marked)";
    case EmitResult::Outcome::Error:
        return "ERROR: the header or the result cannot be used";
    }
    return "?";
}

// The human report of one codegen run (printed to stderr by the CLI so that stdout can carry the header). Every section is always present.
inline std::string FormatEmitReport(const EmitResult& r, const ResolvedBuild& rb, bool allowPartial)
{
    std::string s;
    s += "== gamebuild emit ==\n";
    s += std::string("result: ") + OutcomeName(r.outcome) + "\n";
    s += "image: timeDateStamp=" + emit_detail::Hex(rb.timeDateStamp) + " sizeOfImage=" + emit_detail::Hex(rb.sizeOfImage) + "\n";
    s += "bound fields: " + std::to_string(r.boundFields) + " (rewritten from the resolver: " + std::to_string(r.changed.size()) + " changed entries, " +
         std::to_string(r.confirmed.size()) + " confirmed unchanged, " + std::to_string(r.unresolved.size()) + " unresolved)\n";
    for (const std::string& e : r.errors)
        s += "ERROR: " + e + "\n";

    s += "-- seam status table (pattern search without the table; status, value, anchors that hit) --\n";
    for (const SeamResult& seam : rb.seams)
    {
        char buf[320];
        std::string hits;
        for (std::size_t i = 0; i < seam.anchors.size(); ++i)
            hits += (i ? "+" : "") + std::to_string(seam.anchors[i].candidates);
        std::snprintf(buf, sizeof(buf), "  %-18s %-40s %s hits=%s%s%s\n", StatusName(seam.status), seam.id.c_str(),
                      seam.hasValue ? emit_detail::Hex(seam.rva).c_str() : "n/a", hits.empty() ? "-" : hits.c_str(), seam.viaWeakDependency ? " via-weak-dependency" : "",
                      seam.reason.empty() ? "" : (" # " + seam.reason).c_str());
        s += buf;
    }
    s += "  found=" + std::to_string(rb.Count(SeamStatus::Found)) + " single=" + std::to_string(rb.Count(SeamStatus::FoundSingleAnchor)) +
         " notfound=" + std::to_string(rb.Count(SeamStatus::NotFound)) + " ambiguous=" + std::to_string(rb.Count(SeamStatus::Ambiguous)) +
         " conflict=" + std::to_string(rb.Count(SeamStatus::Conflict)) + " patched=" + std::to_string(rb.Count(SeamStatus::EntryPatched)) + "\n";

    if (!r.unresolved.empty())
    {
        s += std::string("-- UNRESOLVED bound fields (") + std::to_string(r.unresolved.size()) + (allowPartial ? ", old text kept and marked) --\n" : ") --\n");
        for (const UnresolvedBinding& u : r.unresolved)
            s += "  " + u.key + "  <- seam " + u.seam + ": " + u.status + (u.reason.empty() ? "" : " (" + u.reason + ")") + "\n";
    }
    if (!r.changed.empty())
    {
        s += "-- values that CHANGED against the input header (review each against the old build) --\n";
        for (const ChangedField& c : r.changed)
            s += "  " + c.key + ": " + c.oldText + " -> " + c.newText + "\n";
    }
    if (!r.marked.empty())
    {
        s += "-- fields carrying a mark in the output --\n";
        for (const std::string& k : r.marked)
            s += "  " + k + "\n";
    }

    s += "== NOT SELF-HEALING / NOT RE-VERIFIED ==\n";
    s += "The resolver cannot derive the fields below. The output header still holds the values they had in the INPUT header (the old build's) and NONE of\n"
         "them was verified against this executable. A human must review every one of them before the entry is pinned: decompile the code that reads\n"
         "them, or run the game and read the log.\n";
    const char* kinds[] = { "Rva", "Guard", "Offset", "Constant", "Pattern" };
    for (const char* kind : kinds)
    {
        std::size_t n = 0;
        for (const NotVerifiedField& v : r.notVerified)
            n += v.kind == kind ? 1 : 0;
        if (n == 0)
            continue;
        s += std::string("-- ") + kind + " fields (" + std::to_string(n) + ") --\n";
        for (const NotVerifiedField& v : r.notVerified)
        {
            if (v.kind == kind)
                s += "  " + v.key + " = " + v.value + "   [" + v.why + "]\n";
        }
    }
    if (r.notVerified.empty())
        s += "(every field of the table is bound to a usable seam)\n";
    s += "Comments of the header are copied unchanged. Descriptions that quote the old build's RVAs, hashes or dates (the JitterSourceBuild and CharaViewBuild\n"
         "headers, the SHA-256 and the game version) were NOT updated: edit them by hand.\n";
    for (const std::string& m : r.staleMentions)
        s += "  stale comment: " + m + "\n";

    s += "== SHADER CATALOGS: NOT VERIFIABLE OFFLINE ==\n";
    s += "The game's shaders live in its data archives, not in the executable, so no tool can check the CRC catalogs against the new build before it runs.\n"
         "Only the in-game log shows their health: the last [ffxiv-catalog] summary line (";
    for (std::size_t c = 0; c < ext::ffxiv::catalog::kCatalogCount; ++c)
        s += std::string(c ? ", " : "") + ext::ffxiv::catalog::kInfo[c].name + " n/" + std::to_string(ext::ffxiv::catalog::kInfo[c].count);
    s += "), its 'first seen', 'missing' and\n"
         "'near-miss' lines, and the verdict at its end ('UI boundary ok', or 'UI boundary FALLBACK'). A healthy known build reads UI " +
         std::to_string(ext::ffxiv::catalog::kUiCatalogHealthyMin) + "/" + std::to_string(ext::ffxiv::catalog::kInfo[0].count) + " and 'UI boundary ok'.\n";
    return s;
}
} // namespace ext::ffxiv::resolve::offline
