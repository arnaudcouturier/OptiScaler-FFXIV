#pragma once
// The log lines of the resolver, and the product sink.
//
// One line per seam, fixed format (the resolver writes them through a LogSink, so tests capture them):
//   [ffxiv-resolve] <seam> <Status> rva=0x<hex>|n/a hits=<a>[+<b>...] table=0x<hex>|n/a <agree|DISAGREE|n/a> ms=<x.xxx>[ note=<why>]
// and one total line:
//   [ffxiv-resolve] total seams=N found=a single=b notfound=c ambiguous=d conflict=e patched=f scan_ms=x.xxx ms=y.yyy
//                   stamp=0x<hex> size=0x<hex> known=yes|no[ registry_errors=N]
//
// `hits` is the number of candidates each anchor left after the pattern and the predicates (so "1+0" = two anchors, the first found
// one, the second none). `ms` of a seam is its own evaluation time; the one image scan all seams share is `scan_ms` in the total.
// A seam is logged at Info only when it is Found and (if a known table was supplied) agrees with it; every other status, and every
// DISAGREE, is a Warn. A registry error is an Error line before the seams.
//
// DefaultLogSink / MakeDefaultSink exist only where the LOG_INFO / LOG_WARN / LOG_ERROR macros are visible when this header is first
// included (the product's pch.h defines them; a test defines its own capturing macros first). The core headers never use the macros.

#include "ResolveTypes.h"

#include <cstdio>
#include <string>

namespace ext::ffxiv::resolve
{
namespace detail
{
inline std::string HexValue(std::uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}
inline std::string Millis(std::uint64_t micros)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(micros) / 1000.0);
    return buf;
}
} // namespace detail

inline std::string FormatSeamLine(const SeamResult& s)
{
    std::string hits;
    for (const AnchorResult& a : s.anchors)
    {
        if (!hits.empty())
            hits += '+';
        hits += std::to_string(a.candidates);
    }
    if (hits.empty())
        hits = "0";

    const char* agreement = s.agreement == Agreement::Agree ? "agree" : s.agreement == Agreement::Disagree ? "DISAGREE" : "n/a";
    std::string line = "[ffxiv-resolve] " + s.id + " " + StatusName(s.status) + " rva=" + (s.hasValue ? detail::HexValue(s.rva) : std::string("n/a")) +
                       " hits=" + hits + " table=" + (s.hasTable ? detail::HexValue(s.tableValue) : std::string("n/a")) + " " + agreement +
                       " ms=" + detail::Millis(s.micros);
    if (!s.reason.empty())
        line += " note=" + s.reason;
    return line;
}

inline LogLevel SeamLogLevel(const SeamResult& s)
{
    if (s.status != SeamStatus::Found || s.agreement == Agreement::Disagree)
        return LogLevel::Warn;
    return LogLevel::Info;
}

inline std::string FormatTotalLine(const ResolvedBuild& rb)
{
    std::string line = "[ffxiv-resolve] total seams=" + std::to_string(rb.seams.size()) + " found=" + std::to_string(rb.Count(SeamStatus::Found)) +
                       " single=" + std::to_string(rb.Count(SeamStatus::FoundSingleAnchor)) + " notfound=" + std::to_string(rb.Count(SeamStatus::NotFound)) +
                       " ambiguous=" + std::to_string(rb.Count(SeamStatus::Ambiguous)) + " conflict=" + std::to_string(rb.Count(SeamStatus::Conflict)) +
                       " patched=" + std::to_string(rb.Count(SeamStatus::EntryPatched)) + " scan_ms=" + detail::Millis(rb.scanMicros) +
                       " ms=" + detail::Millis(rb.totalMicros) + " stamp=" + detail::HexValue(rb.timeDateStamp) + " size=" + detail::HexValue(rb.sizeOfImage) +
                       " known=" + (rb.knownBuild ? "yes" : "no");
    if (!rb.registryErrors.empty())
        line += " registry_errors=" + std::to_string(rb.registryErrors.size());
    return line;
}

#if defined(LOG_INFO) && defined(LOG_WARN) && defined(LOG_ERROR)
// The product sink. The line is an ARGUMENT of the macro, never its format string (a '{' in a seam note must not reach the formatter).
inline void DefaultLogSink(LogLevel level, const std::string& line)
{
    switch (level)
    {
    case LogLevel::Info:
        LOG_INFO("{}", line);
        break;
    case LogLevel::Warn:
        LOG_WARN("{}", line);
        break;
    case LogLevel::Error:
        LOG_ERROR("{}", line);
        break;
    }
}
inline LogSink MakeDefaultSink() { return LogSink(&DefaultLogSink); }
#endif
} // namespace ext::ffxiv::resolve
