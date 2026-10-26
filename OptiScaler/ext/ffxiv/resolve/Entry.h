#pragma once
// The entry generator: the resolved build as stable `key=value` text, ready for a later codegen of a known-build table entry (the Q10
// deliverable), and the same text produced from a table, which is what the golden test compares.
//
//   timeDateStamp=0x6ad10bfd
//   sizeOfImage=0x381f000
//   charaView.accessorRva=0x26abd0                  one line per bound field, sorted by key
//   nativeQuality.querySizeInstructions=48895c24..  guards as contiguous lowercase hex, the length of the table array
//   jitterSource.phasesRva=0x25f721 # single-anchor  a value found by ONE anchor only (FoundSingleAnchor): review it before pinning it
//   jitterSource.strengthRva=0x25f799 # via-weak-dependency   a value that rests on a weak seam (EntryPatched or single-anchor)
//   # charaView.managerPointerRva unresolved: NotFound   (comment lines last: a field no usable seam filled)
// A trailing " # ..." is a comment: a value line of a plain Found seam never has one.
//
// Only the fields a Registry binds appear: the golden test therefore grows with every seam the P2 writers register.

#include "FieldTable.h"
#include "ResolveTypes.h"
#include "Resolver.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ext::ffxiv::resolve
{
namespace detail
{
inline std::string HexBytes(const std::uint8_t* p, std::size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 15]);
    }
    return s;
}

inline std::string FieldText(const GameBuild& b, const FieldRow& row)
{
    if (row.kind == FieldKind::Guard)
        return HexBytes(FieldBytes(b, row), row.size);
    return HexValue(*FieldU32(b, row));
}

inline std::string EntryHeader(std::uint32_t stamp, std::uint32_t size)
{
    return "timeDateStamp=" + HexValue(stamp) + "\nsizeOfImage=" + HexValue(size) + "\n";
}
} // namespace detail

// The resolved build as entry text. A binding whose seam did not resolve to a usable value is listed as a comment, never as a value.
inline std::string FormatEntry(const ResolvedBuild& rb, const Registry& registry)
{
    std::vector<std::pair<std::string, std::string>> lines;
    std::vector<std::string> comments;
    for (const Binding& b : registry.bindings)
    {
        const FieldRow* row = FindField(b.key);
        if (row == nullptr)
            continue;
        const SeamResult* seam = rb.Find(b.seam);
        if (seam != nullptr && IsUsable(seam->status))
        {
            // a value that was NOT confirmed by two anchors is marked: the text is meant to be pinned into a known-build table
            const char* mark = seam->status == SeamStatus::Found ? "" : seam->viaWeakDependency ? " # via-weak-dependency" : " # single-anchor";
            lines.emplace_back(b.key, detail::FieldText(rb.build, *row) + mark);
        }
        else
            comments.push_back(std::string("# ") + b.key + " unresolved: " + (seam != nullptr ? StatusName(seam->status) : "NotFound") + "\n");
    }
    std::sort(lines.begin(), lines.end());
    std::sort(comments.begin(), comments.end());
    std::string text = detail::EntryHeader(rb.timeDateStamp, rb.sizeOfImage);
    for (const auto& l : lines)
        text += l.first + "=" + l.second + "\n";
    for (const std::string& c : comments)
        text += c;
    return text;
}

// The same text for a table (kCurrentBuild or any GameBuild), restricted to the fields the registry binds.
inline std::string FormatTableEntry(const GameBuild& table, const Registry& registry)
{
    std::vector<std::pair<std::string, std::string>> lines;
    for (const Binding& b : registry.bindings)
    {
        const FieldRow* row = FindField(b.key);
        if (row != nullptr)
            lines.emplace_back(b.key, detail::FieldText(table, *row));
    }
    std::sort(lines.begin(), lines.end());
    std::string text = detail::EntryHeader(table.timeDateStamp, table.sizeOfImage);
    for (const auto& l : lines)
        text += l.first + "=" + l.second + "\n";
    return text;
}

// The Rva and Guard fields of GameBuild that no binding fills yet: the fields no seam covers, which a consumer must not treat as resolved. Offsets are not
// listed (they keep their defaults, "not self-healing"). In FieldTable order.
inline std::vector<std::string> UnboundFields(const Registry& registry)
{
    std::vector<std::string> out;
    for (const FieldRow& row : kFieldTable)
    {
        if (row.kind != FieldKind::Rva && row.kind != FieldKind::Guard)
            continue;
        char key[96];
        if (FieldKey(row, key, sizeof(key)) == 0)
            continue;
        const bool bound = std::any_of(registry.bindings.begin(), registry.bindings.end(),
                                       [&](const Binding& b) { return std::string_view(b.key) == key; });
        if (!bound)
            out.emplace_back(key);
    }
    return out;
}
} // namespace ext::ffxiv::resolve
