#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/fgrec/FgRecorder.h"

#include <menu/menu_common.h>

// Card `frame_pacing` (tab "Frame Gen", after fg_auto_mfg; position: ext/core/MenuLayout.h): the FG event
// recorder switch. Always visible: the recorder works in any game the bridge serves, not only FFXIV.

namespace
{
void DrawFramePacing(ext::menu::Ctx& ctx)
{
    (void) ctx;
    auto& cfg = ext::Cfg().framePacing;

    ext::menu::SectionTitle("Frame pacing");

    bool record = cfg.fgRecorder.value_or_default();
    if (ImGui::Checkbox("Record FG events to fg-recordings (diagnostic)", &record))
        cfg.fgRecorder = record;
    ext::menu::Tooltip("Writes one JSONL file per session beside the DLL, in fg-recordings: every real present\n"
                       "(FG chain and visible output, with HRESULT), Reflex sleeps, the hidden present, AutoMFG\n"
                       "lines, NR model rebuilds, provider activations and module loads. No record holds a path.\n"
                       "The queue is bounded, each file rotates at 16 MiB (3 kept per session) and only the newest\n"
                       "5 sessions are kept. Off (the default) means no thread, no file, one atomic load plus a\n"
                       "counter per event and a config re-read every 16th tap per thread. Read a session with\n"
                       "tools/analyze-fg-recording.ps1.");

    if (record)
    {
        if (ext::fgrec::Running())
            ImGui::TextWrapped("Recording: the writer is running.");
        else if (const unsigned err = ext::fgrec::Error(); err != 0)
            ImGui::TextWrapped("Recorder error %u: no file is written (the folder may be read-only).", err);
        else
            ImGui::TextWrapped("Recorder armed: the first present starts the session file.");
    }
}

ext::menu::AutoRegister reg { { "frame_pacing", "Frame Gen", "Frame pacing", DrawFramePacing, "fg_auto_mfg" } };
} // namespace
