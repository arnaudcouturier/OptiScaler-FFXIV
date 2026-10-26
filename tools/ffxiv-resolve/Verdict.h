#pragma once
// OFFLINE ONLY (lives under tools\, never included from OptiScaler\). The pass/fail decisions of the patch-day CLI, separated from main()
// so that a test can feed them a synthetic result (a DISAGREE cannot be produced on the real executable: its table is its own build).

#include "ext/ffxiv/resolve/ResolveTypes.h"

#include <string>
#include <vector>

namespace ext::ffxiv::resolve::offline
{
// One line per reason `--strict` fails: a registry error, a seam that is not plain Found (a usable single-anchor seam, a seam that fell back
// to the table, or any unresolved status), and a seam that DISAGREES with the table (a Found seam can disagree: the table then won).
inline std::vector<std::string> StrictFailures(const ResolvedBuild& rb)
{
    std::vector<std::string> out;
    for (const std::string& e : rb.registryErrors)
        out.push_back("registry error: " + e);
    for (const SeamResult& s : rb.seams)
    {
        if (s.status != SeamStatus::Found)
            out.push_back("seam " + s.id + " is " + StatusName(s.status));
        if (s.agreement == Agreement::Disagree)
            out.push_back("seam " + s.id + " DISAGREES with the table");
    }
    return out;
}

// The seams of a result resolved WITH the table that disagree with it. `--compare-table` fails on any.
inline std::vector<std::string> DisagreeingSeams(const ResolvedBuild& rb)
{
    std::vector<std::string> out;
    for (const SeamResult& s : rb.seams)
    {
        if (s.agreement == Agreement::Disagree)
            out.push_back(s.id);
    }
    return out;
}

// The entry text without the trailing " # ..." comment of a value line (single-anchor marks). Whole-line comments (# key unresolved) stay:
// an unresolved field must show up as a difference against a table.
inline std::string StripEntryComments(const std::string& text)
{
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t end = text.find('\n', pos);
        const bool last = end == std::string::npos;
        std::string line = text.substr(pos, last ? std::string::npos : end - pos);
        if (!line.empty() && line[0] != '#')
        {
            const std::size_t mark = line.find(" # ");
            if (mark != std::string::npos)
                line.erase(mark);
        }
        out += line;
        if (!last)
            out += '\n';
        pos = last ? text.size() : end + 1;
    }
    return out;
}
} // namespace ext::ffxiv::resolve::offline
