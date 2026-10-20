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
    // the tick leaves before any step when the configured request is the active one (and no switch is waiting to be cancelled)
    CHECK(seams.find("(configured == state.Active() && !switcher.Pending()) || status.restartNeeded") != std::string::npos);
    CHECK(Before(seams, "configured == state.Active()", "switcher.Apply(configured, steps, GetTickCount64())"));
    // the read-only check is the only module call while FG still runs; the hooks come from the install step alone
    CHECK(Count(seams, "module::Prepare()") == 1);
    CHECK(Count(seams, "module::Install()") == 1);
    CHECK(Count(seams, "module::Ensure") == 0);
    CHECK(seams.find("steps.evaluate = [] { return module::Prepare(); };") != std::string::npos);
    CHECK(seams.find("steps.install = [] { return module::Install(); };") != std::string::npos);
    // the dispatch of the frame is skipped while a switch is under way
    CHECK(seams.find("if (result == SwitchResult::InProgress)") != std::string::npos);

    // the order lives in the state machine (tested in fg_preset_switch): evaluate then off; release, install, commit later
    const std::string sw = Flat(ReadFile(repo + "/OptiScaler/ext/fg/dlssg/PresetSwitch.h"));
    CHECK(Before(sw, "Run(steps.evaluate)", "Run(steps.off)"));
    CHECK(Before(sw, "Run(steps.release)", "Run(steps.install)"));
    CHECK(Before(sw, "Run(steps.install)", "state_.Commit(requested);"));
    CHECK(sw.find("if (waited_ < kMinWaitTicks || nowMs - offAt_ < kMinWaitMs)") != std::string::npos);

    const std::string mod = Flat(ReadFile(repo + "/OptiScaler/ext/fg/dlssg/PresetModule.cpp"));
    CHECK(Count(mod, "DetourAttach(") == 3);       // reader, create, copy
    CHECK(Count(mod, "DetourTransactionBegin()") == 1);
    CHECK(Count(mod, "InstallHooks(g_picked.seen.module") == 1); // only Install calls it
    // nothing is suspended: no thread scope, and only the calling thread is enlisted
    CHECK(Count(mod, "DetourThreadScope") == 0);
    CHECK(Count(mod, "SuspendThread") == 0);
    CHECK(Count(mod, "DetourUpdateThread(") == 1);
    CHECK(mod.find("DetourUpdateThread(GetCurrentThread())") != std::string::npos);
    // Prepare is read-only: it does not install, and it chooses before it pins
    CHECK(Before(mod, "bool Prepare()", "guards::Choose(candidates)"));
    CHECK(Before(mod, "guards::Choose(candidates)", "g_picked.valid = true;"));
    CHECK(Before(mod, "bool Install()", "InstallHooks(g_picked.seen.module"));
    CHECK(Before(mod, "Evaluate(seen[i], located[i])", "guards::Choose(candidates)"));
    // the module stays loaded for the life of the process (the trampolines point into it)
    CHECK(mod.find("GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(picked.module)") != std::string::npos);
    // only a refusal by the checks is final for a module; an unmapped module is just not a candidate
    CHECK(mod.find("seen[i].refused = true;") != std::string::npos);
    CHECK(Count(mod, "g_installFailed = true;") == 1);
}

void UiRecompositionPause(const std::string& repo)
{
    const std::string seams = Flat(ReadFile(repo + "/OptiScaler/ext/fg/dlssg/DlssgSeams.cpp"));
    // the pause follows what the runtime confirmed (UiRecompositionPaused), not the request
    CHECK(seams.find("const bool paused = recomposition.enabled && ext::fg::preset::UiRecompositionPaused();") != std::string::npos);
    CHECK(seams.find("PausesUiRecomposition(ext::fg::preset::Requests().Active())") == std::string::npos);
    CHECK(seams.find("if (recomposition.enabled && !paused)") != std::string::npos);
    CHECK(seams.find("options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;") != std::string::npos);
    // the restart remark is only for a change of the config / hud-less mode, not for a pause by the preset switch
    CHECK(seams.find("lastEnabled != (int) recomposition.enabled") != std::string::npos);
    const std::string relay = Flat(ReadFile(repo + "/OptiScaler/ext/fg/dlssg/PresetLogRelay.cpp"));
    CHECK(relay.find("return PausesUiRecomposition(request, confirmed.effect, confirmed.uir);") != std::string::npos);
    CHECK(relay.find("if (request != kPresetA && request != kPresetC) return false;") != std::string::npos);
}

void MenuText(const std::string& repo)
{
    const std::string menu = Flat(ReadFile(repo + "/OptiScaler/ext/fg/MenuFgPreset.cpp"));
    CHECK(menu.find("\"Auto\0Preset A\0Preset B\0Preset C\0\"") != std::string::npos);
    CHECK(menu.find("Driver queried the setting: %s") != std::string::npos);
    CHECK(menu.find("NVIDIA preset line: %s") != std::string::npos);
    CHECK(menu.find("confirmed: the line has a known format") != std::string::npos);
    CHECK(menu.find("Restart needed: %s. %s stays in use until the next start, then the saved choice applies.") != std::string::npos);
    // "in use" is only ever said by StatusSentence, from what the runtime confirmed
    CHECK(menu.find("StatusSentence(status.active, confirmed.effect, ReplyReadable())") != std::string::npos);
    CHECK(menu.find("In use") == std::string::npos);
    CHECK(menu.find("Preset C: the 310.9.1.0 runtime ignores it (it reports applied=0); other builds may differ.") != std::string::npos);
    CHECK(menu.find("UI recomposition of NVIDIA's runtime is paused by this preset") != std::string::npos);
    CHECK(menu.find("Preset B or Auto brings it back.") != std::string::npos);
    CHECK(menu.find("Preset C: UI recomposition stays on until the runtime confirms that it acts on C and turns it off.") != std::string::npos);
    CHECK(menu.find("Preset A and C turn off") == std::string::npos);
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
