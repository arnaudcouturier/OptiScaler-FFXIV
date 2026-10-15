#include "pch.h"

#include "ext/core/MenuWidgets.h"

#include <menu/menu_common.h>

namespace ext::menu
{
void Tooltip(const char* text)
{
    if (text == nullptr)
        return;

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }
}

void SectionTitle(const char* label)
{
    if (!ScopedCard::TryTitle(label))
        ImGui::SeparatorText(label);
}
} // namespace ext::menu
