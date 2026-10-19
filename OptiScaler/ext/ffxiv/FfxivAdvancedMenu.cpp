#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/MenuRegistry.h"
#include "ext/core/MenuWidgets.h"
#include "ext/game/Game.h"

#include <imgui/imgui.h>

// "Advanced (FFXIV)" card (tab FFXIV, last card of the tab; position: ext/core/MenuLayout.h): two switches that only
// matter for the FFXIV build match and are rarely needed, behind a fold that is closed by default:
//   [Hotfix] RenderSizeFromWidthHeight  (code gate: game::Current().geometry, ext/upscaler/UpscalerSeams.cpp)
//   [XeFG]   ResetOnUpscalerChange      (ext/dx11bridge/NgxLifecycle.cpp, XeFG output only)
// Both are read where they are used, nothing is cached here; like every menu setting they are stored in the ini when the
// settings are saved. The card decides nothing.
namespace ext::ffxiv::advanced
{
namespace
{
// Same condition as the other FFXIV cards and the FFXIV tab itself.
bool AdvancedVisible(const ext::menu::Ctx&) { return ext::game::Current().lighting != nullptr; }

void Draw(ext::menu::Ctx&)
{
    ext::menu::SectionTitle("Advanced (FFXIV)");

    if (!ImGui::TreeNode("Rarely needed options##ffxiv_advanced"))
        return;

    auto& cfg = ext::Cfg();

    bool fromWidthHeight = cfg.upscaler.renderSizeFromWidthHeight.value_or_default();
    if (ImGui::Checkbox("Render size from Width/Height (FFXIV)", &fromWidthHeight))
        cfg.upscaler.renderSizeFromWidthHeight = fromWidthHeight;
    ext::menu::Tooltip("Works out the render size from the Width/Height values the game reports instead of from the render\n"
                       "subrect. Only FFXIV uses it: the code behind it exists only for the matched FFXIV build, so it does\n"
                       "nothing in any other game. Leave it off unless the render size looks wrong in FFXIV: a wrong reading\n"
                       "is a wrong picture rather than an error, and it applies to every upscaler.\n\n"
                       "It is read at every evaluate, but an upscaler that was created at the other size refuses the\n"
                       "evaluate, so a change settles when the upscaler is created again (change the quality level, or\n"
                       "restart the game).");

    bool resetOnChange = cfg.fg.xefgResetOnUpscalerChange.value_or_default();
    if (ImGui::Checkbox("Rebuild XeFG when the upscaler is released (FFXIV)", &resetOnChange))
        cfg.fg.xefgResetOnUpscalerChange = resetOnChange;
    ext::menu::Tooltip("When the upscaler is released (a quality or NR change), rebuilds the XeFG frame generation context so\n"
                       "its pacing history starts clean; without it variable refresh can drop out after such a change.\n"
                       "Only used in FFXIV, and only while XeFG is the frame generation output. It is read each time an\n"
                       "upscaler is released, so a change applies at the next release. On by default.");

    ImGui::TreePop();
}

const ext::menu::AutoRegister registration { { "ffxiv_advanced", "FFXIV", "Advanced (FFXIV)", Draw, nullptr,
                                               AdvancedVisible } };
} // namespace
} // namespace ext::ffxiv::advanced
