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
// The recorder's one-key mark: unbound by default, a row in the Keybinds card, one `mark` record and one log line per press.
void* MarkKeyOption() { return &ext::Cfg().framePacing.fgRecorderMarkKey; }

void MarkPressed(ext::menu::Ctx&)
{
    const unsigned number = ext::fgrec::Mark();
    LOG_INFO("user mark {}", number);
}

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
                       "The queue is bounded, each file rotates at 16 MiB (9 kept per session: 144 MiB plus the one\n"
                       "queue batch that crossed the limit, about 40 minutes at 60 base fps and 2x frame generation,\n"
                       "so a 25-minute session keeps its beginning) and only the newest 5 sessions are kept. Off\n"
                       "(the default) means no thread, no file, one atomic load plus a counter per event and a\n"
                       "config re-read every 16th tap per thread. Read a session with\n"
                       "tools/analyze-fg-recording.ps1. The mark key (Keybinds) writes a mark record when you\n"
                       "release it.");

    // [FramePacing] FgRecorderMarkKey: the one-key mark, set from the Keybinds section like the other shortcut rows.
    ImGui::TextDisabled("Mark key: press it while playing to write a mark at that moment into the\n"
                        "recording and one line into the log (it fires when you release the key). Bind it in\n"
                        "the Keybinds section.");

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

struct FramePacingShortcutRegistration
{
    FramePacingShortcutRegistration()
    {
        ext::menu::RegisterShortcut({ "FG recorder mark", 19, MarkKeyOption, "FG recorder mark key pressed, writing a mark",
                                      MarkPressed });
    }
} shortcutRegistration;
} // namespace
