// The companion registry pinned as text, independently of GameBuild.h: the exact pattern text, section class,
// hit counts, value rule, decode, entry flag, margin, predicates and guard of EVERY anchor of AddCompanionAnchors, and every binding and
// mustDiffer group. Over-wildcarding passes every other gate (the margin is a floor, a unique hit and a right value do not see a mask that is
// too wide), a changed fixed byte passes if the table still agrees: this literal table is what stops both. A later cleanup removes the old arrays of
// GameBuild.h; these lines then stay the only pin of the patterns.
//
// A change of an anchor means editing a line here on purpose. The file also runs with the argument --dump to print the lines for pasting.
#include "resolve_companion_bytes.h"

#include <cstdio>
#include <cstring>

using namespace rt;
using namespace cb;

namespace
{
using ext::ffxiv::resolve::AnchorSpec;

const char* KindName(SeamKind k)
{
    switch (k)
    {
    case SeamKind::Function:
        return "Function";
    case SeamKind::CodeSite:
        return "CodeSite";
    case SeamKind::Global:
        return "Global";
    case SeamKind::Table:
        return "Table";
    case SeamKind::Offset:
        return "Offset";
    }
    return "?";
}

std::string Hex(std::uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%x", v);
    return b;
}

std::string Describe(const std::string& seam, SeamKind kind, const AnchorSpec& a)
{
    const char* in = a.searchIn == SearchIn::Code ? "Code" : a.searchIn == SearchIn::ReadOnlyData ? "RO" : "Init";
    const char* from = a.valueFrom == ValueFrom::Match ? "Match" : a.valueFrom == ValueFrom::EnclosingFunction ? "Enclosing" : "Target";
    const char* dk = "None";
    switch (a.decode.kind)
    {
    case DecodeKind::RipDisp32:
        dk = "RipDisp32";
        break;
    case DecodeKind::Rel32:
        dk = "Rel32";
        break;
    case DecodeKind::Field:
        dk = "Field";
        break;
    case DecodeKind::RipDeref32:
        dk = "RipDeref32";
        break;
    case DecodeKind::None:
        break;
    }
    std::string preds;
    for (const Predicate& p : a.predicates)
    {
        if (!preds.empty())
            preds += ";";
        switch (p.kind)
        {
        case PredicateKind::TargetIn:
            preds += std::string("TargetIn:") + (p.region == RegionKind::Bss ? "Bss" : p.region == RegionKind::Code ? "Code" : "Other");
            break;
        case PredicateKind::TargetIsSeam:
            preds += "TargetIs:" + p.seam;
            break;
        case PredicateKind::CallsSeam:
            preds += "CallsSeam:" + p.seam;
            break;
        case PredicateKind::Within:
            preds += "Within:" + p.seam + "," + std::to_string(p.lo) + ".." + std::to_string(p.hi);
            break;
        case PredicateKind::SameFunctionAs:
            preds += "SameFunctionAs:" + p.seam;
            break;
        case PredicateKind::ValueIn:
            preds += "ValueIn:" + Hex(p.valueLo) + ".." + Hex(p.valueHi);
            break;
        }
    }
    char line[2048];
    std::snprintf(line, sizeof(line), "%s|%s|%s|%s|in=%s|hits=%u/%u|from=%s@%d|decode=%s:%d,%u,%u,%u|entry=%d|margin=%u|pred=%s|guard=%s@%d", seam.c_str(),
                  KindName(kind), a.name.c_str(), a.pattern.c_str(), in, a.expectedHits, a.pickIndex, from, a.valueOffset, dk, a.decode.at,
                  static_cast<unsigned>(a.decode.dispAt), static_cast<unsigned>(a.decode.length), static_cast<unsigned>(a.decode.width), a.entryAnchor ? 1 : 0,
                  a.minMargin, preds.c_str(), a.fromSeam.c_str(), a.fromOffset);
    return line;
}

std::vector<std::string> DescribeAll(const Registry& reg)
{
    std::vector<std::string> out;
    for (const SeamSpec& s : reg.seams)
    {
        for (const AnchorSpec& a : s.anchors)
            out.push_back(Describe(s.id, s.kind, a));
    }
    for (const Binding& b : reg.bindings)
        out.push_back("bind|" + b.key + "|" + b.seam + "|guardOffset=" + std::to_string(b.guardOffset) + "|valueOffset=" + std::to_string(b.valueOffset));
    for (const auto& group : reg.mustDiffer)
    {
        std::string line = "mustDiffer";
        for (const std::string& id : group)
            line += "|" + id;
        out.push_back(line);
    }
    return out;
}

// The pinned registry, one line per anchor (see Describe) then the bindings and the must-differ groups. Generated once with --dump from the
// registry the tests below were written against, then checked by hand against the executable.
const char* const kExpected[] = {
    "companion.batch16|Function|entryToIndexShift|88 54 24 10 48 89 4C 24 08 55 53 56 41 54 41 55 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 48 8B 81 80 05 00 00 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ?? 48 8B D8 48 89 44 24 58 4C 8B F8 48 89 44 24 38 4C 8B E0 49 8B 85 60 05 00 00 48 D1 E8 48 89 44 24 50 33 C0|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=1|margin=3|pred=|guard=@0",
    "companion.batch16|Function|dispatcherCall|48 8B 83 E0 01 00 00 40 0F B6 D5 48 8B CB 83 78 3C 02 75 07 E8 ?? ?? ?? ?? EB 05 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Rel32:27,0,0,0|entry=0|margin=3|pred=TargetIn:Code|guard=@0",
    "companion.batch16|Function|dispatcherStub|48 8B 81 E0 01 00 00 83 78 3C 02 0F 84 ?? ?? ?? ?? E9 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Rel32:17,0,0,0|entry=0|margin=3|pred=TargetIn:Code|guard=@0",
    "companion.batch32|Function|entryToIndexShift|88 54 24 10 48 89 4C 24 08 55 53 56 41 54 41 55 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 48 8B 81 80 05 00 00 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ?? 48 8B D8 48 89 44 24 58 4C 8B F8 48 89 44 24 38 4C 8B E0 49 8B 85 60 05 00 00 48 C1 E8 02 48 89 44 24 50 33 C0|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=1|margin=3|pred=|guard=@0",
    "companion.batch32|Function|dispatcherCall|48 8B 83 E0 01 00 00 40 0F B6 D5 48 8B CB 83 78 3C 02 75 07 E8 ?? ?? ?? ?? EB 05 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Rel32:20,0,0,0|entry=0|margin=3|pred=TargetIn:Code|guard=@0",
    "companion.batch16Shape|CodeSite|shape|88 54 24 10 48 89 4C 24 08 55 53 56 41 54 41 55 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 48 8B 81 80 05 00 00 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ?? 48 8B D8 48 89 44 24 58 4C 8B F8 48 89 44 24 38 4C 8B E0 49 8B 85 60 05 00 00 48 D1 E8 48 89 44 24 50 33 C0|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=companion.batch16@0",
    "companion.batch32Shape|CodeSite|shape|88 54 24 10 48 89 4C 24 08 55 53 56 41 54 41 55 41 57 48 8D 6C 24 D8 48 81 EC 28 01 00 00 48 8B 81 80 05 00 00 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ?? 48 8B D8 48 89 44 24 58 4C 8B F8 48 89 44 24 38 4C 8B E0 49 8B 85 60 05 00 00 48 C1 E8 02 48 89 44 24 50 33 C0|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=companion.batch32@0",
    "companion.enqueue|Function|entry|48 63 41 0C 4C 8B DA 4C 8D 04 40 8B 41 08 42 83|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=1|margin=3|pred=|guard=@0",
    "companion.enqueue|Function|entryWriteTail|41 C7 41 28 FF 03 00 00 49 8B 51 20 48 8D 4A 10 49 89 49 20 89 02 4C 89 5A 08 C3|in=Code|hits=1/0|from=Match@-155|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=@0",
    "companion.enqueue|Function|callerEpilogue|48 8B D7 4C 89 77 30 48 8B CE E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B 6C 24 38 48 8B 74 24 40 48 8B 7C 24 48 48 83 C4 20 41 5E C3|in=Code|hits=1/0|from=Target@0|decode=Rel32:10,0,0,0|entry=0|margin=3|pred=TargetIn:Code|guard=@0",
    "companion.enqueueShape|CodeSite|shape|48 63 41 0C 4C 8B DA 4C 8D 04 40 8B 41 08 42 83|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=companion.enqueue@0",
    "companion.bindState|Function|entry|40 56 41 56 48 83 EC 48 80 7A 49 00 4C 8B F2 48|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=1|margin=3|pred=|guard=@0",
    "companion.bindState|Function|argumentMapping|40 56 41 56 48 83 EC 48 80 7A 49 00 4C 8B F2 48 89 5C 24 70 48 8B F1 48 89 6C 24 78 4C 89 64 24 38 45 8B E0 4C 89 7C 24 30 0F 84 ?? ?? ?? ??|in=Code|hits=1/0|from=Enclosing@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=@0",
    "companion.bindState|Function|contextCall|83 7B 3C 02 BA 39 00 00 00 48 8B 8E ?? ?? ?? ?? 41 B8 2A 00 00 00 44 0F 45 C2 8B 15 ?? ?? ?? ?? 45 33 C9 48 8B 01 48 8B 54 D3 48 FF 90 98 00 00 00|in=Code|hits=1/0|from=Enclosing@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=@0",
    "companion.bindStateShape|CodeSite|shape|40 56 41 56 48 83 EC 48 80 7A 49 00 4C 8B F2 48 89 5C 24 70 48 8B F1 48 89 6C 24 78 4C 89 64 24 38 45 8B E0 4C 89 7C 24 30 0F 84 ?? ?? ?? ??|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=companion.bindState@0",
    "companion.nameplateModulePointer|Global|chainKeySet|48 8B 05 ?? ?? ?? ?? 0F B6 91 EF 01 00 00 48 8B 48 08 48 8B 41 08 89 90 90 00 00 00|in=Code|hits=1/0|from=Target@0|decode=RipDisp32:0,3,7,0|entry=0|margin=3|pred=TargetIn:Bss|guard=@0",
    "companion.nameplateModulePointer|Global|chainKeyReset|48 8B 05 ?? ?? ?? ?? 48 8B 48 08 48 8B 41 08 C7 80 90 00 00 00 00 00 00 00|in=Code|hits=1/0|from=Target@0|decode=RipDisp32:0,3,7,0|entry=0|margin=3|pred=TargetIn:Bss|guard=@0",
    "companion.nameplateModulePointer|Global|chainSecondary|48 8B 05 ?? ?? ?? ?? 48 8B 58 08 0F 85 ?? ?? ?? ?? 48 8B 4B 08|in=Code|hits=1/0|from=Target@0|decode=RipDisp32:0,3,7,0|entry=0|margin=3|pred=TargetIn:Bss|guard=@0",
    "companion.moduleStatePointerOffset|Offset|chainKeySet|48 8B 05 ?? ?? ?? ?? 0F B6 91 EF 01 00 00 48 8B 48 08 48 8B 41 08 89 90 90 00 00 00|in=Code|hits=1/0|from=Target@0|decode=Field:17,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f|guard=@0",
    "companion.moduleStatePointerOffset|Offset|chainKeyReset|48 8B 05 ?? ?? ?? ?? 48 8B 48 08 48 8B 41 08 C7 80 90 00 00 00 00 00 00 00|in=Code|hits=1/0|from=Target@0|decode=Field:10,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f|guard=@0",
    "companion.moduleStatePointerOffset|Offset|chainSecondary|48 8B 05 ?? ?? ?? ?? 48 8B 58 08 0F 85 ?? ?? ?? ?? 48 8B 4B 08|in=Code|hits=1/0|from=Target@0|decode=Field:10,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f|guard=@0",
    "companion.stateQueuePointerOffset|Offset|chainKeySet|48 8B 05 ?? ?? ?? ?? 0F B6 91 EF 01 00 00 48 8B 48 08 48 8B 41 08 89 90 90 00 00 00|in=Code|hits=1/0|from=Target@0|decode=Field:21,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f|guard=@0",
    "companion.stateQueuePointerOffset|Offset|chainKeyReset|48 8B 05 ?? ?? ?? ?? 48 8B 48 08 48 8B 41 08 C7 80 90 00 00 00 00 00 00 00|in=Code|hits=1/0|from=Target@0|decode=Field:14,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f|guard=@0",
    "companion.stateQueuePointerOffset|Offset|chainSecondary|48 8B 05 ?? ?? ?? ?? 48 8B 58 08 0F 85 ?? ?? ?? ?? 48 8B 4B 08|in=Code|hits=1/0|from=Target@0|decode=Field:20,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f|guard=@0",
    "companion.layoutSlotTable|Table|inBatchA|48 8D 0D ?? ?? ?? ?? 48 63 8C D9 ?? ?? ?? ?? 48 8B C1 48 03 C0 41 8B 84 C5 ?? ?? ?? ?? 49 03 85 ?? ?? ?? ?? 48 89 45 88 48 8D 41 50 48 03 C0 41 8B 4C C5 00|in=Code|hits=2/0|from=Target@0|decode=Field:11,0,0,4|entry=0|margin=3|pred=ValueIn:100000..6000000|guard=@0",
    "companion.layoutSlotTable|Table|inBatchB|48 8D 0D ?? ?? ?? ?? 48 63 8C D9 ?? ?? ?? ?? 48 8B C1 48 03 C0 41 8B 84 C5 ?? ?? ?? ?? 49 03 85 ?? ?? ?? ?? 48 89 45 88 48 8D 41 50 48 03 C0 41 8B 4C C5 00|in=Code|hits=2/1|from=Target@0|decode=Field:11,0,0,4|entry=0|margin=3|pred=ValueIn:100000..6000000|guard=@0",
    "companion.layoutSlotTable|Table|content|00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 05 00 00 00 02 00 00 00 04 00 00 00 02 00 00 00 03 00 00 00 01 00 00 00 02 00 00 00 01 00 00 00 01 00 00 00|in=RO|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=@0",
    "companion.indexBufferMap|Function|entry|48 89 5C 24 08 57 48 83 EC 20 48 8B D9 8B 49 38 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B 40 F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ??|in=Code|hits=1/0|from=Match@0|decode=None:0,0,0,0|entry=1|margin=3|pred=|guard=@0",
    "companion.indexBufferMap|Function|calledFromBatchA|41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=2/0|from=Target@0|decode=Rel32:28,0,0,0|entry=0|margin=3|pred=TargetIn:Code|guard=@0",
    "companion.indexBufferMap|Function|calledFromBatchB|41 B9 02 00 00 00 8B 81 88 05 00 00 45 33 C0 48 8B 89 E0 01 00 00 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=2/1|from=Target@0|decode=Rel32:28,0,0,0|entry=0|margin=3|pred=TargetIn:Code|guard=@0",
    "companion.indexBufferMap|Function|flagLogic|8B 49 ?? 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B ?? F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 48 83 7C C3 48 00 75 07 48 83 7B ?? 00 74 72 F6 C1 11 74 23 F6 C1 40 74 0F 48 8B 43 ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 43 ??|in=Code|hits=1/0|from=Enclosing@0|decode=None:0,0,0,0|entry=0|margin=3|pred=|guard=@0",
    "companion.rendererBatchListOffset|Offset|inBatch16|48 8B 81 ?? ?? ?? ?? 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 ?? ?? ?? ?? 45 33 C0 48 8B 89 ?? ?? ?? ?? 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Field:3,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch16|guard=@0",
    "companion.rendererBatchListOffset|Offset|inBatch32|48 8B 81 ?? ?? ?? ?? 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 ?? ?? ?? ?? 45 33 C0 48 8B 89 ?? ?? ?? ?? 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Field:3,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch32|guard=@0",
    "companion.rendererBatchCountOffset|Offset|inBatch16|48 8B 81 ?? ?? ?? ?? 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 ?? ?? ?? ?? 45 33 C0 48 8B 89 ?? ?? ?? ?? 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Field:22,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch16|guard=@0",
    "companion.rendererBatchCountOffset|Offset|inBatch32|48 8B 81 ?? ?? ?? ?? 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 ?? ?? ?? ?? 45 33 C0 48 8B 89 ?? ?? ?? ?? 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Field:22,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch32|guard=@0",
    "companion.rendererIndexBufferOffset|Offset|inBatch16|48 8B 81 ?? ?? ?? ?? 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 ?? ?? ?? ?? 45 33 C0 48 8B 89 ?? ?? ?? ?? 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Field:32,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch16|guard=@0",
    "companion.rendererIndexBufferOffset|Offset|inBatch32|48 8B 81 ?? ?? ?? ?? 4C 8B E9 48 89 45 E8 41 B9 02 00 00 00 8B 81 ?? ?? ?? ?? 45 33 C0 48 8B 89 ?? ?? ?? ?? 33 D2 89 44 24 68 E8 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Field:32,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch32|guard=@0",
    "companion.rendererVertexOffsetOffset|Offset|inBatch16|33 C9 41 8B 95 ?? ?? ?? ?? 49 03 95 ?? ?? ?? ?? 45 8B 9D ?? ?? ?? ?? 48 89 55 88 4C 89 5D 98 89 4D 78|in=Code|hits=1/0|from=Target@0|decode=Field:5,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch16|guard=@0",
    "companion.rendererVertexOffsetOffset|Offset|inBatch32|33 C9 41 8B 95 ?? ?? ?? ?? 49 03 95 ?? ?? ?? ?? 45 8B 9D ?? ?? ?? ?? 48 89 55 88 4C 89 5D 98 89 4D 78|in=Code|hits=1/0|from=Target@0|decode=Field:5,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch32|guard=@0",
    "companion.rendererVertexAllocationOffset|Offset|inBatch16|33 C9 41 8B 95 ?? ?? ?? ?? 49 03 95 ?? ?? ?? ?? 45 8B 9D ?? ?? ?? ?? 48 89 55 88 4C 89 5D 98 89 4D 78|in=Code|hits=1/0|from=Target@0|decode=Field:12,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch16|guard=@0",
    "companion.rendererVertexAllocationOffset|Offset|inBatch32|33 C9 41 8B 95 ?? ?? ?? ?? 49 03 95 ?? ?? ?? ?? 45 8B 9D ?? ?? ?? ?? 48 89 55 88 4C 89 5D 98 89 4D 78|in=Code|hits=1/0|from=Target@0|decode=Field:12,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch32|guard=@0",
    "companion.rendererVertexStrideOffset|Offset|inBatch16|33 C9 41 8B 95 ?? ?? ?? ?? 49 03 95 ?? ?? ?? ?? 45 8B 9D ?? ?? ?? ?? 48 89 55 88 4C 89 5D 98 89 4D 78|in=Code|hits=1/0|from=Target@0|decode=Field:19,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch16|guard=@0",
    "companion.rendererVertexStrideOffset|Offset|inBatch32|33 C9 41 8B 95 ?? ?? ?? ?? 49 03 95 ?? ?? ?? ?? 45 8B 9D ?? ?? ?? ?? 48 89 55 88 4C 89 5D 98 89 4D 78|in=Code|hits=1/0|from=Target@0|decode=Field:19,0,0,4|entry=0|margin=0|pred=ValueIn:40..4000;SameFunctionAs:companion.batch32|guard=@0",
    "companion.batchFormatOffset|Offset|inBatch16|41 8B 4C 24 ?? 8B C1 83 E0 0F 3B 44 24 44 75 32 8B C1 C1 E8 04 0F B6 C0 3B 44 24 34|in=Code|hits=1/0|from=Target@0|decode=Field:4,0,0,1|entry=0|margin=0|pred=ValueIn:1..3c;SameFunctionAs:companion.batch16|guard=@0",
    "companion.batchFormatOffset|Offset|inBatch32|41 8B 4C 24 ?? 8B C1 83 E0 0F 3B 44 24 44 75 32 8B C1 C1 E8 04 0F B6 C0 3B 44 24 34|in=Code|hits=1/0|from=Target@0|decode=Field:4,0,0,1|entry=0|margin=0|pred=ValueIn:1..3c;SameFunctionAs:companion.batch32|guard=@0",
    "companion.batchExtensionOffset|Offset|inBatch16|66 41 83 7C 24 ?? 02 44 8B 54 24 40 40 0F 94 C7 44 3B D3|in=Code|hits=1/0|from=Target@0|decode=Field:5,0,0,1|entry=0|margin=0|pred=ValueIn:1..3e;SameFunctionAs:companion.batch16|guard=@0",
    "companion.batchExtensionOffset|Offset|inBatch32|66 41 83 7C 24 ?? 02 44 8B 54 24 40 40 0F 94 C7 44 3B D3|in=Code|hits=1/0|from=Target@0|decode=Field:5,0,0,1|entry=0|margin=0|pred=ValueIn:1..3e;SameFunctionAs:companion.batch32|guard=@0",
    "companion.drawStateFlagOffset|Offset|firstCompare|80 7A ?? 00 4C 8B F2 48 89 5C 24 ?? 48 8B F1 48 89 6C 24 ?? 4C 89 64 24 ?? 45 8B E0 4C 89 7C 24 ?? 0F 84 ?? ?? ?? ??|in=Code|hits=1/0|from=Target@0|decode=Field:2,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f;SameFunctionAs:companion.bindState|guard=@0",
    "companion.nativeContextOffset|Offset|loadBeforeVirtualCall|48 8B BE ?? ?? ?? ?? 8B D5 48 8B 07 48 8B 98 18 01 00 00|in=Code|hits=1/0|from=Target@0|decode=Field:3,0,0,4|entry=0|margin=3|pred=ValueIn:100..10000;SameFunctionAs:companion.bindState|guard=@0",
    "companion.nativeContextOffset|Offset|loadBeforeStateCall|83 7B 3C 02 BA 39 00 00 00 48 8B 8E ?? ?? ?? ?? 41 B8 2A 00 00 00 44 0F 45 C2 8B 15 ?? ?? ?? ?? 45 33 C9 48 8B 01 48 8B 54 D3 48 FF 90 98 00 00 00|in=Code|hits=1/0|from=Target@0|decode=Field:12,0,0,4|entry=0|margin=3|pred=ValueIn:100..10000;SameFunctionAs:companion.bindState|guard=@0",
    "companion.indexBufferCapacityOffset|Offset|capacityLoad|8B 49 ?? 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B ?? F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 48 83 7C C3 48 00 75 07 48 83 7B ?? 00 74 72 F6 C1 11 74 23 F6 C1 40 74 0F 48 8B 43 ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 43 ??|in=Code|hits=1/0|from=Target@0|decode=Field:2,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f;SameFunctionAs:companion.indexBufferMap|guard=@0",
    "companion.indexBufferFlagsOffset|Offset|flagsLoad|8B 49 ?? 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B ?? F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 48 83 7C C3 48 00 75 07 48 83 7B ?? 00 74 72 F6 C1 11 74 23 F6 C1 40 74 0F 48 8B 43 ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 43 ??|in=Code|hits=1/0|from=Target@0|decode=Field:28,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f;SameFunctionAs:companion.indexBufferMap|guard=@0",
    "companion.indexBufferCpuStartOffset|Offset|startCompare|8B 49 ?? 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B ?? F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 48 83 7C C3 48 00 75 07 48 83 7B ?? 00 74 72 F6 C1 11 74 23 F6 C1 40 74 0F 48 8B 43 ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 43 ??|in=Code|hits=1/0|from=Target@0|decode=Field:61,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f;SameFunctionAs:companion.indexBufferMap|guard=@0",
    "companion.indexBufferCpuStartOffset|Offset|startReturn|8B 49 ?? 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B ?? F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 48 83 7C C3 48 00 75 07 48 83 7B ?? 00 74 72 F6 C1 11 74 23 F6 C1 40 74 0F 48 8B 43 ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 43 ??|in=Code|hits=1/0|from=Target@0|decode=Field:78,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f;SameFunctionAs:companion.indexBufferMap|guard=@0",
    "companion.indexBufferCpuEndOffset|Offset|endReturn|8B 49 ?? 41 8B F8 45 85 C0 75 04 8B F9 2B FA 8D 04 3A 3B C8 0F 82 ?? ?? ?? ?? 8B 4B ?? F6 C1 03 74 0A 41 F6 C1 01 0F 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 48 83 7C C3 48 00 75 07 48 83 7B ?? 00 74 72 F6 C1 11 74 23 F6 C1 40 74 0F 48 8B 43 ?? 48 8B 5C 24 30 48 83 C4 20 5F C3 48 8B 43 ??|in=Code|hits=1/0|from=Target@0|decode=Field:93,0,0,1|entry=0|margin=3|pred=ValueIn:1..7f;SameFunctionAs:companion.indexBufferMap|guard=@0",
    "companion.entryPointerOffset|Offset|listEntryInBatch16|4C 8B 4D E8 48 03 C0 4D 8B 64 C1 ?? 4D 85 E4 0F 84 ?? ?? ?? ?? 41 8B 04 24|in=Code|hits=1/0|from=Target@0|decode=Field:11,0,0,1|entry=0|margin=0|pred=ValueIn:8..8;SameFunctionAs:companion.batch16|guard=@0",
    "companion.entryPointerOffset|Offset|listEntryInBatch32|4C 8B 4D E8 48 03 C0 4D 8B 64 C1 ?? 4D 85 E4 0F 84 ?? ?? ?? ?? 41 8B 04 24|in=Code|hits=1/0|from=Target@0|decode=Field:11,0,0,1|entry=0|margin=0|pred=ValueIn:8..8;SameFunctionAs:companion.batch32|guard=@0",
    "companion.entryPointerOffset|Offset|commandStore|49 8B 51 20 48 8D 4A 10 49 89 49 20 89 02 4C 89 5A ?? C3|in=Code|hits=1/0|from=Target@0|decode=Field:17,0,0,1|entry=0|margin=3|pred=ValueIn:8..8;Within:companion.enqueue,1..512|guard=@0",
    "bind|companion.batch16Rva|companion.batch16|guardOffset=0|valueOffset=0",
    "bind|companion.batch32Rva|companion.batch32|guardOffset=0|valueOffset=0",
    "bind|companion.batchInstructions|companion.batch16Shape|guardOffset=0|valueOffset=0",
    "bind|companion.enqueueRva|companion.enqueue|guardOffset=0|valueOffset=0",
    "bind|companion.enqueueInstructions|companion.enqueueShape|guardOffset=0|valueOffset=0",
    "bind|companion.bindStateRva|companion.bindState|guardOffset=0|valueOffset=0",
    "bind|companion.bindStateInstructions|companion.bindStateShape|guardOffset=0|valueOffset=0",
    "bind|companion.nameplateModulePointerRva|companion.nameplateModulePointer|guardOffset=0|valueOffset=0",
    "bind|companion.moduleStatePointerOffset|companion.moduleStatePointerOffset|guardOffset=0|valueOffset=0",
    "bind|companion.stateQueuePointerOffset|companion.stateQueuePointerOffset|guardOffset=0|valueOffset=0",
    "bind|companion.layoutSlotTableRva|companion.layoutSlotTable|guardOffset=0|valueOffset=0",
    "bind|companion.rendererBatchListOffset|companion.rendererBatchListOffset|guardOffset=0|valueOffset=0",
    "bind|companion.rendererBatchCountOffset|companion.rendererBatchCountOffset|guardOffset=0|valueOffset=0",
    "bind|companion.rendererIndexBufferOffset|companion.rendererIndexBufferOffset|guardOffset=0|valueOffset=0",
    "bind|companion.rendererVertexOffsetOffset|companion.rendererVertexOffsetOffset|guardOffset=0|valueOffset=0",
    "bind|companion.rendererVertexAllocationOffset|companion.rendererVertexAllocationOffset|guardOffset=0|valueOffset=0",
    "bind|companion.rendererVertexStrideOffset|companion.rendererVertexStrideOffset|guardOffset=0|valueOffset=0",
    "bind|companion.batchFormatOffset|companion.batchFormatOffset|guardOffset=0|valueOffset=0",
    "bind|companion.batchExtensionOffset|companion.batchExtensionOffset|guardOffset=0|valueOffset=0",
    "bind|companion.drawStateFlagOffset|companion.drawStateFlagOffset|guardOffset=0|valueOffset=0",
    "bind|companion.nativeContextOffset|companion.nativeContextOffset|guardOffset=0|valueOffset=0",
    "bind|companion.indexBufferCapacityOffset|companion.indexBufferCapacityOffset|guardOffset=0|valueOffset=0",
    "bind|companion.indexBufferFlagsOffset|companion.indexBufferFlagsOffset|guardOffset=0|valueOffset=0",
    "bind|companion.indexBufferCpuStartOffset|companion.indexBufferCpuStartOffset|guardOffset=0|valueOffset=0",
    "bind|companion.indexBufferCpuEndOffset|companion.indexBufferCpuEndOffset|guardOffset=0|valueOffset=0",
    "bind|companion.entryPointerOffset|companion.entryPointerOffset|guardOffset=0|valueOffset=0",
    "mustDiffer|companion.batch16|companion.batch32",
};

void TestPinnedText()
{
    const Registry reg = CompanionRegistry();
    CHECK(ValidateRegistry(reg).empty());
    const std::vector<std::string> got = DescribeAll(reg);
    const std::size_t want = sizeof(kExpected) / sizeof(kExpected[0]);
    CHECK(got.size() == want);
    std::size_t diffs = 0;
    for (std::size_t i = 0; i < std::min(got.size(), want); ++i)
    {
        if (got[i] != kExpected[i])
        {
            ++diffs;
            std::printf("FAIL: line %zu differs\n  pinned: %s\n  actual: %s\n", i, kExpected[i], got[i].c_str());
        }
    }
    CHECK(diffs == 0);
}

// The seams of this group are all companion.*, no anchor names a seam of another group (a worktree is self-contained), and the batch pair is
// declared as must-differ.
void TestSelfContained()
{
    const Registry reg = CompanionRegistry();
    for (const SeamSpec& s : reg.seams)
    {
        CHECK(s.id.rfind("companion.", 0) == 0);
        for (const AnchorSpec& a : s.anchors)
        {
            if (!a.fromSeam.empty())
                CHECK(a.fromSeam.rfind("companion.", 0) == 0);
            for (const Predicate& p : a.predicates)
            {
                if (!p.seam.empty())
                    CHECK(p.seam.rfind("companion.", 0) == 0);
            }
        }
    }
    for (const Binding& b : reg.bindings)
    {
        CHECK(b.key.rfind("companion.", 0) == 0);
        CHECK(b.seam.rfind("companion.", 0) == 0);
    }
    REQUIRE(reg.mustDiffer.size() == 1);
    REQUIRE(reg.mustDiffer[0].size() == 2);
    CHECK(reg.mustDiffer[0][0] == "companion.batch16" && reg.mustDiffer[0][1] == "companion.batch32");
}

// Every field this group binds is a field of the table, and the bindings are exactly the Rva / Guard / Offset fields listed in the header.
void TestBoundFields()
{
    const Registry reg = CompanionRegistry();
    std::vector<std::string> keys;
    for (const Binding& b : reg.bindings)
        keys.push_back(b.key);
    std::sort(keys.begin(), keys.end());
    const std::vector<std::string> want = {
        "companion.batch16Rva",
        "companion.batch32Rva",
        "companion.batchExtensionOffset",
        "companion.batchFormatOffset",
        "companion.batchInstructions",
        "companion.bindStateInstructions",
        "companion.bindStateRva",
        "companion.drawStateFlagOffset",
        "companion.enqueueInstructions",
        "companion.enqueueRva",
        "companion.entryPointerOffset",
        "companion.indexBufferCapacityOffset",
        "companion.indexBufferCpuEndOffset",
        "companion.indexBufferCpuStartOffset",
        "companion.indexBufferFlagsOffset",
        "companion.layoutSlotTableRva",
        "companion.moduleStatePointerOffset",
        "companion.nameplateModulePointerRva",
        "companion.nativeContextOffset",
        "companion.rendererBatchCountOffset",
        "companion.rendererBatchListOffset",
        "companion.rendererIndexBufferOffset",
        "companion.rendererVertexAllocationOffset",
        "companion.rendererVertexOffsetOffset",
        "companion.rendererVertexStrideOffset",
        "companion.stateQueuePointerOffset",
    };
    CHECK(keys == want);
    for (const std::string& k : keys)
        CHECK(FindField(k) != nullptr);
    // every Rva and Guard field of the companion group is bound (nothing of this group is left to the table on an unknown build)
    for (const FieldRow& row : kFieldTable)
    {
        if (std::string(row.group) != "companion" || (row.kind != FieldKind::Rva && row.kind != FieldKind::Guard))
            continue;
        const std::string key = std::string(row.group) + "." + row.name;
        CHECK(std::find(keys.begin(), keys.end(), key) != keys.end());
    }
}

// The two-tier design as structure, independent of the pinned text above. Every Detour site has a LOCATION seam (Rva fields) and
// a SHAPE seam (CodeSite, failClosed, ONE guard anchor verified at the location's address, the strict anchor text of the location seam); the Guard
// fields are bound to the shape seams, never to a location seam; failClosed is set on exactly these four seams.
void TestShapeDesign()
{
    const Registry reg = CompanionRegistry();
    const struct
    {
        const char* shape;
        const char* location;
        const char* strictAnchor; // the anchor of the location seam whose text the shape repeats
        const char* guardKey;     // "" = no Guard field (the table keeps one batch guard, bound to batch16Shape)
        const char* rvaKey;
    } sites[] = {
        { "companion.batch16Shape", "companion.batch16", "entryToIndexShift", "companion.batchInstructions", "companion.batch16Rva" },
        { "companion.batch32Shape", "companion.batch32", "entryToIndexShift", "", "companion.batch32Rva" },
        { "companion.enqueueShape", "companion.enqueue", "entry", "companion.enqueueInstructions", "companion.enqueueRva" },
        { "companion.bindStateShape", "companion.bindState", "argumentMapping", "companion.bindStateInstructions", "companion.bindStateRva" },
    };
    auto binding = [&](const std::string& key) -> const Binding*
    {
        for (const Binding& b : reg.bindings)
        {
            if (b.key == key)
                return &b;
        }
        return nullptr;
    };
    for (const auto& s : sites)
    {
        const SeamSpec* shape = FindSpec(reg, s.shape);
        const SeamSpec* loc = FindSpec(reg, s.location);
        REQUIRE(shape != nullptr && loc != nullptr);
        CHECK(shape->kind == SeamKind::CodeSite && shape->failClosed && !loc->failClosed);
        REQUIRE(shape->anchors.size() == 1); // ONE anchor: with two, a failing piece next to a passing one would read FoundSingleAnchor (usable)
        const AnchorSpec& a = shape->anchors[0];
        CHECK(a.fromSeam == s.location && a.fromOffset == 0 && a.valueOffset == 0 && a.valueFrom == ValueFrom::Match && !a.entryAnchor);
        CHECK(a.predicates.empty() && a.decode.kind == DecodeKind::None && a.expectedHits == 1 && a.searchIn == SearchIn::Code);
        bool same = false;
        for (const AnchorSpec& la : loc->anchors)
            same = same || (la.name == s.strictAnchor && la.pattern == a.pattern);
        CHECK(same);
        if (std::string(s.guardKey).empty())
        {
            for (const Binding& b : reg.bindings)
                CHECK(b.seam != s.shape);
        }
        else
        {
            const Binding* g = binding(s.guardKey);
            REQUIRE(g != nullptr);
            CHECK(g->seam == s.shape && g->guardOffset == 0);
        }
        const Binding* r = binding(s.rvaKey);
        REQUIRE(r != nullptr);
        CHECK(r->seam == s.location);
    }
    // failClosed on exactly the four shape seams, and every Guard field of the group is bound to one of them
    std::size_t closed = 0;
    for (const SeamSpec& s : reg.seams)
    {
        const bool isShape = s.id.size() > 5 && s.id.compare(s.id.size() - 5, 5, "Shape") == 0;
        CHECK(s.failClosed == isShape);
        closed += s.failClosed ? 1 : 0;
    }
    CHECK(closed == 4);
    for (const Binding& b : reg.bindings)
    {
        const FieldRow* row = FindField(b.key);
        REQUIRE(row != nullptr);
        if (row->kind == FieldKind::Guard)
        {
            const SeamSpec* s = FindSpec(reg, b.seam);
            REQUIRE(s != nullptr);
            CHECK(s->failClosed && s->kind == SeamKind::CodeSite);
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0)
    {
        for (const std::string& line : DescribeAll(CompanionRegistry()))
            std::printf("    \"%s\",\n", line.c_str());
        return 0;
    }
    TestPinnedText();
    TestSelfContained();
    TestBoundFields();
    TestShapeDesign();
    return TEST_RESULT("resolve_companion_pins");
}
