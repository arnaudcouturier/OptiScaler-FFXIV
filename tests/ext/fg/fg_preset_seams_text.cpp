// Text pins of the DLSS-G preset integration: the places in upstream files where the fork is called, their order, and the rules
// that make "RenderPreset = Auto" cost nothing (no hooks without a live change). The code behind them is tested elsewhere
// (fg_preset_request, _switch, _guards, _log); these pins keep an upstream merge or a refactor from silently dropping a seam.
#include "fg_preset_harness.h"

#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// whitespace runs collapse to one space, so the pins survive re-wrapping
std::string Flat(const std::string& text)
{
    std::string out;
    bool space = false;
    for (char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            space = true;
        else
        {
            if (space && !out.empty())
                out += ' ';
            space = false;
            out += c;
        }
    }
    return out;
}

size_t Count(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

bool Before(const std::string& text, const std::string& first, const std::string& second)
{
    const size_t a = text.find(first), b = text.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

void UpstreamSeams(const std::string& repo)
{
    const std::string nv = Flat(ReadFile(repo + "/OptiScaler/nvapi/NvApiHooks.cpp"));
    CHECK(!nv.empty());
    // the call sits right after the original call and BEFORE the "found" block, so a "setting not found" reply can become OK
    const std::string call = "result = ext::fg::preset::OnDrsGetSetting(settingId, pSetting, result, _ReturnAddress());";
    CHECK(nv.find(call) != std::string::npos);
    CHECK(Before(nv, "auto result = o_NvAPI_DRS_GetSetting(hSession, hProfile, settingId, pSetting);", call));
    CHECK(Before(nv, call, "if (pSetting && result == NVAPI_OK)"));
    CHECK(nv.find("#include \"ext/fg/dlssg/PresetDrs.h\"") != std::string::npos);
    CHECK(Count(nv, "OnDrsGetSetting") == 1);

    const std::string dx = Flat(ReadFile(repo + "/OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp"));
    CHECK(Count(dx, "ext::fg::PresetSwitchTick(this, _viewport)") == 1);
    CHECK(Before(dx, "bool DLSSG_Dx12::Dispatch()", "ext::fg::PresetSwitchTick(this, _viewport)"));
    CHECK(Before(dx, "if (ext::fg::PresetSwitchTick(this, _viewport)) // ext: WS2 live DLSS-G render preset change return false;", "SetOptions(true);"));

    const std::string banner = Flat(ReadFile(repo + "/OptiScaler/ext/core/Banner.cpp"));
    CHECK(banner.find("ext::fg::preset::AttachLogRelay();") != std::string::npos);
}

void AutoCostsNothing(const std::string& repo)
{
    const std::string seams = Flat(ReadFile(repo + "/OptiScaler/ext/fg/dlssg/PresetSeams.cpp"));
    // the tick leaves before any step when the configured request is the active one
    CHECK(seams.find("configured == state.Active() || status.restartNeeded") != std::string::npos);
    CHECK(Before(seams, "configured == state.Active()", "switcher.Apply(configured, steps)"));
    // the only caller of Ensure() (which installs hooks) is the prepare step of a switch
    CHECK(Count(seams, "module::Ensure()") == 1);
    CHECK(seams.find("steps.prepare = [] { return module::Ensure(); };") != std::string::npos);

    const std::string mod = Flat(ReadFile(repo + "/OptiScaler/ext/fg/dlssg/PresetModule.cpp"));
    CHECK(Count(mod, "DetourAttach(") == 3);       // reader, create, copy
    CHECK(Count(mod, "DetourTransactionBegin()") == 1);
    CHECK(Count(mod, "Install(picked.module") == 1); // only Ensure installs
    // nothing is hooked before the guards chose a module
    CHECK(Before(mod, "guards::Choose(candidates)", "Install(picked.module"));
    CHECK(Before(mod, "Evaluate(seen[i].module", "guards::Choose(candidates)"));
    // the module stays loaded for the life of the process (the trampolines point into it)
    CHECK(mod.find("GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(picked.module)") != std::string::npos);
    // a refusal is final unless the module was simply not seen yet
    CHECK(mod.find("if (!seen.empty()) g_state.store(State::Failed);") != std::string::npos);
}

void UiRecompositionPause(const std::string& repo)
{
    const std::string seams = Flat(ReadFile(repo + "/OptiScaler/ext/fg/dlssg/DlssgSeams.cpp"));
    CHECK(seams.find("const bool paused = recomposition.enabled && ext::fg::preset::PausesUiRecomposition(ext::fg::preset::Requests().Active());") !=
          std::string::npos);
    CHECK(seams.find("if (recomposition.enabled && !paused)") != std::string::npos);
    CHECK(seams.find("options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;") != std::string::npos);
}

void MenuText(const std::string& repo)
{
    const std::string menu = Flat(ReadFile(repo + "/OptiScaler/ext/fg/MenuFgPreset.cpp"));
    CHECK(menu.find("\"Auto\\0Preset A\\0Preset B\\0Preset C\\0\"") != std::string::npos);
    CHECK(menu.find("Driver queried the setting: %s") != std::string::npos);
    CHECK(menu.find("NVIDIA preset line: %s") != std::string::npos);
    CHECK(menu.find("confirmed: the line has a known format") != std::string::npos);
    CHECK(menu.find("Restart needed: %s. %s stays in use until the next start, then the saved choice applies.") != std::string::npos);
    CHECK(menu.find("Preset A and C turn off NVIDIA's UI recomposition") != std::string::npos);
    CHECK(menu.find("Preset B or Auto brings it back.") != std::string::npos);
    CHECK(menu.find("The runtime did not apply this preset (applied=0)") != std::string::npos);
    CHECK(menu.find("ImGui::BeginDisabled(off);") != std::string::npos);
    CHECK(Count(menu, "ImGui::BeginDisabled(") == Count(menu, "ImGui::EndDisabled();"));
    // drawn from the unlocks card
    const std::string unlocks = Flat(ReadFile(repo + "/OptiScaler/ext/fg/MenuFgUnlocks.cpp"));
    CHECK(unlocks.find("ext::fg::DrawRenderPresetSection(); // WS2") != std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    UpstreamSeams(repo);
    AutoCostsNothing(repo);
    UiRecompositionPause(repo);
    MenuText(repo);
    if (g_failures)
    {
        std::printf("fg_preset_seams_text: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_seams_text: seam order, Auto costs nothing, recomposition pause, menu text passed");
    return 0;
}
