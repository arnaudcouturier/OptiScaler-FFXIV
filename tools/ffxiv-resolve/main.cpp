// ffxiv-resolve: the patch-day report of the pattern-search resolver, offline, no game launch.
//
//   ffxiv-resolve <ffxiv_dx11.exe> [--margin] [--strict] [--compare-table] [--list-unbound]
//                                  [--emit-gamebuild <GameBuild.h> [--out <file>] [--allow-partial]]
//
// It runs the SAME resolver the game runs (ext/ffxiv/resolve: BuildRegistry + ResolveImage) over a DenseImage of the executable and prints
//   * one line per seam, in the exact format of the game's log (status, rva, hits per anchor, table value and agreement, milliseconds),
//   * the generated entry: `key=value` lines for every bound GameBuild field,
//   * the GameBuild Rva / Guard fields no seam binds (what a patch would still need by hand): the count, the names with --list-unbound,
//   * with --margin: the uniqueness margin of every scanned anchor (offline only),
//   * with --emit-gamebuild: the codegen of the known-build table entry. The text of the given GameBuild.h is copied and every BOUND field
//     (Rva, Offset, Guard arrays, timeDateStamp, sizeOfImage) is rewritten to what the pattern search found on THIS executable (a resolve
//     without the table, so a disagreement or a seam that would fall back to the table cannot hide). Comments and formatting are kept. The
//     text goes to --out (default stdout) and NEVER to the source header; --out may not name the source header or the input executable. If any bound field's seam is not
//     usable the codegen REFUSES (exit 2, nothing written) unless --allow-partial is given: the unresolved fields then keep their old text
//     and carry `// NOT re-verified for this build`. The whole human report (seam table, changed values, the NOT SELF-HEALING / NOT
//     RE-VERIFIED list, the shader-catalog caveat) goes to STDERR in this mode, so that `> new.h` captures the header and nothing else.
// It is NOT part of the product: it lives under tools\ so the winmm.dll build (ext\**\*.cpp) never compiles it.
//
// Exit codes: 0 report printed (and, when asked, every check passed, and the entry was emitted with every bound field re-verified);
//             1 usage, or the file could not be read as an x64 PE, or the header could not be read, does not fit the field table, or --out
//               is the source header or the input executable, or cannot be written;
//             2 --strict and a seam is not Found or disagrees with the table (or a scanned anchor is below its required margin when
//               --margin is also given); or --emit-gamebuild REFUSED because a bound field's seam is not usable;
//             3 --compare-table and a seam DISAGREES with the table, or the entry resolved WITHOUT the table differs from it (only when the
//               executable IS that build: the table is never offered to another build);
//             4 --emit-gamebuild --allow-partial emitted a header with NOT re-verified fields.
//             When several apply the worst wins: 1, then 2, then 3, then 4.
#include "EmitGameBuild.h"
#include "Margin.h"
#include "Verdict.h"
#include "ext/ffxiv/resolve/DenseImage.h"
#include "ext/ffxiv/resolve/Entry.h"
#include "ext/ffxiv/resolve/Registry.h"
#include "ext/ffxiv/resolve/Resolver.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace ext::ffxiv::resolve;

