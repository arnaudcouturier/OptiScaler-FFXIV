// The Help / Status DLSS DLL rows: the pure formatting of ext/upscaler/DlssDlls.h. The probing of loaded modules is
// exercised in the game (log line "DLSS DLLs: ..." and the Help page), not here.
#include "ext/upscaler/DlssDlls.h"

#include <cassert>
#include <cstdio>

using namespace ext::upscaler;

int main()
{
    // Snippet packing: major << 16 | minor << 8 | patch, same decode as GetVersionUsingNGXSnippet.
    assert(DlssSnippetText((310u << 16) | (9u << 8) | 1u) == "310.9.1");
    assert(DlssSnippetText((3u << 16) | (5u << 8) | 10u) == "3.5.10");
    assert(DlssSnippetText(0) == "0.0.0");

    // Nothing found -> no rows.
    assert(FormatDlssDllRows({}).empty());

    // The user's real layout: NGX searches the OptiScaler folder first (310.9.1), the game's own 3.5.10 is a copy that is
    // loaded in the process (OptiScaler opens it to read a version) but is not what NGX uses.
    const std::vector<DlssDllEntry> both {
        { "OptiScaler folder", (310u << 16) | (9u << 8) | 1u, true },
        { "game folder", (3u << 16) | (5u << 8) | 10u, true },
    };
    const auto rows = FormatDlssDllRows(both);
    assert(rows.size() == 2);
    assert(rows[0].label == "DLSS DLL in use" && rows[0].value == "310.9.1 (OptiScaler folder)" && !rows[0].dim);
    assert(rows[1].label == "DLSS DLL not used" && rows[1].value == "3.5.10 (game folder)" && rows[1].dim);

    // One copy only: one row, never dimmed.
    const auto one = FormatDlssDllRows({ { "game folder", (3u << 16) | (5u << 8) | 10u, true } });
    assert(one.size() == 1 && one[0].label == "DLSS DLL in use" && !one[0].dim);

    // A copy that is present on disk but not loaded (or without the version export) says so instead of a fake version.
    const auto unloaded = FormatDlssDllRows({ { "OptiScaler folder", 0, false }, { "game folder", 0, true } });
    assert(unloaded[0].value == "not loaded (OptiScaler folder)");
    assert(unloaded[1].value == "not loaded (game folder)");

    std::puts("PASS: DLSS DLL rows: in-use first, others dimmed, snippet text decode, unloaded copies");
    return 0;
}
