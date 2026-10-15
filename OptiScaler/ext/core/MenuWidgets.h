#pragma once
// Fork menu widgets (docs/SEAMS.md section 3.6). Upstream's private helpers are not exposed; these mirror their
// behaviour using only public upstream API (ScopedCard) and ImGui.

namespace ext::menu
{
// Tooltip for the last item (same rule as upstream's ShowTooltip: also when the item is disabled)
void Tooltip(const char* text);

// Like upstream's SectionTitle: the first one in a card becomes the title of the card, otherwise a separator text
void SectionTitle(const char* label);
} // namespace ext::menu