namespace
{
void Usage()
{
    std::printf("usage: ffxiv-resolve <ffxiv_dx11.exe> [--margin] [--strict] [--compare-table] [--list-unbound]\n"
                "                     [--emit-gamebuild <GameBuild.h> [--out <file>] [--allow-partial]]\n"
                "  prints the per-seam resolve results (the game's log lines), the generated entry and the unbound fields\n"
                "  --margin         also compute the uniqueness margin of every scanned anchor (offline, a few seconds)\n"
                "  --strict         exit 2 when any seam is not Found or disagrees with the table\n"
                "  --compare-table  when the executable is the table's build, exit 3 if a seam disagrees with the table or the entry resolved\n"
                "                   without the table differs from it\n"
                "  --list-unbound   name the GameBuild Rva/Guard fields no seam fills yet (the count is always printed)\n"
                "  --emit-gamebuild <GameBuild.h>\n"
                "                   write a COPY of that header with every bound field rewritten from this executable (stdout, or --out).\n"
                "                   The source is never written. The report goes to stderr. Refuses (exit 2) when a bound seam is unusable.\n"
                "  --out <file>     where --emit-gamebuild writes the header (default: stdout); not the source header or the input executable\n"
                "  --allow-partial  emit even when bound seams are unresolved: those fields keep their old text, marked\n"
                "                   `// NOT re-verified for this build` (exit 4)\n"
                "exit codes: 0 ok, 1 usage/read/shape error, 2 strict failure or emit refused, 3 table disagreement, 4 partial emit\n");
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        out.push_back(line);
    return out;
}

double MillisSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// 1 (error) beats 2 (strict failure / refused) beats 3 (table disagreement) beats 4 (partial emit) beats 0.
int Worse(int a, int b)
{
    auto rank = [](int c) { return c == 0 ? 0 : c == 4 ? 1 : c == 3 ? 2 : c == 2 ? 3 : 4; };
    return rank(b) > rank(a) ? b : a;
}

bool ReadBinary(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// True when both names are the same file (or, when `out` does not exist yet, the same absolute path).
bool SameFile(const std::string& a, const std::string& b)
{
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec))
    {
        const bool same = std::filesystem::equivalent(a, b, ec);
        return !ec && same;
    }
    const auto ca = std::filesystem::weakly_canonical(a, ec);
    const auto cb = std::filesystem::weakly_canonical(b, ec);
    return !ec && ca == cb;
}
} // namespace

