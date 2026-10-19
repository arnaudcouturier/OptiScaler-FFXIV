// The RTX 20/30 "Kernel image" and "Kernel family" combos of the Frame generation unlocks card against the runtime's settings
// writer (AmpereMfgLoader.h). A hand-edited ini value the writer rejects (it writes Auto instead) must not be shown as selected:
// the menu's choice for a stored value is derived from the same exact comparisons the writer makes. KernelImageChoice and
// RouterChoice sit next to the writer and are used by the menu; this test runs the real writer (FormatIniContent030,
// ResolveRouter) for a set of stored values, including the case variants and padded spellings that used to be shown as selected.
//
// The menu file itself is ImGui glue; the call sites are pinned by text at the end (the combo reads its index from the two
// helpers, and the index helper with the case-insensitive compare is only left for the Preset combo, which the writer
// also accepts in either case).
#include "menu_scan.h"

#include "ext/fg/dlssg/AmpereMfgLoader.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void Check(bool ok, const std::string& what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what.c_str());
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)
#define CHECKM(x, msg) Check((x), (msg), __LINE__)

// The value after `key=` on its own line of the writer's output
std::string IniValue(const std::string& ini, const std::string& key)
{
    const std::string needle = "\n" + key + "=";
    const size_t at = ini.find(needle);
    if (at == std::string::npos)
        return {};
    const size_t from = at + needle.size();
    return ini.substr(from, ini.find('\n', from) - from);
}

const char* const kKernelImages[] = { "Auto", "PTX", "Cubin" };
const char* const kRouters[] = { "Auto", "SM75", "SM86" };

constexpr uint32_t kTuring = 0x00000160;
constexpr uint32_t kAmpere = 0x00000170;
} // namespace

int main(int argc, char** argv)
{
    using namespace AmpereMfgLoader;
    const std::string repo = argc > 1 ? argv[1] : ".";

    const std::vector<std::string> stored = { "",     "Auto", "auto", "AUTO", "PTX",   "ptx",   "Ptx",  "Cubin",
                                              "cubin", "CUBIN", " PTX", "PTX ", "SM75", "SM86", "sm75", "sm86",
                                              "Sm75", "SM 75", "x",    "1",    "true" };

    for (const std::string& s : stored)
    {
        // Kernel image: what the writer puts in the file is what the menu shows
        const std::string ini = FormatIniContent030(3, 1, "Auto", s, 0, "Auto", 1);
        const std::string written = IniValue(ini, "KernelImage");
        const int shown = KernelImageChoice(s);
        CHECKM(shown >= 0 && shown < 3 && written == kKernelImages[shown], "KernelImage '" + s + "': menu shows '" +
                                                                               std::string(kKernelImages[shown < 3 && shown >= 0 ? shown : 0]) +
                                                                               "', writer writes '" + written + "'");

        // Kernel family: the choice is "forced" exactly when the loader's ResolveRouter does not treat the value as Auto, and
        // then it forces the family the menu shows. Turing and Ampere cover both families.
        const int router = RouterChoice(s);
        CHECKM(router >= 0 && router < 3, "RouterChoice range for '" + s + "'");
        bool forcedSomewhere = false;
        for (uint32_t arch : { kTuring, kAmpere })
        {
            const std::string autoResult = ResolveRouter(arch, "", "Auto", true);
            const std::string result = ResolveRouter(arch, "", s, true);
            if (result != autoResult)
            {
                forcedSomewhere = true;
                CHECKM(router != 0 && result == kRouters[router], "Router '" + s + "': the loader forces '" + result +
                                                                      "', the menu shows '" + std::string(kRouters[router]) + "'");
            }
        }
        CHECKM(!(router != 0 && !forcedSomewhere), "Router '" + s + "': shown as selected but the loader treats it as Auto");
        CHECKM(!(router == 0 && forcedSomewhere), "Router '" + s + "': the loader forces it but the menu shows Auto");
    }

    // the spellings that matter, spelled out
    CHECK(KernelImageChoice("PTX") == 1 && KernelImageChoice("Cubin") == 2 && KernelImageChoice("Auto") == 0);
    CHECK(KernelImageChoice("ptx") == 0 && KernelImageChoice("cubin") == 0 && KernelImageChoice("CUBIN") == 0);
    CHECK(RouterChoice("SM75") == 1 && RouterChoice("sm75") == 1 && RouterChoice("SM86") == 2 && RouterChoice("sm86") == 2);
    CHECK(RouterChoice("Sm75") == 0 && RouterChoice("Auto") == 0 && RouterChoice("") == 0);

    // the menu draws its indexes from the two helpers; only the Preset combo keeps the case-insensitive helper
    std::string menu = menuscan::ReadFile(repo + "/OptiScaler/ext/fg/MenuFgUnlocks.cpp");
    menu = menuscan::Squash(menuscan::StripComments(menu));
    CHECK(menu.find("intkernelImage=AmpereMfgLoader::KernelImageChoice(cfg.ampereMfgKernelImage.value_or_default());") !=
          std::string::npos);
    CHECK(menu.find("introuter=AmpereMfgLoader::RouterChoice(cfg.ampereMfgRouter.value_or_default());") != std::string::npos);
    CHECK(menu.find("intpreset=IndexOfValue(cfg.ampereMfgPreset.value_or_default(),kPresets,3);") != std::string::npos);
    CHECK(menu.find("IndexOfValue(cfg.ampereMfgKernelImage") == std::string::npos);
    CHECK(menu.find("IndexOfValue(cfg.ampereMfgRouter") == std::string::npos);

    if (failures)
    {
        std::printf("ampere_choices: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("ampere_choices: all passed\n");
    return 0;
}
