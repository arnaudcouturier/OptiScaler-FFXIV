// The Ember palette (ext/menu/EmberPalette.h): every slot is set, the neutrals never take the accent, the accent only marks
// state, the window alpha is honoured, text and accent are readable on the surfaces, and the metrics are roomier than the
// classic theme's. The rules run for several accents (orange, blue, red, green, near-black), so a slot that quietly picked up
// the accent for one hue fails for another.
#include "ext/menu/EmberPalette.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ext::menustyle;

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

namespace
{
float Chroma(const Rgba& c) { return std::max({ c.r, c.g, c.b }) - std::min({ c.r, c.g, c.b }); }

float Linear(float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float RelLum(const Rgba& c) { return 0.2126f * Linear(c.r) + 0.7152f * Linear(c.g) + 0.0722f * Linear(c.b); }
float Contrast(const Rgba& a, const Rgba& b)
{
    const float la = RelLum(a), lb = RelLum(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

bool Same(const Rgba& a, const Rgba& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

struct Named
{
    const char* name;
    Rgba EmberColors::*slot;
};

// The slots that must stay neutral: surfaces, controls, headers, tabs, borders, the title bar.
const Named kNeutral[] = {
    { "windowBg", &EmberColors::windowBg }, { "childBg", &EmberColors::childBg }, { "popupBg", &EmberColors::popupBg },
    { "menuBarBg", &EmberColors::menuBarBg }, { "frameBg", &EmberColors::frameBg },
    { "frameBgHovered", &EmberColors::frameBgHovered }, { "frameBgActive", &EmberColors::frameBgActive },
    { "titleBg", &EmberColors::titleBg }, { "titleBgActive", &EmberColors::titleBgActive },
    { "titleBgCollapsed", &EmberColors::titleBgCollapsed }, { "button", &EmberColors::button },
    { "buttonHovered", &EmberColors::buttonHovered }, { "buttonActive", &EmberColors::buttonActive },
    { "header", &EmberColors::header }, { "headerHovered", &EmberColors::headerHovered },
    { "headerActive", &EmberColors::headerActive }, { "tabHovered", &EmberColors::tabHovered },
    { "tabSelected", &EmberColors::tabSelected }, { "tabDimmedSelected", &EmberColors::tabDimmedSelected },
    { "border", &EmberColors::border }, { "separator", &EmberColors::separator },
    { "scrollbarGrab", &EmberColors::scrollbarGrab }, { "scrollbarGrabHovered", &EmberColors::scrollbarGrabHovered },
    { "tableHeaderBg", &EmberColors::tableHeaderBg }, { "tableBorderStrong", &EmberColors::tableBorderStrong },
    { "text", &EmberColors::text }, { "textDisabled", &EmberColors::textDisabled },
};

// The slots that are exactly the accent (a bright accent is not lifted).
const Named kAccent[] = {
    { "checkMark", &EmberColors::checkMark }, { "sliderGrab", &EmberColors::sliderGrab },
    { "tabSelectedOverline", &EmberColors::tabSelectedOverline }, { "scrollbarGrabActive", &EmberColors::scrollbarGrabActive },
    { "separatorActive", &EmberColors::separatorActive }, { "navCursor", &EmberColors::navCursor },
    { "textLink", &EmberColors::textLink }, { "inputTextCursor", &EmberColors::inputTextCursor },
};

// Every colour slot and metric of the palette, by name: the golden dump below writes all of them, and the check in main proves
// the table is complete (it has as many entries as the structs have members).
#define COLOUR(n) { #n, &EmberColors::n }
const Named kAllColours[] = {
    COLOUR(text),
    COLOUR(textDisabled),
    COLOUR(textLink),
    COLOUR(textSelectedBg),
    COLOUR(windowBg),
    COLOUR(childBg),
    COLOUR(popupBg),
    COLOUR(menuBarBg),
    COLOUR(dockingEmptyBg),
    COLOUR(border),
    COLOUR(borderShadow),
    COLOUR(separator),
    COLOUR(separatorHovered),
    COLOUR(separatorActive),
    COLOUR(treeLines),
    COLOUR(frameBg),
    COLOUR(frameBgHovered),
    COLOUR(frameBgActive),
    COLOUR(titleBg),
    COLOUR(titleBgActive),
    COLOUR(titleBgCollapsed),
    COLOUR(scrollbarBg),
    COLOUR(scrollbarGrab),
    COLOUR(scrollbarGrabHovered),
    COLOUR(scrollbarGrabActive),
    COLOUR(checkMark),
    COLOUR(sliderGrab),
    COLOUR(sliderGrabActive),
    COLOUR(inputTextCursor),
    COLOUR(button),
    COLOUR(buttonHovered),
    COLOUR(buttonActive),
    COLOUR(header),
    COLOUR(headerHovered),
    COLOUR(headerActive),
    COLOUR(resizeGrip),
    COLOUR(resizeGripHovered),
    COLOUR(resizeGripActive),
    COLOUR(tab),
    COLOUR(tabHovered),
    COLOUR(tabSelected),
    COLOUR(tabSelectedOverline),
    COLOUR(tabDimmed),
    COLOUR(tabDimmedSelected),
    COLOUR(tabDimmedSelectedOverline),
    COLOUR(dockingPreview),
    COLOUR(plotLines),
    COLOUR(plotLinesHovered),
    COLOUR(plotHistogram),
    COLOUR(plotHistogramHovered),
    COLOUR(tableHeaderBg),
    COLOUR(tableBorderStrong),
    COLOUR(tableBorderLight),
    COLOUR(tableRowBg),
    COLOUR(tableRowBgAlt),
    COLOUR(dragDropTarget),
    COLOUR(navCursor),
    COLOUR(navWindowingHighlight),
    COLOUR(navWindowingDimBg),
    COLOUR(modalWindowDimBg),
};
#undef COLOUR
struct Scalar
{
    const char* name;
    float EmberMetrics::*value;
};
#define SCALAR(n) { #n, &EmberMetrics::n }
const Scalar kAllScalars[] = {
    SCALAR(windowRounding),
    SCALAR(childRounding),
    SCALAR(frameRounding),
    SCALAR(popupRounding),
    SCALAR(scrollbarRounding),
    SCALAR(grabRounding),
    SCALAR(tabRounding),
    SCALAR(windowBorderSize),
    SCALAR(childBorderSize),
    SCALAR(popupBorderSize),
    SCALAR(frameBorderSize),
    SCALAR(tabBorderSize),
    SCALAR(scrollbarSize),
    SCALAR(grabMinSize),
    SCALAR(indentSpacing),
};
#undef SCALAR
struct Pair
{
    const char* name;
    float (EmberMetrics::*value)[2];
};
#define PAIR(n) { #n, &EmberMetrics::n }
const Pair kAllPairs[] = {
    PAIR(windowPadding),
    PAIR(framePadding),
    PAIR(itemSpacing),
    PAIR(itemInnerSpacing),
    PAIR(cellPadding),
};
#undef PAIR

// The golden dump: external tools read it so their menus can carry exactly the same palette (set EMBER_PALETTE_JSON to a path).
void DumpJson(const char* path, const EmberInputs& in, const Ember& e)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "{\n  \"inputs\": { \"accent\": [%.6f, %.6f, %.6f], \"windowAlpha\": %.6f },\n  \"colors\": {\n", in.accent[0],
                 in.accent[1], in.accent[2], in.windowAlpha);
    for (size_t i = 0; i < sizeof(kAllColours) / sizeof(kAllColours[0]); ++i)
    {
        const Rgba& c = e.colors.*kAllColours[i].slot;
        std::fprintf(f, "    \"%s\": [%.6f, %.6f, %.6f, %.6f]%s\n", kAllColours[i].name, c.r, c.g, c.b, c.a,
                     i + 1 < sizeof(kAllColours) / sizeof(kAllColours[0]) ? "," : "");
    }
    std::fprintf(f, "  },\n  \"metrics\": {\n");
    for (const Scalar& s : kAllScalars)
        std::fprintf(f, "    \"%s\": %.6f,\n", s.name, e.metrics.*s.value);
    for (size_t i = 0; i < sizeof(kAllPairs) / sizeof(kAllPairs[0]); ++i)
    {
        const auto& v = e.metrics.*kAllPairs[i].value;
        std::fprintf(f, "    \"%s\": [%.6f, %.6f]%s\n", kAllPairs[i].name, v[0], v[1], i + 1 < sizeof(kAllPairs) / sizeof(kAllPairs[0]) ? "," : "");
    }
    std::fprintf(f, "  }\n}\n");
    std::fclose(f);
}

EmberInputs Inputs(float r, float g, float b, float alpha = 0.92f) { return { { r, g, b }, alpha }; }
} // namespace

int main()
{
    const std::vector<EmberInputs> accents = {
        Inputs(1.000000f, 0.392157f, 0.039216f), // Crunchyroll orange, as this style ships it
        Inputs(0.00f, 0.40f, 0.77f),             // OptiScaler's own blue
        Inputs(0.86f, 0.15f, 0.20f),
        Inputs(0.20f, 0.80f, 0.35f),
    };

    // Every colour and metric slot is set (BuildEmber starts them all at kUnsetValue), for ordinary and extreme inputs.
    for (const EmberInputs& in : { accents[0], accents[1], Inputs(0.0f, 0.0f, 0.0f), Inputs(1.0f, 1.0f, 1.0f, 0.0f) })
    {
        const Ember e = BuildEmber(in);
        const Rgba* slots = reinterpret_cast<const Rgba*>(&e.colors);
        for (size_t i = 0; i < kColorSlots; ++i)
        {
            const Rgba& c = slots[i];
            CHECK(c.r >= 0.0f && c.r <= 1.0f && c.g >= 0.0f && c.g <= 1.0f && c.b >= 0.0f && c.b <= 1.0f);
            CHECK(c.a >= 0.0f && c.a <= 1.0f);
        }
        const float* values = reinterpret_cast<const float*>(&e.metrics);
        for (size_t i = 0; i < kMetricValues; ++i)
            CHECK(values[i] >= 0.0f);
    }

    // The by-name tables cover every member of the structs.
    CHECK(sizeof(kAllColours) / sizeof(kAllColours[0]) == kColorSlots);
    CHECK(sizeof(kAllScalars) / sizeof(kAllScalars[0]) + 2 * sizeof(kAllPairs) / sizeof(kAllPairs[0]) == kMetricValues);

    const Ember base = BuildEmber(accents[0]);

    if (const char* path = std::getenv("EMBER_PALETTE_JSON"))
        DumpJson(path, accents[0], base);

    // Neutral slots are the same whatever the accent, and carry no hue of it.
    for (const EmberInputs& in : accents)
    {
        const Ember e = BuildEmber(in);
        for (const Named& n : kNeutral)
        {
            CHECK(Same(e.colors.*n.slot, base.colors.*n.slot));
            CHECK(Chroma(e.colors.*n.slot) <= 0.08f);
        }
    }

    // The accent only marks state: those slots are the accent itself, and the washes stay washes.
    for (const EmberInputs& in : accents)
    {
        const Ember e = BuildEmber(in);
        for (const Named& n : kAccent)
        {
            const Rgba& c = e.colors.*n.slot;
            CHECK(c.r == in.accent[0] && c.g == in.accent[1] && c.b == in.accent[2] && c.a == 1.0f);
        }
        CHECK(e.colors.sliderGrabActive.r >= in.accent[0] && e.colors.sliderGrabActive.g >= in.accent[1]);
        CHECK(e.colors.resizeGrip.a < e.colors.resizeGripHovered.a && e.colors.resizeGripHovered.a < e.colors.resizeGripActive.a);
        CHECK(e.colors.textSelectedBg.a > 0.0f && e.colors.textSelectedBg.a < 0.5f);
    }
    // A dark accent is lifted so the check mark does not vanish on the charcoal; the slider grab keeps the user's pick.
    {
        const Ember e = BuildEmber(Inputs(0.0f, 0.0f, 0.0f));
        CHECK(palette::Luminance(e.colors.checkMark) >= 0.30f);
        CHECK(Contrast(e.colors.checkMark, e.colors.childBg) >= 3.0f);
        CHECK(e.colors.sliderGrab.r == 0.0f);
    }

    // Window alpha: the window and title bar follow BGColorA; surfaces on the window never fall below a readable alpha.
    {
        const Ember glass = BuildEmber(Inputs(1.0f, 0.4f, 0.04f, 0.92f));
        CHECK(glass.colors.windowBg.a == 0.92f);
        CHECK(glass.colors.titleBg.a == 0.92f);
        CHECK(glass.colors.childBg.a >= glass.colors.windowBg.a);
        CHECK(glass.colors.childBg.a <= 1.0f);

        const Ember thin = BuildEmber(Inputs(1.0f, 0.4f, 0.04f, 0.20f));
        CHECK(thin.colors.windowBg.a == 0.20f);
        CHECK(std::fabs(thin.colors.childBg.a - 0.56f) < 1e-5f);

        const Ember solid = BuildEmber(Inputs(1.0f, 0.4f, 0.04f, 1.0f));
        CHECK(solid.colors.windowBg.a == 1.0f && solid.colors.childBg.a == 1.0f);

        // Values outside 0..1 are clamped, not trusted.
        const Ember wild = BuildEmber(Inputs(2.0f, -1.0f, 0.5f, 7.0f));
        CHECK(wild.colors.windowBg.a == 1.0f);
        CHECK(wild.colors.checkMark.r == 1.0f && wild.colors.checkMark.g >= 0.0f);
    }

    // Surfaces step up in lightness: canvas < card < field < hover < active, and the same for headers.
    {
        const auto& c = base.colors;
        CHECK(palette::Luminance(c.windowBg) < palette::Luminance(c.childBg));
        CHECK(palette::Luminance(c.childBg) < palette::Luminance(c.frameBg));
        CHECK(palette::Luminance(c.frameBg) < palette::Luminance(c.frameBgHovered));
        CHECK(palette::Luminance(c.frameBgHovered) < palette::Luminance(c.frameBgActive));
        CHECK(Same(c.button, c.frameBg) && Same(c.buttonHovered, c.frameBgHovered) && Same(c.buttonActive, c.frameBgActive));
        CHECK(palette::Luminance(c.header) < palette::Luminance(c.headerHovered));
        CHECK(palette::Luminance(c.headerHovered) < palette::Luminance(c.headerActive));
        CHECK(palette::Luminance(c.childBg) < palette::Luminance(c.header));
    }

    // Readable: body text 12:1 on the canvas and the card, dim text 4.5:1 on the card, the orange accent 3:1 (the WCAG floor for a
    // UI component) on the card and the field, and the hairline border stays a hairline.
    {
        const auto& c = base.colors;
        const Rgba canvas = palette::kCanvas, card = palette::kCard, field = palette::kField;
        CHECK(Contrast(c.text, canvas) >= 12.0f);
        CHECK(Contrast(c.text, card) >= 12.0f);
        CHECK(Contrast(c.text, field) >= 10.0f);
        CHECK(Contrast(c.textDisabled, card) >= 4.5f);
        CHECK(Contrast(c.textDisabled, field) >= 4.0f);
        CHECK(Contrast(c.checkMark, card) >= 3.0f);
        CHECK(Contrast(c.sliderGrab, field) >= 3.0f);
        CHECK(c.border.a > 0.0f && c.border.a <= 0.15f);
        CHECK(c.border.r == 1.0f && c.border.g == 1.0f && c.border.b == 1.0f);
    }

    // Metrics: rounder and roomier than the classic theme (2px corners, 8x4 spacing, 8x3 padding, 10px scrollbar).
    {
        const auto& m = base.metrics;
        CHECK(m.windowRounding >= 8.0f && m.childRounding >= 6.0f && m.frameRounding >= 4.0f && m.popupRounding >= 6.0f);
        CHECK(m.tabRounding >= 4.0f && m.grabRounding >= 4.0f && m.scrollbarRounding >= 4.0f);
        CHECK(m.windowPadding[0] >= 12.0f && m.windowPadding[1] >= 10.0f);
        CHECK(m.framePadding[0] >= 8.0f && m.framePadding[1] >= 4.0f);
        CHECK(m.itemSpacing[0] >= 8.0f && m.itemSpacing[1] >= 6.0f);
        CHECK(m.scrollbarSize <= 10.0f && m.scrollbarSize >= 6.0f);
        CHECK(m.frameBorderSize == 1.0f && m.windowBorderSize == 1.0f && m.tabBorderSize == 0.0f);
        CHECK(m.grabMinSize >= 10.0f);
    }

    return g_failed ? 1 : 0;
}
