#pragma once
// Display only: which nvngx_dlss.dll the NGX loader picks (OptiScaler's own search order) and which other copies sit in
// other folders, so the Help / Status page can show both instead of one misleading number.
//
// Background (found 2026-10-16): OptiScaler's DLSSFeature::ReadVersion asks for the module by NAME only
// (DetourFindFunction("nvngx_dlss.dll", ...)), so with two nvngx_dlss.dll in the process (the game's own 3.5.10 and
// OptiScaler's 310.9.1) the label can show the game's file although NGX loads OptiScaler's. This header and
// DlssDlls.cpp read each copy BY FULL PATH and never change which file is used or the version the code gates on.
//
// The formatting is pure (no upstream types) so tests/ext/upscaler/dlss_dll_rows.cpp can build it standalone.

#include <cstdint>
#include <string>
#include <vector>

namespace ext::upscaler
{
struct DlssDllRow
{
    std::string label;
    std::string value;
    bool dim = false; // greyed out: a copy that is present but not the one in use
};

// One nvngx_dlss.dll candidate, in the order NGX searches the folders. `snippet` is NVSDK_NGX_GetSnippetVersion packed as
// major << 16 | minor << 8 | patch (0 = the module is not loaded or has no version export). `where` is a short folder
// name for people ("OptiScaler folder", "game folder", ...).
struct DlssDllEntry
{
    std::string where;
    uint32_t snippet = 0;
    bool loaded = false;
};

inline std::string DlssSnippetText(uint32_t snippet)
{
    const uint32_t major = (snippet & 0xFFFF0000u) / 0x00010000u;
    const uint32_t minor = (snippet & 0x0000FF00u) / 0x00000100u;
    const uint32_t patch = (snippet & 0x000000FFu);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

// The first entry is the one NGX loads; later entries are copies in other folders that are NOT used. No entries, no rows.
inline std::vector<DlssDllRow> FormatDlssDllRows(const std::vector<DlssDllEntry>& entries)
{
    std::vector<DlssDllRow> rows;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto& e = entries[i];
        std::string version = e.loaded && e.snippet != 0 ? DlssSnippetText(e.snippet) : std::string("not loaded");
        DlssDllRow row;
        row.label = i == 0 ? "DLSS DLL in use" : "DLSS DLL not used";
        row.value = version + " (" + e.where + ")";
        row.dim = i != 0;
        rows.push_back(std::move(row));
    }
    return rows;
}

// Help / Status page rows for the active upscaler. Empty unless the upscaler is DLSS and at least one copy is found.
// Cheap: cached for a couple of seconds, never throws.
std::vector<DlssDllRow> DlssDllRows(const std::string& upscalerShortName);
} // namespace ext::upscaler
