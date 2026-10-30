#pragma once
// The `[Menu] CustomTabCards` pin list as the Custom Tab picker saves it. Header-only and pure, so the rule is driven offline
// (tests/ext/menu/menu_manifest.cpp) instead of being read out of the menu glue.
//
// A pin is a card id in a comma separated list. The picker rebuilds the list from the boxes it lists, and outside the FFXIV
// build match the FFXIV-tab cards are not among them (ext/core/MenuManifest.h). Their pins stay anyway: the ini is copied
// between games, and the Custom Tab of a copy that does show these cards still has them. A pin that names no live card is
// dropped by the picker itself (no box, no alias there), so a retired id does not accumulate.

#include "ext/core/MenuManifest.h"

#include <string>

namespace ext::menu
{
// Whether the list holds this id as a whole entry: the boundaries are the ends of the list and the commas, so a shorter id
// never matches inside a longer one.
inline bool Pinned(const std::string& pins, const char* id)
{
    const std::string needle(id);
    if (needle.empty())
        return false;
    for (size_t at = pins.find(needle); at != std::string::npos; at = pins.find(needle, at + needle.size()))
    {
        const bool start = at == 0 || pins[at - 1] == ',';
        const bool end = at + needle.size() == pins.size() || pins[at + needle.size()] == ',';
        if (start && end)
            return true;
    }
    return false;
}

// `rebuilt` with the pins the picker could not offer here: while the FFXIV tab is hidden, every FFXIV-only card that was
// pinned keeps its pin. Nothing else is added, and no entry is duplicated.
inline std::string WithHiddenFfxivPins(const std::string& rebuilt, const std::string& previous, bool ffxivTabVisible)
{
    if (ffxivTabVisible)
        return rebuilt;
    std::string pins = rebuilt;
    for (const char* id : manifest::kFfxivOnlyIds)
    {
        if (!Pinned(previous, id) || Pinned(pins, id))
            continue;
        if (!pins.empty())
            pins += ',';
        pins += id;
    }
    return pins;
}
} // namespace ext::menu
