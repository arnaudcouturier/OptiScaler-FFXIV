// Text pins of the per-stage image dump: where each stage reports, and what the report is allowed to be.
//
// The dump has to show what every stage did to the capture frame without changing what any stage renders. That is a property of the call sites:
// each stage reports AFTER its own pass returned and its guards are out of scope (the copy is a plain CopySubresourceRegion on the context the
// pass ran on), BEFORE the stage after it is handed the answer (so the copies are in chain order), with one relaxed load when no capture is open.
// Comments are ignored, and the structural pins below read the code with its whitespace.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

// The source without its comments, whitespace kept (the structural pins walk braces and declarations).
std::string Stripped(const std::string& repo, const char* relative)
{
    std::ifstream file(repo + "/OptiScaler/ext/" + relative, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 2, "//") == 0)
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            out += '\n';
            continue;
        }
        if (text[i] != '\r')
            out += text[i];
    }
    return out;
}

// The same without any whitespace, for the one-line pins.
std::string Squeezed(const std::string& stripped)
{
    std::string out;
    for (const char c : stripped)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

bool IsWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// The matching close of the '{' at `open`, or npos.
size_t CloseOf(const std::string& code, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < code.size(); ++i)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return i;
    }
    return std::string::npos;
}

// The innermost '{' that is still open at `at`.
size_t EnclosingOpen(const std::string& code, size_t at)
{
    int depth = 0;
    for (size_t i = at; i-- > 0;)
    {
        if (code[i] == '}')
            ++depth;
        else if (code[i] == '{')
        {
            if (depth == 0)
                return i;
            --depth;
        }
    }
    return std::string::npos;
}

// Whether a guard of the stage's own work is alive at `at`: an `OwnWork x;`, a `ReplayScope x;` or an explicit-state `Guard x` declared in a block
// that has not closed yet. The tracker flag of the contact pass (TrackGuard) is not one: it keeps the shader tracker out of the way and the copy
// binds and draws nothing.
bool GuardAliveAt(const std::string& code, size_t at)
{
    for (const char* type : { "OwnWork", "ReplayScope", "Guard" })
    {
        const std::string t = type;
        for (size_t d = code.find(t); d != std::string::npos && d < at; d = code.find(t, d + 1))
        {
            if ((d > 0 && IsWord(code[d - 1])) || IsWord(code[d + t.size()]))
                continue;
            size_t name = d + t.size();
            while (name < code.size() && code[name] == ' ')
                ++name;
            if (name == d + t.size() || !IsWord(code[name]))
                continue; // not a declaration (a use of the type's name, a cast)
            size_t end = name;
            while (end < code.size() && IsWord(code[end]))
                ++end;
            while (end < code.size() && code[end] == ' ')
                ++end;
            if (end >= code.size() || (code[end] != ';' && code[end] != '{' && code[end] != '('))
                continue;
            const size_t open = EnclosingOpen(code, d);
            const size_t close = open == std::string::npos ? std::string::npos : CloseOf(code, open);
            if (close == std::string::npos || close > at)
                return true;
        }
    }
    return false;
}

