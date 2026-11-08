#pragma once
// Effect meter (METER): the one line every effect's menu card ends with. A header of its own so the cards include ImGui here and the runtime (Meter.cpp) and the
// rules include none.

#include "ext/ffxiv/meter/Meter.h"

#include <imgui/imgui.h>

namespace ext::ffxiv::meter
{
// The card's last line, in the disabled text colour: "Meter: ran X %, visible Y % (mean Z/255)" live, "Meter: off" while the effect is off.
inline void DrawCardLine(StageId id)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", CardLine(id).c_str());
    ImGui::PopStyleColor();
}
} // namespace ext::ffxiv::meter
