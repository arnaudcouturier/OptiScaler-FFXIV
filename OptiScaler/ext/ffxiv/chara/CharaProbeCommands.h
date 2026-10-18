#pragma once
// Runs one command of the live command channel (chara-probe.cmd, see CharaProbeRules.h for the parser) against the character preview
// configuration. A template over the configuration type so the tests can run it on a fake with the same member names; the product
// instantiates it with ext::ffxiv::chara::CharaConfig.
//
// Config writes are in memory only: nothing here saves the ini file.

#include "ext/ffxiv/chara/CharaProbeRules.h"

#include <string>

namespace ext::ffxiv::chara::probe
{
// `actions` provides:
//   std::string RequestDump();      // asks for the preview images + state.txt; returns the result text ("ok, ..." or "refused, ...")
//   std::string RequestState();     // writes state.txt only
//   std::string SetCopies(bool on); // copy tracking of the derived-texture set
//   std::string ResetAll();         // forgets derived textures and counters
// Returns the result text for the line of the result file.
template <class Config, class Actions> std::string Execute(const Command& command, Config& config, Actions& actions)
{
    auto flag = [&command](const char* key, auto& option)
    {
        const bool was = option.value_or_default();
        option = command.value;
        std::string text = std::string("ok, ") + key + " is now " + (command.value ? "on" : "off") + " (was " + (was ? "on" : "off") +
                           "); in memory only, the ini file is not changed";
        return text;
    };

    switch (command.kind)
    {
    case CommandKind::Tint: return flag("PreviewDebugTint", config.previewDebugTint);
    case CommandKind::Flood: return flag("PreviewDebugFlood", config.previewDebugFlood);
    case CommandKind::Aa:
    {
        std::string text = flag("PreviewAA", config.previewAA);
        if (config.previewDlss.value_or_default())
            text += "; note: PreviewAA is bypassed while the PreviewDLSS pass runs";
        return text;
    }
    case CommandKind::Dlss: return flag("PreviewDLSS", config.previewDlss);
    case CommandKind::Upscale: return flag("PreviewUpscale", config.previewUpscale);
    case CommandKind::Copies: return actions.SetCopies(command.value);
    case CommandKind::Dump: return actions.RequestDump();
    case CommandKind::State: return actions.RequestState();
    case CommandKind::Reset: return actions.ResetAll();
    case CommandKind::Help: return kHelpText;
    case CommandKind::Unknown: return "ignored, " + command.note;
    case CommandKind::Empty: break;
    }
    return "ignored";
}
} // namespace ext::ffxiv::chara::probe
