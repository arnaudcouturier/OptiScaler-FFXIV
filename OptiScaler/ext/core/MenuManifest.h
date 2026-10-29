#pragma once
// The menu manifest: the fork's part of the menu as pure data, checked offline by tests/ext/menu/menu_manifest.cpp.
//
// Which card sits in which tab, and in what order, is the layout table (ext/core/MenuLayout.h: every slot has the card id
// and its tab). This file adds what the layout cannot say: which cards need the FFXIV build match, which card ids exist
// in users' configs, the ids that were retired, and the config keys that deliberately have no control.
//
// Adding a card: register it, add its slot to kSlots, and (when it needs the FFXIV build match) its id to kFfxivOnlyIds;
// the manifest test fails until all of those agree. Retiring a card: remove its slot and add an alias.

#include "ext/core/MenuLayout.h"

#include <array>

namespace ext::menu::manifest
{
// The tab that is shown only on the FFXIV build match (a row of upstream's tabs[] array, see FfxivTabVisible)
inline constexpr const char* kFfxivTab = "FFXIV";

// Fork cards that need the FFXIV build match: they live in the FFXIV tab, and only they do. Their visible predicates are
// the build match (`ext::game::Current()`), so the card hides itself even when it is pinned to the Custom Tab.
inline constexpr const char* kFfxivOnlyIds[] = {
    "window_mode", "reshade_depth", "ffxiv_hdr", "ffxiv_jitter", "ffxiv_character_preview", "fg_hudless", "ffxiv_companion",
    "hudless_diag", "ffxiv_advanced",
};

// Every fork card id that has been in a released menu. The Custom Tab saves pins by id (`[Menu] CustomTabCards`), so each
// of these must still resolve: a live card (a slot of kSlots) or an alias.
inline constexpr const char* kPersistedForkIds[] = {
    "dlss_nr",       "ffxiv_character_preview", "forced_quality", "ffxiv_jitter", "fg_unlocks",
    "ffxiv_hdr",     "fg_hudless",              "fg_auto_mfg",    "window_mode",  "hudless_diag",
    "mipmap_auto",   "ffxiv_companion",         "reshade_addon_probe",
    "ffxiv_advanced", // new in the menu cleanup (not in session-final)
    "reshade_depth",  // new with the automatic ReShade scene depth
};

// A retired card id and the card that took its place (a merged or removed card keeps its id here)
struct Alias
{
    const char* oldId;
    const char* newId;
};
inline constexpr std::array<Alias, 0> kAliases {};

// Config keys ("[Section] Key") that stay without a control on purpose: developer gates and the menu style, read from the ini only.
inline constexpr const char* kIniOnlyKeys[] = {
    "[HudLess] Diagnostics", // shows the HUD-less diagnostics card
    "[HudLess] ReShadeProbe", // shows the ReShade add-on probe card
    "[HudLess] HdrPaste", // opt-in for a GPU path no HDR user has confirmed yet: kept out of the menu until one does
    "[HudLess] GuardMode", // opt-in guard policy that has not been tried in a game: kept out of the menu until it has
    "[HudLess] PlateDiagnostics", // developer probes (log only) of the output stream and the UI draws
    "[HudLess] PlateOverlay", // developer outline of the probe's suspected nameplate rectangles
    "[Menu] Style", // the Ember menu look: a theme preference read from the ini when the menu opens
    "[DlssNr] ReplayCapture", // developer key: the capture triggers write a full replay set of the pass
    "[NgxRelease] SkipReleaseSleep", // kill switch of the release-path sleep skip
    "[FramePacing] Trace", // diagnostic switch of the frame-pacing CPU trace
    "[FramePacing] GpuTimers", // diagnostic switch of the frame-pacing GPU stage timers
    "[FramePacing] HiddenPresentDoNotWait", // debug switch of the hidden DX11 present, compared with the trace
    "[Mipmap] AutomaticFFXIV", // another name for MipmapBiasAuto, which the Automatic Mipmap Bias card shows
    "[Mipmap] MipmapBiasObserve", // a debug log switch for the bind-time mipmap bias
    "[DlssNr] PauseOnFocusLoss", // another name for DisableOnFocusLoss, moved into it when the ini is read
    "[FFXIV] FfxivLegacyGameBuild", // persistent rollback switch of the game build handling, read once at start
    "[FFXIV] FfxivUnknownBuildHooks", // persistent switch: the game-address features on a game build the table does not know
    "[FFXIV] FfxivForceUnknownBuild", // persistent debug switch: ignore the known-build table
};

// Proof of the control for keys whose member name is shared by several configs ([HDR], [HudLess] and [Window] all bind a member
// called mode), so the key rule of tests/ext/menu/menu_key_controls.cpp cannot tell them apart by name: the menu file (relative
// to ext/) that draws the control and a text that must be in it (whitespace and comments ignored).
struct KeyEvidence
{
    const char* key;  // "[Section] Key"
    const char* file;
    const char* text;
};
inline constexpr KeyEvidence kKeyEvidence[] = {
    { "[HDR] Mode", "hdr/menu/HdrMenu.cpp", "hdr.mode = mode;" },
    { "[HudLess] Mode", "hudless/HudlessMenu.cpp", "ext::hudless::SetMode(chosen);" },
    { "[Window] Mode", "window/WindowMenu.cpp", "cfg.mode = std::string(kNames[index]);" },
};
} // namespace ext::menu::manifest
