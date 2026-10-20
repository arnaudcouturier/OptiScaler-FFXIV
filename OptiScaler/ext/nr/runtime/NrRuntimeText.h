#pragma once
// The words of the NR model version picker in the Neural Rendering card: the combo entries and the status lines. Pure (the card hands
// in the facts it read from the real state), pinned by tests/ext/nr/nr_runtime_text.cpp so a change of wording is a deliberate edit.

#include "ext/nr/runtime/NrRuntimeCatalog.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ext::nr::runtime::text
{
namespace detail
{
inline std::string Narrow(const std::wstring& w)
{
    std::string out;
    for (const wchar_t c : w)
        out += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
    return out;
}
} // namespace detail

// "NR.310.9  (310.9.16.0, 263.4 MB, SHA-256 1a2b3c4d5e6f)"; the legacy file is "Legacy location  (...)".
inline std::string EntryLabel(const Entry& e)
{
    std::string label = e.legacy ? std::string("Legacy location") : detail::Narrow(e.id);
    const VersionParts& v = OrderVersion(e);
    label += "  (";
    if (!v.empty())
        label += detail::Narrow(VersionText(v)) + ", ";
    label += SizeText(e.sizeBytes) + ", SHA-256 " + ShortSha(e.sha256) + ")";
    return label;
}

// The first entry of the combo: what Automatic picks right now.
inline std::string AutomaticLabel(const std::vector<Entry>& entries)
{
    const auto best = ChooseAutomatic(entries);
    if (!best)
        return "Automatic (nothing installed)";
    const Entry& e = entries[*best];
    return "Automatic (newest: " + (e.legacy ? std::string("legacy location") : detail::Narrow(e.id)) + ")";
}

struct Choices
{
    std::vector<std::string> labels; // what the combo shows
    std::vector<std::string> values; // what [DlssNr] Runtime is set to when the entry is picked ("" = Automatic)
    int selected = 0;
};

// The combo: Automatic, then every installed candidate (legacy file first, then the folders by name), then - when the config names a
// runtime that is not installed - that name marked "(not installed)", selected, so the card never shows a different model than asked.
inline Choices BuildChoices(const std::vector<Entry>& entries, const std::string& requested)
{
    Choices c;
    c.labels.push_back(AutomaticLabel(entries));
    c.values.push_back(std::string());
    std::wstring wide;
    for (const char ch : requested)
        wide += static_cast<wchar_t>(static_cast<unsigned char>(ch));
    const Request parsed = ParseRequest(wide);

    // the legacy file first, then the folders, newest first
    std::vector<size_t> order;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].legacy)
            order.push_back(i);
    const size_t legacyCount = order.size();
    for (size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].legacy)
            order.push_back(i);
    std::stable_sort(order.begin() + static_cast<std::ptrdiff_t>(legacyCount), order.end(),
                     [&](size_t a, size_t b) { return Preferred(entries[a], entries[b]); });
    for (const size_t i : order)
    {
        const Entry& e = entries[i];
        c.labels.push_back(EntryLabel(e));
        c.values.push_back(e.legacy ? std::string("legacy") : detail::Narrow(e.id));
        const bool match = e.legacy ? parsed.kind == RequestKind::Legacy
                                    : parsed.kind == RequestKind::Folder && runtime::detail::EqualsNoCase(parsed.folder, e.id);
        if (match)
            c.selected = static_cast<int>(c.labels.size()) - 1;
    }
    if (parsed.kind != RequestKind::Automatic && c.selected == 0)
    {
        c.labels.push_back(requested + " (not installed)");
        c.values.push_back(requested);
        c.selected = static_cast<int>(c.labels.size()) - 1;
    }
    return c;
}

// Facts the card reads from the real state before it writes its status lines.
struct CardFacts
{
    bool available = false;      // the selection names a model file that exists
    std::string reason;          // when not available: UnavailableReason of the snapshot
    std::string loadedId;        // "legacy" or the folder name of the selected model
    std::string loadedVersion;   // its version text
    bool pending = false;        // a request waits for the quiet period
    std::string lockReason;      // empty: live switching is possible
    bool nativeVulkanRunning = false;
    bool ngxPathFollows = true;  // the selected model sits in the folder the NGX core was told to search first
    std::string ngxFolder;       // that folder
    bool reuseWanted = false;
    std::string reuseStatus;
};

inline std::vector<std::string> StatusLines(const CardFacts& f)
{
    std::vector<std::string> lines;
    if (!f.available)
        lines.push_back("Neural Rendering is unavailable: " + f.reason + ".");
    if (f.pending)
        lines.push_back("Switching as soon as the clicks settle...");
    if (f.available)
        lines.push_back("Selected model: " + f.loadedId + (f.loadedVersion.empty() ? std::string() : ", file version " + f.loadedVersion) + ".");
    if (!f.lockReason.empty())
        lines.push_back("Live switching is off for this session: " + f.lockReason + ". Restart the game to change the model.");
    if (f.nativeVulkanRunning)
        lines.push_back("Native Vulkan Neural Rendering keeps the model it started with; restart the game to change it.");
    if (f.available && !f.ngxPathFollows)
        lines.push_back("The NGX core was told at startup to search " + f.ngxFolder +
                        " first. This model is loaded by its full path, so it works now; restart the game to move that search path too.");
    if (f.reuseWanted)
        lines.push_back("Bottleneck reuse (describes the loaded model only): " + f.reuseStatus);
    return lines;
}
} // namespace ext::nr::runtime::text
