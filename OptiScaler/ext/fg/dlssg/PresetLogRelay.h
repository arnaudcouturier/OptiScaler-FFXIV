#pragma once
// DLSS-G preset readout, the part that touches the logger: a sink that reads the relayed NVIDIA runtime lines (PresetLog.h
// parses them) and the process-wide readout the menu shows. Nothing is written back into the logger from inside the sink:
// unrecognised preset lines are queued and logged later from FlushCandidateLines().

#include "ext/fg/dlssg/PresetLog.h"

namespace ext::fg::preset
{
// The readout shown in the menu.
Readout& ReadoutState();

// Adds the sink to the logger that exists right now. Called once, right after the logger is created (ext::core::Banner,
// single threaded at that point). A logger that is replaced later (changing log settings in the menu) drops the sink; the menu
// then says so (LogRelayState).
void AttachLogRelay();

enum class RelayState : uint8_t
{
    NotAttached, // no logger, or Banner has not run
    Attached,
    Lost, // the logger was replaced after the sink was added
    AsyncLogging // LogAsync is on: adding a sink while worker threads log is not safe, so it is skipped
};
RelayState LogRelayState();

// True while the runtime's own reply says that the active request turned its UI recomposition off: Preset A unless the runtime says
// otherwise, Preset C only when the runtime confirmed C and turned the recomposition off. For the menu readout only: it never decides
// the flag that OptiScaler sends (UiRecompositionRule.h does, from the request alone; a pin in fg_preset_seams_text checks it).
bool UiRecompositionPaused();

// Logs, at most a few at a time, the preset lines the parser did not recognise. Cheap when there are none; call from any
// thread that is allowed to log (the FG dispatch thread, the menu).
void FlushCandidateLines();
} // namespace ext::fg::preset
