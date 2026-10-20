#pragma once
// Entry point of the Ember menu style (docs/FEATURES.md, "Menu style"). Called once from MenuCommon::ApplyThemeStyle in
// menu/menu_common.cpp, after the classic colours are set and before the HDR tone mapping of the style, so under HDR output the
// Ember colours are tone-mapped like the classic ones. No other upstream file names anything in here.

struct ImGuiStyle;

namespace ext::menu
{
// With [Menu] Style = ember (and the light theme off), replaces every colour and metric the classic theme sets with the Ember
// palette. Otherwise it does nothing. Never throws.
void ApplyStyle(ImGuiStyle& style) noexcept;
} // namespace ext::menu
