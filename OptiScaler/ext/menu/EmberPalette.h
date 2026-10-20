#pragma once
// The Ember menu style as plain data (docs/FEATURES.md, "Menu style"). No ImGui types, so the palette and its rules are
// unit-tested without the library; MenuStyleSeams.cpp copies the result into the ImGuiStyle.
//
// The idea is the opposite of the classic theme. Classic mixes the accent colour into almost everything: a 32% mix over near
// black is a brown button, a 55% mix is a rust one, so an orange accent reads muddy. Ember keeps every surface and control
// neutral and lets the accent mark state only: the check mark, the slider grab, the selected tab's overline, the focus ring,
// the text selection, the resize grips and the plots. Buttons, headers and the title bar never carry it.
//
// Surfaces are three steps of one charcoal (canvas, card, field) with hairline borders of white at low alpha, instead of grey
// borders, so a translucent window shows the game through its edges. The window alpha is the user's [Menu] BGColorA, the accent
// is [Menu] AccentColorR/G/B; BGColorR/G/B is not used, because the classic code mixes at most 12% of it into a fixed grey anyway.
//
// Dark only. The seam leaves a light theme to the classic code.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ext::menustyle
{
struct Rgba
{
    float r, g, b, a;
};

struct EmberInputs
{
    float accent[3];   // [Menu] AccentColorR/G/B, 0..1
    float windowAlpha; // [Menu] BGColorA, 0..1
};

struct EmberColors
{
    Rgba text, textDisabled, textLink, textSelectedBg;
    Rgba windowBg, childBg, popupBg, menuBarBg, dockingEmptyBg;
    Rgba border, borderShadow, separator, separatorHovered, separatorActive, treeLines;
    Rgba frameBg, frameBgHovered, frameBgActive;
    Rgba titleBg, titleBgActive, titleBgCollapsed;
    Rgba scrollbarBg, scrollbarGrab, scrollbarGrabHovered, scrollbarGrabActive;
    Rgba checkMark, sliderGrab, sliderGrabActive, inputTextCursor;
    Rgba button, buttonHovered, buttonActive;
    Rgba header, headerHovered, headerActive;
    Rgba resizeGrip, resizeGripHovered, resizeGripActive;
    Rgba tab, tabHovered, tabSelected, tabSelectedOverline, tabDimmed, tabDimmedSelected, tabDimmedSelectedOverline;
    Rgba dockingPreview;
    Rgba plotLines, plotLinesHovered, plotHistogram, plotHistogramHovered;
    Rgba tableHeaderBg, tableBorderStrong, tableBorderLight, tableRowBg, tableRowBgAlt;
    Rgba dragDropTarget, navCursor, navWindowingHighlight, navWindowingDimBg, modalWindowDimBg;
};

struct EmberMetrics
{
    float windowRounding, childRounding, frameRounding, popupRounding, scrollbarRounding, grabRounding, tabRounding;
    float windowBorderSize, childBorderSize, popupBorderSize, frameBorderSize, tabBorderSize;
    float scrollbarSize, grabMinSize, indentSpacing;
    float windowPadding[2], framePadding[2], itemSpacing[2], itemInnerSpacing[2], cellPadding[2];
};

struct Ember
{
    EmberColors colors;
    EmberMetrics metrics;
};

// BuildEmber starts every slot at these values, so a slot it forgets to set stays recognisable: the palette test rejects any.
inline constexpr float kUnsetValue = -1.0f;
inline constexpr size_t kColorSlots = sizeof(EmberColors) / sizeof(Rgba);
inline constexpr size_t kMetricValues = sizeof(EmberMetrics) / sizeof(float);
static_assert(sizeof(EmberColors) % sizeof(Rgba) == 0 && sizeof(EmberMetrics) % sizeof(float) == 0,
              "EmberColors is only Rgba members and EmberMetrics only floats");

namespace palette
{
constexpr float Clamp01(float v) { return std::max(0.0f, std::min(v, 1.0f)); }

// 0xRRGGBB -> Rgba. The surfaces below are written as hex so they read the same as the app's design tokens.
constexpr Rgba Hex(uint32_t rgb, float a = 1.0f)
{
    return { static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
             static_cast<float>(rgb & 0xFF) / 255.0f, a };
}
constexpr Rgba WithAlpha(Rgba c, float a) { return { c.r, c.g, c.b, a }; }
constexpr Rgba Mix(Rgba a, Rgba b, float t, float alpha = 1.0f)
{
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, alpha };
}
constexpr float Luminance(Rgba c) { return c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f; }

inline constexpr Rgba kWhite { 1.0f, 1.0f, 1.0f, 1.0f };

// Surfaces: canvas < card < field, then two lifts for hover and press. The same values as Lucent's app tokens.
inline constexpr Rgba kCanvas = Hex(0x141519);
inline constexpr Rgba kCard = Hex(0x1A1B20);
inline constexpr Rgba kField = Hex(0x212329);
inline constexpr Rgba kFieldHover = Hex(0x2A2D35);
inline constexpr Rgba kFieldActive = Hex(0x33363F);
inline constexpr Rgba kHeader = Hex(0x24262D);
inline constexpr Rgba kHeaderHover = Hex(0x2D3038);
inline constexpr Rgba kHeaderActive = Hex(0x353841);

inline constexpr Rgba kText = Hex(0xF4F4F5);
inline constexpr Rgba kTextDim = Hex(0xA0A0A6);

inline constexpr Rgba kHairline = { 1.0f, 1.0f, 1.0f, 0.09f };
inline constexpr Rgba kHairlineLight = { 1.0f, 1.0f, 1.0f, 0.05f };
} // namespace palette