struct Site
{
    const char* file;
    const char* id;       // StageId enumerator
    size_t runCalls;      // StageRan( ... ) calls of that id in the file
    bool beforeChainNext; // the call precedes the stage's ComposeChainNext(context, facts, ours); call
    const char* registerFile = nullptr; // where the stage registers its armed probe, when that is not the file that reports
};
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";

    // ---- every stage reports its run where its pass has returned, outside every guard -----------------------------------------------------------
    const Site sites[] = {
        { "ffxiv/relight/ContactShadowsApply.cpp", "ContactShadows", 1, false, "ffxiv/relight/Inject.cpp" }, // Armed() lives in Inject.cpp
        { "ffxiv/shadows/ShadowsDispatch.cpp", "ShadowLift", 1, false },
        { "ffxiv/engine/AoApply.cpp", "Ao", 1, false },
        { "ffxiv/engine/GlossPuddlesApply.cpp", "Gloss", 1, false },
        { "ffxiv/engine/SsrApply.cpp", "Ssr", 1, false },
        { "ffxiv/engine/ShaftsDraw.cpp", "Shafts", 1, false },
        { "ffxiv/engine/PuddleToneDraw.cpp", "PuddleTone", 1, false }, // both answer sites go through one helper
        { "ffxiv/engine/GodraysDraw.cpp", "Godrays", 1, true },
        { "ffxiv/engine/GiDraw.cpp", "Gi", 1, true },
        { "ffxiv/engine/DofDraw.cpp", "Dof", 1, false },
        { "ffxiv/enginelight/EngineLightDraw.cpp", "NightGrade", 1, true }, // stage-13 (L1b): its pictures are copied before the stage after it (depth of field) is handed the answer
    };
    for (const Site& site : sites)
    {
        const std::string code = Stripped(repo, site.file);
        const std::string run = std::string("ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::") + site.id + ",";
        EXPECT(Count(code, run) == site.runCalls, "%s calls the dump hook for %s exactly %zu time(s), not %zu", site.file, site.id, site.runCalls,
               Count(code, run));
        const size_t at = code.find(run);
        EXPECT(at != std::string::npos, "%s reports its pictures", site.file);
        if (at == std::string::npos)
            continue;
        EXPECT(!GuardAliveAt(code, at), "%s: the copy is made after the stage's own guards went out of scope", site.file);
        if (site.beforeChainNext)
        {
            const size_t next = code.find("ComposeChainNext(context, facts, ours);");
            EXPECT(next != std::string::npos && at < next, "%s: the pictures are copied before the stage after it is handed the answer", site.file);
        }
        const std::string registrar = site.registerFile != nullptr ? Stripped(repo, site.registerFile) : code;
        EXPECT(Has(Squeezed(registrar), std::string("ext::ffxiv::diag::StageRegister(ext::ffxiv::diag::StageId::") + site.id + ","),
                "%s registers its armed probe and values for the capture frame", site.registerFile != nullptr ? site.registerFile : site.file);
        EXPECT(Has(code, "#include \"ext/ffxiv/diag/StageDump.h\""), "%s includes the stage-side header only (no link dependency)", site.file);
    }
    {
        // The flare card makes no picture: status only, and it registers like the others.
        const std::string code = Stripped(repo, "ffxiv/engine/FlareDraw.cpp");
        EXPECT(!Has(code, "StageRan(") && !Has(code, "StageImage("), "the flare card reports no picture");
        EXPECT(Has(code, "StageReport(ext::ffxiv::diag::StageId::Flare, ext::ffxiv::diag::StageOutcome::Ran)"), "... only its run");
        EXPECT(Has(Squeezed(code), "StageRegister(ext::ffxiv::diag::StageId::Flare,"), "... and registers its armed probe");
    }
    {
        // The puddles' tone stage has two answer sites (the fallback tone map and the shafts' chain), both through one helper, and the hold is
        // neither a run nor a decline.
        const std::string code = Stripped(repo, "ffxiv/engine/PuddleToneDraw.cpp");
        EXPECT(Count(code, "ReportAnswer(context, ") == 2, "both answer sites of the tone stage report through the one helper");
        EXPECT(Has(code, "StageOutcome::Held"), "the tone stage's hold has its own status");
        // The first stage of the chain reports the chain's input.
        const std::string shafts = Stripped(repo, "ffxiv/engine/ShaftsDraw.cpp");
        EXPECT(Has(shafts, "StageRan(ext::ffxiv::diag::StageId::Shafts, context, scene.Get(), copy,"), "the first stage's input is the scene the game bound");
    }

    // ---- the white point, the fade, the sun and the tint reach the stage lines of the stages this slice owns (the god rays' writer owns its own) ----------
    {
        const std::string shafts = Stripped(repo, "ffxiv/engine/ShaftsDraw.cpp");
        EXPECT(Count(shafts, "ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Shafts,") == 1, "the light shafts note white, fade, sun and tint once");
        const size_t ran = shafts.find("ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Shafts,");
        const size_t noted = shafts.find("ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Shafts,");
        EXPECT(ran != std::string::npos && noted != std::string::npos && ran < noted, "... right after the pictures are reported");
        const std::string gi = Stripped(repo, "ffxiv/engine/GiDraw.cpp");
        EXPECT(Count(gi, "ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Gi,") == 1, "the light bounce notes white and fade once");
        // The light bounce's capture-frame numbers (the outlier control's share, the filtered mean light; 2026-11-07 integration): one note, inside the
        // capture-frame block, after the frame values.
        EXPECT(Count(gi, "ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Gi, state.captureText);") == 1, "the light bounce notes its capture numbers once");
        EXPECT(gi.find("ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Gi,") < gi.find("ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::Gi, state.captureText);"),
               "... after the frame values, in the same capture-frame block");
        const std::string dof = Stripped(repo, "ffxiv/engine/DofDraw.cpp");
        EXPECT(Has(dof, "ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Dof,"), "depth of field notes the chain's white point and its own fade");
        // The god rays (2026-11-07 integration): one source for its frame numbers, this frame's own, right after its pictures.
        const std::string godrays = Stripped(repo, "ffxiv/engine/GodraysDraw.cpp");
        EXPECT(Count(godrays, "ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Godrays,") == 1, "the god rays note white, fade, sun and tint once");
        const size_t godraysRan = godrays.find("ext::ffxiv::diag::StageRan(ext::ffxiv::diag::StageId::Godrays,");
        const size_t godraysNoted = godrays.find("ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::Godrays,");
        EXPECT(godraysRan != std::string::npos && godraysNoted != std::string::npos && godraysRan < godraysNoted, "... right after its pictures are reported");
        const std::string godraysRules = Stripped(repo, "ffxiv/engine/GodraysRules.h");
        EXPECT(!Has(godraysRules, "white={") && !Has(godraysRules, "sunPx={"), "and its one-run-old snapshot carries none of them (one source per word)");
        // The night grade (L1b, stage-13): the frame values (white, fade 1) and its own words are noted once, through one helper that tests the open bit first and is
        // called right after the pictures are reported (the run) and at the weight-0 decline (a night F9 that misreads 0 must say so, not "not reached").
        const std::string night = Stripped(repo, "ffxiv/enginelight/EngineLightDraw.cpp");
        EXPECT(Count(night, "ext::ffxiv::diag::StageNoteFrame(ext::ffxiv::diag::StageId::NightGrade,") == 1, "the night grade notes white and fade once");
        EXPECT(Count(night, "ext::ffxiv::diag::StageNoteText(ext::ffxiv::diag::StageId::NightGrade, FormatStageWords(") == 1, "... and its own words (weight, curve, white source, share) once");
        EXPECT(Count(night, "NoteStageNumbers(state);") == 2, "... from two places: the run and the weight-0 decline");
        EXPECT(Has(night, "StageReport(ext::ffxiv::diag::StageId::NightGrade, ext::ffxiv::diag::StageOutcome::Declined, FormatNoWorkReason("),
               "a weight-0 frame reports its own decline (name, curve and arm), never \"not reached this frame\"");
        EXPECT(Has(night, "StageReport(ext::ffxiv::diag::StageId::NightGrade, ext::ffxiv::diag::StageOutcome::Declined, DeclineName(reason));"), "every other decline reports its reason");
        for (const std::string* code : { &shafts, &gi, &dof, &godrays, &night })
            EXPECT(Has(*code, "ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::"), "every note is made only while the capture frame is open");
    }

    // ---- the stage-facing header: one relaxed load per entry, null function pointers are a no-op -----------------------------------------------
    {
        const std::string header = Squeezed(Stripped(repo, "ffxiv/diag/StageDump.h"));
        EXPECT(Has(header, "inlineboolStageOpen(StageIdid){return(detail::g_stageOpen.load(std::memory_order_relaxed)&StageBitOf(id))!=0;}"),
               "the open test is one relaxed load of the mask");
        EXPECT(Has(header, "inlinevoidStageImage(StageIdid,StagePortport,ID3D11DeviceContext*context,ID3D11ShaderResourceView*image,constStageWindow&window){if(!StageOpen(id)||"),
               "the image entry tests the mask first");
        EXPECT(Has(header, "inlinevoidStageReport(StageIdid,StageOutcomeoutcome,constchar*reason=nullptr){if(detail::g_meterReport!=nullptr)detail::g_meterReport(id,outcome,reason);if(!StageOpen(id)||"),
               "the status entry tells the effect meter on every frame (one null test), then tests the mask");
        EXPECT(Has(header, "inlinevoidStageRan(StageIdid,ID3D11DeviceContext*context,ID3D11ShaderResourceView*in,ID3D11ShaderResourceView*out,constStageWindow&window){if(detail::g_meterRan!=nullptr)detail::g_meterRan(id,context,in,out,window);if(!StageOpen(id))return;"),
               "the run entry tells the effect meter on every frame (one null test), then tests the mask before it touches either picture");
        EXPECT(Has(header, "inlinevoidMeterSkipped(StageIdid,constchar*reason){if(detail::g_meterReport!=nullptr)detail::g_meterReport(id,StageOutcome::Declined,reason);}"),
               "a frame left alone for a reason the capture's status line never sees reaches the meter alone");
        EXPECT(Has(header, "floatwhite=0.0f;"), "the window carries the stage's white point for the meter (0 = none of its own)");
        EXPECT(!Has(header, "Cfg()") && !Has(header, "ExtConfig"), "no key: the stage dump rides on DrawDiagnosticsDump = frame");
        EXPECT(Has(header, "inlinestd::atomic<uint32_t>g_stageOpen{0};"), "the mask starts closed");
        EXPECT(Has(header, "inlinevoidStageNoteText(StageIdid,conststd::string&text){if(!StageOpen(id)||detail::g_stageLate==nullptr)return;"),
               "the late text entry tests the mask and the hook before it stores anything");
        EXPECT(Has(header, "inlinevoidStageNoteFrame(StageIdid,constStageFrameValues&values){if(!StageOpen(id)||detail::g_stageLate==nullptr)return;"),
               "the white point / fade / sun / tint entry too");
        EXPECT(Has(header, "inlinevoidStageProbe(StageIdid,ID3D11DeviceContext*context,ID3D11ShaderResourceView*texel,StageProbeFormatFnformat){if(!StageOpen(id)||"),
               "the probe entry tests the mask first: the one-texel copy is made on the capture frame only");
        EXPECT(Has(header, "inlinefloatStageFrameWhite()"), "the chain's later stages can read the white point an earlier stage noted this capture frame");
    }

    // ---- the runtime: the copy sets no state, the mask follows the capture frame --------------------------------------------------------------------
    {
        const std::string raw = Stripped(repo, "ffxiv/diag/FfxivDrawDiag.cpp");
        const size_t from = raw.find("void RefreshStageOpen(State& s)");
        const size_t to = raw.find("void DumpCopyPending(", from);
        EXPECT(from != std::string::npos && to != std::string::npos && from < to, "the stage section sits in front of the dump's copy-pending step");
        const std::string section = from != std::string::npos && to != std::string::npos ? raw.substr(from, to - from) : std::string();
        EXPECT(Count(section, "CopySubresourceRegion(") == 2, "two copies: the image hook's, and the probe hook's one texel on the capture frame");
        for (const char* forbidden : { "->Draw", "->Dispatch", "->OMSet", "->PSSet", "->VSSet", "->IASet", "->RSSet", "->CSSet", "ClearRenderTarget",
                                       "ClearUnorderedAccess", "ClearDepth", "->Map(", "->Unmap(", "->Flush", "UpdateSubresource", "GenerateMips" })
            EXPECT(!Has(section, forbidden), "the stage copy sets no pipeline state and draws nothing (%s)", forbidden);
        EXPECT(Has(Squeezed(section), "if(!s.dumpActive||s.dumpWritten||c==nullptr||view==nullptr||!StageInfoOf(id).images)return;"),
               "a closed dump costs nothing: the hook returns before any D3D call");
        EXPECT(Has(Squeezed(section), "constStageBoxbox=mip==0?StageCrop(window.x,window.y,window.width,window.height,levelWidth,levelHeight)"),
               "the crop is the stage's own window, not the whole texture");
        EXPECT(Has(Squeezed(section), "DumpMakeRoom(holds,count,DumpRank::Stage,bytes,s.dumpHeldBytes,s.dumpBudget)"),
               "a stage image that does not fit takes the places of the lower ranks, inside the dump's own budget");
        const std::string squeezed = Squeezed(raw);
        EXPECT(Count(squeezed, "RefreshStageOpen(s);") >= 5, "the mask is republished wherever the capture's frame state moves");
        EXPECT(Has(squeezed, "detail::g_stageImage=&StageImageHook;detail::g_stageStatus=&StageStatusHook;"), "the hooks are installed by the runtime only");
        EXPECT(Has(squeezed, "detail::g_stageLate=&StageLateHook;detail::g_stageProbe=&StageProbeHook;"), "the late-values and probe hooks too");
        EXPECT(Has(squeezed, "ResolveStageProbes(c,s);facts+=StageFactsText(s);"), "a probe's staging copy is mapped where the dump maps its other readbacks, right before the stage lines are written");
        EXPECT(Has(squeezed, "text+=StageStatusLine(StageId(i),s.stageRecords[i],s.stageArmed[i],StageValuesWithLate(s.stageValues[i],s.stageLate[i]));"),
               "the stage line is the snapshot of the keys plus the late numbers: the line's format is unchanged");
        EXPECT(Has(squeezed, "for(std::string&late:s.stageLate)late.clear();s.stageProbes.clear();"), "a capture starts the late numbers and the probes over");
        EXPECT(Has(squeezed, "if(!s.dumpActive||s.dumpWritten||size_t(id)>=kStageCount)return;"), "the late hook stores only while a dump is armed and unwritten");
        EXPECT(Has(squeezed, "ext::ffxiv::diag::InstallStageHooks();"), "... from the diagnostic's static initialiser");
        EXPECT(Has(squeezed, "constboolstageImage=item->stage;"), "the stage images have places of their own beside the dump's twenty-four");
        EXPECT(Has(squeezed, "item.rank=uint8_t(DumpRank::Stage);item.stage=true;item.copied=true;"), "every stage image is marked as one when it is made");
        EXPECT(Has(squeezed, "DumpFits(stageImage?0:written,bytes,item->bytes,s.dumpBudget)"), "... and the bytes are the dump's budget");
        EXPECT(Has(squeezed, "facts+=StageFactsText(s);"), "every stage's status line reaches facts.txt");
        EXPECT(Has(squeezed, "RefreshStageOpen(s);s.stageSources.clear();"), "the stage images' texture identities are released the moment the dump is written");
        EXPECT(Has(squeezed, "s.dumpItems.clear();s.dumpHeldBytes=0;s.stageSources.clear();RefreshStageOpen(s);"),
               "... and when a capture finishes, whichever way it ended");
        EXPECT(Has(squeezed, "ext::SetActive(ext::F::DrawDiag,false);s.stageSources.clear();RefreshStageOpen(s);"), "... and when the key is switched off");
    }

    // ---- no new key ---------------------------------------------------------------------------------------------------------------------------
    {
        const std::string schema = Stripped(repo, "ffxiv/FfxivConfig.h");
        EXPECT(!Has(schema, "StageDump") && !Has(schema, "stageDump") && !Has(schema, "StageImages"), "FfxivConfig.h has no stage-dump key");
        const std::string ini = Stripped(repo, "ffxiv/ini/FFXIV.ini");
        EXPECT(!Has(ini, "StageDump") && !Has(ini, "StageImages"), "FFXIV.ini has no stage-dump key: DrawDiagnosticsDump = frame is the one switch");
    }

    if (g_failures)
    {
        std::printf("stage_dump_pins: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("stage_dump_pins: all checks passed\n");
    return 0;
}
