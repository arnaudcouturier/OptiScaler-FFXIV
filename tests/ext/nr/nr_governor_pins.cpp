// Source-text pins for the NR frame-rate governor outside its pure core (which has its own simulated-time test):
// the keys with their defaults and homes, the one-place run-time cap after the scale choice, the off path that
// reads nothing, the once-per-build probe, the VRAM gate mark, the AutoMFG expected-stall seam and the hotkey
// toast. They keep the structure that makes the behaviour true; the behaviour itself runs in the other test.
#include <cstdio>
#include <fstream>
#include <iterator>
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

std::string Squash(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

std::string Read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return Squash(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string nr = repo + "/OptiScaler/ext/nr/";
    const std::string pass = Read(nr + "pass/DlssNr_Dx12.cpp");
    const std::string configH = Read(nr + "NrConfig.h");
    const std::string configCpp = Read(nr + "NrConfig.cpp");
    const std::string ini = Read(repo + "/OptiScaler/ext/ini/DlssNr.ini");
    const std::string manifest = Read(repo + "/OptiScaler/ext/core/MenuManifest.h");
    const std::string menu = Read(nr + "menu/DlssNr_Menu.cpp");
    const std::string nrMenu = Read(nr + "menu/NrMenu.cpp");
    const std::string presets = Read(nr + "presets/NrUserPresets.h");
    const std::string schema = Read(repo + "/tools/settings-schema.json");
    const std::string bridge = Read(nr + "amd/AmdBridge.cpp");
    const std::string gov = Read(nr + "NrGovernor.h");
    const std::string lifetime = Read(repo + "/OptiScaler/ext/gpu/GpuLifetime.h");
    const std::string control = Read(repo + "/OptiScaler/ext/fg/DynamicMfgControl.h");
    const std::string seams = Read(repo + "/OptiScaler/ext/fg/FgSeams.h");
    const std::string glue = Read(repo + "/OptiScaler/ext/fg/DynamicMfgSeams.cpp");
    CHECK(pass.size() > 100000 && menu.size() > 10000 && nrMenu.size() > 1000);

    // ---- keys: defaults, binder, ini, ini-only list, menu, presets, schema -------------------------------
    // defaults are today's behaviour (off, auto, off): the governor never runs unless asked
    CHECK(Count(configH, "CustomOptional<bool>governor{false};") == 1);
    CHECK(Count(configH, "CustomOptional<float>governorTargetFps{0.0f};") == 1);
    CHECK(Count(configH, "CustomOptional<bool>governorProbe{false};") == 1);
    // each key is bound once to its own member
    CHECK(Count(configCpp, "b.Add(\"DlssNr\",\"Governor\",governor);") == 1);
    CHECK(Count(configCpp, "b.Add(\"DlssNr\",\"GovernorTargetFps\",governorTargetFps);") == 1);
    CHECK(Count(configCpp, "b.Add(\"DlssNr\",\"GovernorProbe\",governorProbe);") == 1);
    // a bad saved target (not finite, outside 0..1000) is dropped, like the AutoMFG target
    CHECK(Has(configCpp, "governorTargetFps.has_value()") && Has(configCpp, "governorTargetFps.reset();"));
    CHECK(Has(configCpp, "governorTargetFps.value()>1000.0f"));
    // the ini fragment carries each key with the repo comment style
    CHECK(Count(ini, "Governor=auto") == 1);
    CHECK(Count(ini, "GovernorTargetFps=auto") == 1);
    CHECK(Count(ini, "GovernorProbe=auto") == 1);
    CHECK(Has(ini, "Stepsdownafter3sbelow95%ofthetarget;probesonetierupafter10satorabove97%"));
    // only the probe is ini-only: it has no menu control and the governor never uses it
    const size_t iniOnly = manifest.find("kIniOnlyKeys");
    CHECK(iniOnly != std::string::npos);
    if (iniOnly != std::string::npos)
    {
        const std::string list = manifest.substr(iniOnly, 4000);
        CHECK(Count(list, "\"[DlssNr]GovernorProbe\",") == 1);
        CHECK(!Has(list, "\"[DlssNr]Governor\",") && !Has(list, "\"[DlssNr]GovernorTargetFps\","));
    }
    // the two running keys have menu controls, the probe has none
    CHECK(Count(menu, "ImGui::Checkbox(\"Framerategovernor(experimental)\",&governor)") == 1);
    CHECK(Has(menu, "ImGui::SliderFloat(\"GovernortargetFPS\",&governorTarget,0.0f,500.0f,"));
    CHECK(Has(menu, "Manualvaluesbelow30fpsareraisedto30fps."));
    CHECK(Has(menu, "DlssNr::Governor::StatusLine(DlssNr::CurrentGovernorStatus()).c_str()"));
    CHECK(!Has(menu, "GovernorProbe"));
    // the running keys travel with the presets, pre-governor files upgrade to the defaults, the probe does not
    CHECK(Has(presets, "NR_FIELD(Governor,governor,0,1);NR_FIELD(GovernorTargetFps,governorTargetFps,0,500);"));
    CHECK(Count(presets, "if(!j.contains(\"Governor\"))j[\"Governor\"]=false;") == 1);
    CHECK(Count(presets, "if(!j.contains(\"GovernorTargetFps\"))j[\"GovernorTargetFps\"]=0.0;") == 1);
    CHECK(!Has(presets, "GovernorProbe"));
    // the settings reference documents all three keys and the probe rule
    CHECK(Count(schema, "\"key\":\"[DlssNr]Governor\"") == 1);
    CHECK(Count(schema, "\"key\":\"[DlssNr]GovernorTargetFps\"") == 1);
    CHECK(Count(schema, "\"key\":\"[DlssNr]GovernorProbe\"") == 1);
    CHECK(Has(schema, "probesonetierupafter10satorabove97%ofthetarget"));

    // ---- the one-place cap: after the scale choice, before every consumer --------------------------------
    const size_t scales = pass.find("constDlssNr::AxisScalesaxisScales=DlssNr::EffectiveScales(");
    const size_t capBlock = pass.find("if(g_nr.governorOut.cap>0.0f)");
    const size_t adopted = pass.find("autorequested=DlssNr::Spatial::MakeGeometry(");
    CHECK(scales != std::string::npos && capBlock != std::string::npos && adopted != std::string::npos);
    CHECK(scales < capBlock && capBlock < adopted); // the guides, vectors, geometry and settle all read capped
    CHECK(Count(pass, "scaleX=std::min(scaleX,g_nr.governorOut.cap);") == 1);
    CHECK(Count(pass, "scaleY=std::min(scaleY,g_nr.governorOut.cap);") == 1);
    if (scales != std::string::npos && adopted != std::string::npos && adopted > scales)
    {
        // a run-time cap only: the block reads the keys and never assigns one
        const std::string block = pass.substr(scales, adopted - scales);
        CHECK(!Has(block, "nr.workingScale=") && !Has(block, "nr.workingScaleX="));
        CHECK(!Has(block, "nr.workingScaleY=") && !Has(block, "nr.governor="));
        CHECK(!Has(block, "nr.governorTargetFps=") && !Has(block, "nr.governorProbe="));
    }
    // off costs a reset and nothing more: no refresh query, no VRAM gate on the off path
    const size_t offBranch = pass.find("if(!nr.governor.value_or_default())");
    const size_t refresh = pass.find("GetActiveRefreshRate");
    const size_t gateAsk = pass.find("RetirementsComplete(g_nr.governorReleaseSeq)");
    CHECK(offBranch != std::string::npos && refresh != std::string::npos && gateAsk != std::string::npos);
    CHECK(offBranch < refresh && offBranch < gateAsk);
    // The reset comes first in that branch: turning the key off drops the governor's own state (tier, average,
    // dwells), so switching it back on starts a fresh transition instead of resuming the tier it held. Zeroing the
    // published output alone would leave that state behind for the first on-tick to pick up again.
    CHECK(Has(pass, "if(!nr.governor.value_or_default()){g_nr.governor.Reset();g_nr.governorOut=DlssNr::Governor::Output{};"));
    CHECK(Has(pass, "g_nr.governorOut=DlssNr::Governor::Output{};g_nr.governorDbgHas=false;"));
    CHECK(Count(pass, "RetirementsComplete(g_nr.governorReleaseSeq)") == 1); // asked only while on
    // the feed: one read of the current feature, live activity (not selection), the observed count
    CHECK(Has(pass, "govIn.fgFeature=govFg!=nullptr;"));
    CHECK(Has(pass, "govIn.fgActive=govFg!=nullptr&&govFg->IsActive()&&!govFg->IsPaused();"));
    CHECK(Has(pass, "govIn.unsupportedFg=govIn.fgActive&&!govDlssg;"));
    CHECK(Has(pass, "govIn.multiplier=govIn.fgActive&&govDlssg&&govDetected>0?(double)(govDetected+1):0.0;"));
    CHECK(Has(pass, "govIn.targetFps=ext::fg::ResolveTarget(govTargetCfg,govLimit,govRefresh);"));
    // ---- the probe: one line per build, nothing else ----------------------------------------------------------------
    // An open probe owns the verdict: while an up step is under judgment, a rate under the down edge holds the tier
    // and says so, so the down arm never judges a transition it did not cause. That guard is the only below-target
    // exit ahead of the down dwell; the simulated-time test cannot reach it (the verdict always lands before the down
    // dwell runs out), so its position in front of the down arm is pinned here.
    CHECK(Has(gov, "reason_=Reason::BelowTarget;returnFinish(out,in.nowMs);}"));
    CHECK(Has(gov, "if(probeOpen_){//Anopenprobeownstheverdict"));
    {
        const size_t guardAt = gov.find("if(probeOpen_){//Anopenprobeownstheverdict");
        const size_t downAt = gov.find("constintnext=FirstStepForCeiling(ceiling);");
        CHECK(guardAt != std::string::npos && downAt != std::string::npos && guardAt < downAt);
    }
    CHECK(Count(pass, "if(nr.governorProbe.value_or_default()&&!g_nr.governorProbeLogged&&g_nr.feature!=nullptr){") == 1);
    CHECK(Count(pass, "nr.governor.value_or_default()") == 2); // off branch plus the menu readout: the probe never asks the key
    CHECK(Count(pass, "g_nr.governorProbeLogged=true;") == 1); // latched: one line per build
    CHECK(Count(pass, "g_nr.governorProbeLogged=false;") == 1); // a new build gets its own probe line
    CHECK(Has(pass, "governorprobe:evaluatebelowthecreatedsizeof"));
    CHECK(Has(pass, "notavailable(theNR"));
    {
        const size_t probe = pass.find("if(nr.governorProbe.value_or_default()");
        CHECK(probe < offBranch); // independent: the probe block sits before the off branch and runs whatever Governor says
        CHECK(probe != std::string::npos);
        if (probe != std::string::npos)
        {
            // never a model failure, a latch change or a history reset on the probe path
            const std::string probeBlock = pass.substr(probe, 1200);
            CHECK(!Has(probeBlock, "FailModel") && !Has(probeBlock, "g_nr.reset"));
            CHECK(!Has(probeBlock, "AnamorphicLatch("));
        }
    }
    // ---- the once-a-second debug line: the signal the governor decides on ------------------------------------------
    CHECK(Count(pass, "if(govIn.nowMs-g_nr.governorDbgAt>=1000)") == 1); // governor-on only, once a second
    CHECK(Has(pass, "DLSS-NRgovernor:interval"));
    CHECK(Has(pass, "govIn.fgFrame-g_nr.governorDbgFrame")); // the counter advance in the last second
    CHECK(Count(pass, "g_nr.governorDbgHas") == 3); // off-path reset, first-line guard, latch
    CHECK(Has(pass, "g_nr.governorDbgHas?(int64_t)(govIn.fgFrame-g_nr.governorDbgFrame):0;")); // signed, no advance on the first line
    CHECK(Has(pass, "DlssNr::Governor::ReasonText(g_nr.governorOut.reason)"));
    // ---- the VRAM gate: the mark a work-size park advances, nothing else -------------------------------------------
    CHECK(Count(pass, "g_nr.governorReleaseSeq=g_nrLifetime.RetireSeq();") == 1);
    CHECK(Has(pass, "itsscopecoversbothsets")); // a frame-size change advances the mark too
    CHECK(Has(pass, "withoutmovingthegovernormarkfurther")); // the frame-sized copies retire without moving it
    CHECK(Has(lifetime, "uint64_tRetireSeq();"));
    CHECK(Has(lifetime, "boolRetirementsComplete(uint64_tseq);"));
    CHECK(Has(lifetime, "Onlythoseretirementshold//theanswer:laterones"));
    // ---- the expected-stall seam: both NR call sites, one forwarder, two ignored ticks --------------------------------
    CHECK(Count(pass, "ext::fg::NoteModelRebuild();") == 2); // the rebuild decision and the first build
    CHECK(Has(pass, "Anyrebuildhitchesitsframe:tellAutoMFGtoignorethenexttwointervals"));
    CHECK(Has(pass, "Anewbuildgetsitsownprobeline,andithitchesitsframelikeanyrebuild."));
    CHECK(Count(seams, "voidNoteModelRebuild();") == 1);
    CHECK(Count(glue, "g_glue.controller.NoteExternalStall();") == 1);
    // the seam arms one selector restart plus a watch restart on both ticks, and skips the stall average on both
    CHECK(Has(control, "voidNoteExternalStall(){externalStall_=true;}"));
    CHECK(Has(control, "constboolignoredTick=externalStall_||externalStallWatch_;if(ignoredTick)"));
    CHECK(Has(control, "if(w_.open&&ignoredTick){")); CHECK(Has(control, "}elseif(w_.open&&stalled_){")); // the rebuild ticks record nothing in the watch
    CHECK(Has(control, "externalStall_=false;externalStallWatch_=true;"));
    CHECK(Has(control, "}elseexternalStallWatch_=false;"));
    CHECK(Has(control, "if(w_.open)w_.StartOver(now);"));
    CHECK(Has(control, "}elseUpdateStall(in);")); // skipped on both ignored ticks
    CHECK(Has(control, "recordingresumesonthenexttick")); // accepted trade: a real re-creation stall on the ignored ticks is not seen
    CHECK(Has(control, "!ignoredTick&&st_.at4x")); // the self-test readout never takes the ignored interval
    CHECK(Has(control, "externalStall_=externalStallWatch_=false;")); // cleared while the gate is closed
    // ---- the hotkey toast: override only while per-axis keys win ------------------------------------------------------
    CHECK(Has(nrMenu, "if(PerAxisOverrides()&&!DlssNr::AnamorphicLatchHolds(toastLatch,"));
    CHECK(Count(nrMenu, "content+=\"(per-axiskeysoverride)\";") == 1);
    // ---- the capped size follows the menu and the enlargement key; the AMD path states its exception -------------------
    CHECK(Count(pass, "constGovernor::Outputgovernor=CurrentGovernorStatus();") == 1);
    CHECK(Count(pass, "scales.x=std::min(scales.x,governor.cap);") == 1);
    CHECK(Count(pass, "scales.y=std::min(scales.y,governor.cap);") == 1);
    CHECK(Count(menu, "DlssNr::ShownEffectiveScales(uniformShown)") == 1);
    CHECK(Has(bridge, "Thegovernorcapdoesnotapplyhere"));

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: NR frame-rate governor wiring pins (keys, one-place cap, probe, gate, seam, toast)");
    return 0;
}