inline Ember BuildEmber(const EmberInputs& in)
{
    using namespace palette;

    const Rgba accent { Clamp01(in.accent[0]), Clamp01(in.accent[1]), Clamp01(in.accent[2]), 1.0f };
    // A dark accent would vanish on the charcoal: lift it, as the classic theme does for its readable accent.
    const Rgba accentReadable = Luminance(accent) < 0.30f ? Mix(accent, kWhite, 0.45f) : accent;
    const Rgba accentBright = Mix(accent, kWhite, 0.20f);

    const float windowAlpha = Clamp01(in.windowAlpha);
    // Surfaces that sit on the window stay readable even when the window itself is made nearly transparent.
    const float minAlpha = windowAlpha >= 0.5f ? windowAlpha : 0.5f;
    const float cardAlpha = std::min(1.0f, minAlpha + 0.06f);

    EmberColors c;
    {
        Rgba* slots = reinterpret_cast<Rgba*>(&c);
        for (size_t i = 0; i < kColorSlots; ++i)
            slots[i] = { kUnsetValue, kUnsetValue, kUnsetValue, kUnsetValue };
    }
    c.text = kText;
    c.textDisabled = kTextDim;
    c.textLink = accentReadable;
    c.textSelectedBg = WithAlpha(accent, 0.35f);

    c.windowBg = WithAlpha(kCanvas, windowAlpha);
    c.childBg = WithAlpha(kCard, cardAlpha);
    c.popupBg = WithAlpha(kCard, 0.98f);
    c.menuBarBg = WithAlpha(kCanvas, windowAlpha);
    c.dockingEmptyBg = WithAlpha(kCanvas, windowAlpha);

    c.border = kHairline;
    c.borderShadow = { 0.0f, 0.0f, 0.0f, 0.0f };
    c.separator = kHairline;
    c.separatorHovered = WithAlpha(accent, 0.55f);
    c.separatorActive = accent;
    c.treeLines = kHairline;

    c.frameBg = kField;
    c.frameBgHovered = kFieldHover;
    c.frameBgActive = kFieldActive;

    // A flat title bar: the window's own colour, with the card step behind the active one. No accent.
    c.titleBg = WithAlpha(kCanvas, windowAlpha);
    c.titleBgActive = WithAlpha(kCard, cardAlpha);
    c.titleBgCollapsed = WithAlpha(kCanvas, 0.75f);

    c.scrollbarBg = { 0.0f, 0.0f, 0.0f, 0.0f };
    c.scrollbarGrab = Hex(0x4A4D59);
    c.scrollbarGrabHovered = Hex(0x61656F);
    c.scrollbarGrabActive = accent;

    c.checkMark = accentReadable;
    c.sliderGrab = accent;
    c.sliderGrabActive = accentBright;
    c.inputTextCursor = accentReadable;

    c.button = kField;
    c.buttonHovered = kFieldHover;
    c.buttonActive = kFieldActive;

    c.header = kHeader;
    c.headerHovered = kHeaderHover;
    c.headerActive = kHeaderActive;

    c.resizeGrip = WithAlpha(accent, 0.25f);
    c.resizeGripHovered = WithAlpha(accent, 0.65f);
    c.resizeGripActive = WithAlpha(accent, 0.95f);

    c.tab = { 0.0f, 0.0f, 0.0f, 0.0f };
    c.tabHovered = kHeaderHover;
    c.tabSelected = kField;
    c.tabSelectedOverline = accent;
    c.tabDimmed = { 0.0f, 0.0f, 0.0f, 0.0f };
    c.tabDimmedSelected = kHeader;
    c.tabDimmedSelectedOverline = kHairline;

    c.dockingPreview = WithAlpha(accent, 0.60f);

    c.plotLines = Mix(accent, kWhite, 0.25f);
    c.plotLinesHovered = Mix(accent, kWhite, 0.45f);
    c.plotHistogram = WithAlpha(accent, 0.85f);
    c.plotHistogramHovered = Mix(accent, kWhite, 0.25f);

    c.tableHeaderBg = WithAlpha(kHeader, cardAlpha);
    c.tableBorderStrong = kHairline;
    c.tableBorderLight = kHairlineLight;
    c.tableRowBg = { 0.0f, 0.0f, 0.0f, 0.0f };
    c.tableRowBgAlt = { 1.0f, 1.0f, 1.0f, 0.03f };

    c.dragDropTarget = WithAlpha(accent, 0.90f);
    c.navCursor = accentReadable;
    c.navWindowingHighlight = WithAlpha(accent, 0.70f);
    c.navWindowingDimBg = { 0.09f, 0.10f, 0.13f, 0.20f };
    c.modalWindowDimBg = { 0.04f, 0.04f, 0.07f, 0.55f };

    EmberMetrics m;
    {
        float* values = reinterpret_cast<float*>(&m);
        for (size_t i = 0; i < kMetricValues; ++i)
            values[i] = kUnsetValue;
    }
    m.windowRounding = 10.0f;
    m.childRounding = 8.0f;
    m.frameRounding = 6.0f;
    m.popupRounding = 8.0f;
    m.scrollbarRounding = 8.0f;
    m.grabRounding = 6.0f;
    m.tabRounding = 6.0f;
    m.windowBorderSize = 1.0f;
    m.childBorderSize = 1.0f;
    m.popupBorderSize = 1.0f;
    m.frameBorderSize = 1.0f;
    m.tabBorderSize = 0.0f;
    m.scrollbarSize = 8.0f;
    m.grabMinSize = 12.0f;
    m.indentSpacing = 18.0f;
    m.windowPadding[0] = 14.0f;
    m.windowPadding[1] = 12.0f;
    m.framePadding[0] = 10.0f;
    m.framePadding[1] = 5.0f;
    m.itemSpacing[0] = 10.0f;
    m.itemSpacing[1] = 8.0f;
    m.itemInnerSpacing[0] = 8.0f;
    m.itemInnerSpacing[1] = 6.0f;
    m.cellPadding[0] = 8.0f;
    m.cellPadding[1] = 5.0f;

    return { c, m };
}
} // namespace ext::menustyle
