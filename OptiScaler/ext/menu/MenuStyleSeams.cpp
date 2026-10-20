#include "pch.h"

#include "ext/menu/MenuStyleSeams.h"

#include "ext/core/ExtConfig.h"
#include "ext/menu/EmberPalette.h"
#include "ext/menu/MenuStyle.h"

#include <Config.h>
#include <imgui/imgui.h>

// Copies the Ember palette into the ImGuiStyle. tests/ext/menu/menu_style_imgui_map.cpp checks that every ImGuiCol_ the
// library declares is assigned here (a new colour in a library update would otherwise keep a classic accent tint).
namespace ext::menu
{
namespace
{
ImVec4 V(const menustyle::Rgba& c) { return ImVec4(c.r, c.g, c.b, c.a); }
ImVec2 V2(const float (&v)[2]) { return ImVec2(v[0], v[1]); }

void Copy(ImGuiStyle& style, const menustyle::Ember& e)
{
    const auto& c = e.colors;
    ImVec4* k = style.Colors;

    k[ImGuiCol_Text] = V(c.text);
    k[ImGuiCol_TextDisabled] = V(c.textDisabled);
    k[ImGuiCol_WindowBg] = V(c.windowBg);
    k[ImGuiCol_ChildBg] = V(c.childBg);
    k[ImGuiCol_PopupBg] = V(c.popupBg);
    k[ImGuiCol_Border] = V(c.border);
    k[ImGuiCol_BorderShadow] = V(c.borderShadow);
    k[ImGuiCol_FrameBg] = V(c.frameBg);
    k[ImGuiCol_FrameBgHovered] = V(c.frameBgHovered);
    k[ImGuiCol_FrameBgActive] = V(c.frameBgActive);
    k[ImGuiCol_TitleBg] = V(c.titleBg);
    k[ImGuiCol_TitleBgActive] = V(c.titleBgActive);
    k[ImGuiCol_TitleBgCollapsed] = V(c.titleBgCollapsed);
    k[ImGuiCol_MenuBarBg] = V(c.menuBarBg);
    k[ImGuiCol_ScrollbarBg] = V(c.scrollbarBg);
    k[ImGuiCol_ScrollbarGrab] = V(c.scrollbarGrab);
    k[ImGuiCol_ScrollbarGrabHovered] = V(c.scrollbarGrabHovered);
    k[ImGuiCol_ScrollbarGrabActive] = V(c.scrollbarGrabActive);
    k[ImGuiCol_CheckMark] = V(c.checkMark);
    k[ImGuiCol_SliderGrab] = V(c.sliderGrab);
    k[ImGuiCol_SliderGrabActive] = V(c.sliderGrabActive);
    k[ImGuiCol_Button] = V(c.button);
    k[ImGuiCol_ButtonHovered] = V(c.buttonHovered);
    k[ImGuiCol_ButtonActive] = V(c.buttonActive);
    k[ImGuiCol_Header] = V(c.header);
    k[ImGuiCol_HeaderHovered] = V(c.headerHovered);
    k[ImGuiCol_HeaderActive] = V(c.headerActive);
    k[ImGuiCol_Separator] = V(c.separator);
    k[ImGuiCol_SeparatorHovered] = V(c.separatorHovered);
    k[ImGuiCol_SeparatorActive] = V(c.separatorActive);
    k[ImGuiCol_ResizeGrip] = V(c.resizeGrip);
    k[ImGuiCol_ResizeGripHovered] = V(c.resizeGripHovered);
    k[ImGuiCol_ResizeGripActive] = V(c.resizeGripActive);
    k[ImGuiCol_InputTextCursor] = V(c.inputTextCursor);
    k[ImGuiCol_TabHovered] = V(c.tabHovered);
    k[ImGuiCol_Tab] = V(c.tab);
    k[ImGuiCol_TabSelected] = V(c.tabSelected);
    k[ImGuiCol_TabSelectedOverline] = V(c.tabSelectedOverline);
    k[ImGuiCol_TabDimmed] = V(c.tabDimmed);
    k[ImGuiCol_TabDimmedSelected] = V(c.tabDimmedSelected);
    k[ImGuiCol_TabDimmedSelectedOverline] = V(c.tabDimmedSelectedOverline);
    k[ImGuiCol_DockingPreview] = V(c.dockingPreview);
    k[ImGuiCol_DockingEmptyBg] = V(c.dockingEmptyBg);
    k[ImGuiCol_PlotLines] = V(c.plotLines);
    k[ImGuiCol_PlotLinesHovered] = V(c.plotLinesHovered);
    k[ImGuiCol_PlotHistogram] = V(c.plotHistogram);
    k[ImGuiCol_PlotHistogramHovered] = V(c.plotHistogramHovered);
    k[ImGuiCol_TableHeaderBg] = V(c.tableHeaderBg);
    k[ImGuiCol_TableBorderStrong] = V(c.tableBorderStrong);
    k[ImGuiCol_TableBorderLight] = V(c.tableBorderLight);
    k[ImGuiCol_TableRowBg] = V(c.tableRowBg);
    k[ImGuiCol_TableRowBgAlt] = V(c.tableRowBgAlt);
    k[ImGuiCol_TextLink] = V(c.textLink);
    k[ImGuiCol_TextSelectedBg] = V(c.textSelectedBg);
    k[ImGuiCol_TreeLines] = V(c.treeLines);
    k[ImGuiCol_DragDropTarget] = V(c.dragDropTarget);
    k[ImGuiCol_NavCursor] = V(c.navCursor);
    k[ImGuiCol_NavWindowingHighlight] = V(c.navWindowingHighlight);
    k[ImGuiCol_NavWindowingDimBg] = V(c.navWindowingDimBg);
    k[ImGuiCol_ModalWindowDimBg] = V(c.modalWindowDimBg);

    const auto& m = e.metrics;
    style.WindowRounding = m.windowRounding;
    style.ChildRounding = m.childRounding;
    style.FrameRounding = m.frameRounding;
    style.PopupRounding = m.popupRounding;
    style.ScrollbarRounding = m.scrollbarRounding;
    style.GrabRounding = m.grabRounding;
    style.TabRounding = m.tabRounding;
    style.WindowBorderSize = m.windowBorderSize;
    style.ChildBorderSize = m.childBorderSize;
    style.PopupBorderSize = m.popupBorderSize;
    style.FrameBorderSize = m.frameBorderSize;
    style.TabBorderSize = m.tabBorderSize;
    style.ScrollbarSize = m.scrollbarSize;
    style.GrabMinSize = m.grabMinSize;
    style.IndentSpacing = m.indentSpacing;
    style.WindowPadding = V2(m.windowPadding);
    style.FramePadding = V2(m.framePadding);
    style.ItemSpacing = V2(m.itemSpacing);
    style.ItemInnerSpacing = V2(m.itemInnerSpacing);
    style.CellPadding = V2(m.cellPadding);
}
} // namespace

void ApplyStyle(ImGuiStyle& style) noexcept
{
    try
    {
        if (ext::Cfg().menuStyle.GetStyle() != menustyle::Style::Ember)
            return;

        const auto* conf = Config::Instance();

        // Ember is a dark style; the light theme stays classic.
        if (conf->LightTheme.value_or_default())
            return;

        menustyle::EmberInputs in {};
        in.accent[0] = conf->MenuAccentColorR.value_or_default();
        in.accent[1] = conf->MenuAccentColorG.value_or_default();
        in.accent[2] = conf->MenuAccentColorB.value_or_default();
        in.windowAlpha = conf->MenuBGColorA.value_or_default();

        Copy(style, menustyle::BuildEmber(in));
    }
    catch (...)
    {
    }
}
} // namespace ext::menu