int main(int argc, char** argv)
{
    std::string path, headerPath, outPath;
    bool margin = false, strict = false, compare = false, listUnbound = false, emit = false, allowPartial = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--margin")
            margin = true;
        else if (a == "--strict")
            strict = true;
        else if (a == "--compare-table")
            compare = true;
        else if (a == "--list-unbound")
            listUnbound = true;
        else if (a == "--allow-partial")
            allowPartial = true;
        else if ((a == "--emit-gamebuild" || a == "--out") && i + 1 < argc)
        {
            (a == "--out" ? outPath : headerPath) = argv[++i];
            emit = emit || a == "--emit-gamebuild";
        }
        else if (!a.empty() && a[0] == '-')
        {
            Usage();
            return 1;
        }
        else if (path.empty())
            path = a;
        else
        {
            Usage();
            return 1;
        }
    }
    if (path.empty() || ((!outPath.empty() || allowPartial) && !emit) || (emit && headerPath.empty()))
    {
        Usage();
        return 1;
    }
    // In emit mode stdout carries the header only: the whole report goes to stderr.
    static FILE* rep = emit ? stderr : stdout;

    // The header is read and checked before the long work, and the source is never opened for writing.
    std::string headerText;
    if (emit)
    {
        if (!ReadBinary(headerPath, headerText) || headerText.empty())
        {
            std::fprintf(stderr, "ffxiv-resolve: cannot read the header %s\n", headerPath.c_str());
            return 1;
        }
        if (!outPath.empty() && SameFile(outPath, headerPath))
        {
            std::fprintf(stderr, "ffxiv-resolve: --out is the source header (%s): the source is never overwritten, write the copy somewhere else\n", headerPath.c_str());
            return 1;
        }
        // the input executable is never written either (the write at the end truncates the file named by --out): a typo in the command must not destroy it
        if (!outPath.empty() && SameFile(outPath, path))
        {
            std::fprintf(stderr, "ffxiv-resolve: --out is the input executable (%s): it would be overwritten, write the copy somewhere else\n", path.c_str());
            return 1;
        }
    }

    const auto loadStart = std::chrono::steady_clock::now();
    std::string error;
    const auto image = DenseImage::FromFile(path, &error);
    if (!image)
    {
        std::fprintf(rep, "ffxiv-resolve: cannot use %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    const double loadMs = MillisSince(loadStart);

    const ext::ffxiv::GameBuild& table = ext::ffxiv::kCurrentBuild;
    const bool isTableBuild = image->TimeDateStamp() == table.timeDateStamp && image->SizeOfImage() == table.sizeOfImage;
    std::fprintf(rep, "== image ==\npath=%s\ntimeDateStamp=0x%x\nsizeOfImage=0x%x\nknownBuild=%s\nload_ms=%.1f\n", path.c_str(), image->TimeDateStamp(),
                 image->SizeOfImage(), isTableBuild ? "yes (the table of GameBuild.h)" : "no (pattern search only, nothing to cross-check)", loadMs);

    const Registry registry = BuildRegistry();
    ResolveOptions options;
    options.knownTable = isTableBuild ? &table : nullptr;
    options.sink = [](LogLevel level, const std::string& line)
    { std::fprintf(rep, "%s%s\n", level == LogLevel::Info ? "" : level == LogLevel::Warn ? "WARN  " : "ERROR ", line.c_str()); };

    std::fprintf(rep, "== seams ==\n");
    const auto resolveStart = std::chrono::steady_clock::now();
    const ResolvedBuild rb = ResolveImage(*image, registry, options);
    const double resolveMs = MillisSince(resolveStart);
    std::fprintf(rep, "resolve_wall_ms=%.1f (scan %.1f)\n", resolveMs, static_cast<double>(rb.scanMicros) / 1000.0);

    std::fprintf(rep, "== entry ==\n%s", FormatEntry(rb, registry).c_str());

    const std::vector<std::string> unbound = UnboundFields(registry);
    std::fprintf(rep, "== unbound ==\n%zu GameBuild Rva/Guard fields are not bound to a seam yet%s\n", unbound.size(),
                 unbound.empty() ? "" : (listUnbound ? ":" : " (--list-unbound names them)"));
    if (listUnbound)
    {
        for (const std::string& key : unbound)
            std::fprintf(rep, "  %s\n", key.c_str());
    }

    // The pure pattern-search answer (no table: no cross-check overwrite, no fallback). On an unknown build the run above already is that.
    ResolvedBuild pureStore;
    const ResolvedBuild* pure = &rb;
    auto Pure = [&]() -> const ResolvedBuild&
    {
        if (isTableBuild && pure == &rb)
        {
            pureStore = ResolveImage(*image, registry);
            pure = &pureStore;
        }
        return *pure;
    };

    int exitCode = 0;
    if (margin)
    {
        std::fprintf(rep, "== margin (distance to the nearest other site, fixed bytes; required = AnchorSpec::minMargin) ==\n");
        std::size_t waived = 0;
        for (std::size_t i = 0; i < registry.seams.size(); ++i)
        {
            const SeamSpec& seam = registry.seams[i];
            for (std::size_t a = 0; a < seam.anchors.size(); ++a)
            {
                const AnchorSpec& spec = seam.anchors[a];
                if (!spec.fromSeam.empty())
                    continue;
                waived += spec.minMargin == 0 ? 1 : 0;
                const auto m = offline::AnchorMargin(*image, spec, rb.seams[i].anchors[a]);
                const bool ok = offline::MarginOk(m, spec.minMargin);
                if (m.found)
                    std::fprintf(rep, "%s %s/%s distance=%u at 0x%x (%u site%s) required=%u\n", ok ? "ok  " : "LOW ", seam.id.c_str(), spec.name.c_str(), m.distance, m.rva,
                                 m.sitesAtDistance, m.sitesAtDistance == 1 ? "" : "s", spec.minMargin);
                else
                    std::fprintf(rep, "%s %s/%s distance>%u required=%u\n", ok ? "ok  " : "LOW ", seam.id.c_str(), spec.name.c_str(), offline::kDefaultMarginCap, spec.minMargin);
                if (!ok && strict)
                    exitCode = 2;
            }
        }
        std::fprintf(rep, "margin waived (minMargin 0, the predicates are the disambiguator): %zu anchor%s\n", waived, waived == 1 ? "" : "s");
    }

    if (strict)
    {
        for (const std::string& why : offline::StrictFailures(rb))
        {
            std::fprintf(rep, "STRICT: %s\n", why.c_str());
            exitCode = 2;
        }
    }

    if (compare)
    {
        if (!isTableBuild)
            std::fprintf(rep, "COMPARE: the executable is not the table's build: nothing to compare\n");
        else
        {
            // The run above had the table: on a disagreement the TABLE value replaced the resolved one, so its entry would equal the table's
            // by construction. The comparison therefore uses a second, silent resolve WITHOUT the table (the pure pattern-search answer),
            // and any seam that disagreed in the first run fails the command on its own.
            const auto got = Lines(offline::StripEntryComments(FormatEntry(Pure(), registry)));
            const auto want = Lines(FormatTableEntry(table, registry));
            bool same = got == want;
            const std::vector<std::string> disagree = offline::DisagreeingSeams(rb);
            for (const std::string& id : disagree)
                std::fprintf(rep, "COMPARE: seam %s DISAGREES with the table\n", id.c_str());
            for (const std::string& w : want)
            {
                if (std::find(got.begin(), got.end(), w) == got.end())
                    std::fprintf(rep, "COMPARE: table has   %s\n", w.c_str());
            }
            for (const std::string& g : got)
            {
                if (std::find(want.begin(), want.end(), g) == want.end())
                    std::fprintf(rep, "COMPARE: resolved is %s\n", g.c_str());
            }
            std::fprintf(rep, "COMPARE: the generated entry %s the table (%zu lines), %zu seam%s disagree%s\n", same ? "equals" : "DIFFERS from", want.size(),
                         disagree.size(), disagree.size() == 1 ? "" : "s", disagree.size() == 1 ? "s" : "");
            if ((!same || !disagree.empty()) && exitCode == 0)
                exitCode = 3;
        }
    }

    if (emit)
    {
        const ResolvedBuild& answer = Pure();
        const offline::EmitResult result = offline::EmitGameBuild(headerText, answer, registry, allowPartial);
        std::fprintf(rep, "%s", offline::FormatEmitReport(result, answer, allowPartial).c_str());
        using Outcome = offline::EmitResult::Outcome;
        if (result.outcome == Outcome::Error)
            return 1;
        if (result.outcome == Outcome::Refused)
        {
            std::fprintf(rep, "emit: nothing written (the codegen refused: %zu bound field%s unresolved; --allow-partial emits them marked)\n", result.unresolved.size(),
                         result.unresolved.size() == 1 ? "" : "s");
            exitCode = Worse(exitCode, 2);
        }
        else
        {
            if (!outPath.empty())
            {
                std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
                f.write(result.text.data(), static_cast<std::streamsize>(result.text.size()));
                f.close();
                if (!f)
                {
                    std::fprintf(rep, "ffxiv-resolve: cannot write %s\n", outPath.c_str());
                    return 1;
                }
                std::fprintf(rep, "emit: wrote %zu bytes to %s (the source %s was not touched)\n", result.text.size(), outPath.c_str(), headerPath.c_str());
            }
            else
            {
#ifdef _WIN32
                _setmode(_fileno(stdout), _O_BINARY); // the header keeps its line endings: no \n -> \r\n translation
#endif
                std::fwrite(result.text.data(), 1, result.text.size(), stdout);
                std::fflush(stdout);
                std::fprintf(rep, "emit: wrote %zu bytes to stdout (the source %s was not touched)\n", result.text.size(), headerPath.c_str());
            }
            if (result.outcome == Outcome::EmittedPartial)
                exitCode = Worse(exitCode, 4);
        }
    }
    return exitCode;
}
