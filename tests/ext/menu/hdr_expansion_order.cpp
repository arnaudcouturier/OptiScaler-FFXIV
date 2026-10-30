// The HDR card row order: each expansion row keeps its own Reset and help text. The UI expansion block once sat between
// Highlight expansion's Reset and its tooltip, so hovering the UI row's Reset showed the Highlight help text. The menu is
// ImGui glue and cannot run offline, so this reads the source (comments stripped, string literals kept; see menu_scan.h).
#include "menu_scan.h"

#include <cstdio>
#include <string>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

void Order(const std::string& repo)
{
    const std::string code =
        menuscan::StripComments(menuscan::ReadFile(repo + "/OptiScaler/ext/hdr/menu/HdrMenu.cpp"));

    // each row's Reset id appears once
    CHECK(menuscan::Count(code, "FfxivHDRExpansion") == 1);
    CHECK(menuscan::Count(code, "FfxivHDRUiExpansion") == 1);
    // each row's help text appears where its row is
    CHECK(menuscan::Count(code, "In SDR expansion mode") == 1);
    CHECK(menuscan::Count(code, "UI highlight expansion") == 1);

    const size_t highlightReset = code.find("FfxivHDRExpansion");
    const size_t highlightTip = code.find("In SDR expansion mode");
    const size_t uiSlider = code.find("UI highlight expansion");
    const size_t uiReset = code.find("FfxivHDRUiExpansion");
    const size_t uiTip = code.find("Scene HDR mode only");
    // the Highlight tooltip follows the Highlight Reset, then the whole UI row (slider, Reset, help) comes after it
    CHECK(highlightReset < highlightTip);
    CHECK(highlightTip < uiSlider);
    CHECK(uiSlider < uiReset);
    CHECK(uiReset < uiTip);

    // the UI row is disabled while the legacy composition is selected (it is read by the continuous composition only)
    const size_t begin = code.find("BeginDisabled(legacyComposition)");
    const size_t end = begin == std::string::npos ? std::string::npos : code.find("EndDisabled()", begin);
    CHECK(begin != std::string::npos && end != std::string::npos);
    CHECK(highlightTip < begin);
    CHECK(begin < uiReset && uiReset < end);
    CHECK(end < uiTip);
}
} // namespace

int main(int argc, char** argv)
{
    Order(argc > 1 ? argv[1] : ".");

    if (failures)
    {
        std::printf("hdr_expansion_order: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("hdr_expansion_order: all passed\n");
    return 0;
}
