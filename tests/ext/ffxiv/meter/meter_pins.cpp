// Effect meter (METER): text pins of the wiring, read from the sources (no device). The rules half is meter_rules, the GPU half is meter_warp.
//
// What a wrong wiring would break, and what is pinned here:
//   the seam: the meter is told on EVERY frame, before the capture's gate, by StageRan and StageReport (and by MeterSkipped for the one frame the capture never
//   sees); the entries are function pointers that stay null where the meter is not linked (the host tests of the stages);
//   an unsampled frame pays the counting only: the pass is reached from exactly one place (OnRan), behind the schedule's yes, never from OnReport; the meter
//   never waits (no blocking map, no flushing query read, no wait); its draws are its own work (OwnWork, ReplayScope) so the seam never re-classifies them;
//   the pass touches only what the explicit-state guard covers (two pixel-shader resources, one constant buffer, the OM/IA/VS/PS/RS state): no vertex-shader
//   resource, no geometry/compute/UAV; it draws once and copies only the used corner of its target;
//   the shader and the rules are one definition: the thresholds, the tap hash and its constants, the register layout and the sizes appear identically in
//   Meter.hlsl, MeterRules.h and MeterPass.cpp;
//   the stages: every chain stage that knows its white point hands it over with its run report, and every effect's card ends with the meter's line;
//   the log: the per-effect line and the summary line are written from the rules' formatters only, on the rules' clocks, from both entries;
//   no new config key, no new file the product needs besides the sources and the shader.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

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

std::string Read(const std::string& repo, const std::string& relative)
{
    std::ifstream file(repo + "/" + relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// The source without its comments (whitespace kept).
std::string Stripped(const std::string& text)
{
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

// The body of the function that starts at `signature` (up to its matching brace), or "".
std::string BodyOf(const std::string& code, const std::string& signature)
{
    const size_t at = code.find(signature);
    if (at == std::string::npos)
        return {};
    const size_t open = code.find('{', at);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = open; i < code.size(); ++i)
    {
        if (code[i] == '{')
            ++depth;
        else if (code[i] == '}' && --depth == 0)
            return code.substr(open, i - open + 1);
    }
    return {};
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string ext = "OptiScaler/ext/ffxiv/";
    const std::string meterCpp = Stripped(Read(repo, ext + "meter/Meter.cpp"));
    const std::string pass = Stripped(Read(repo, ext + "meter/MeterPass.cpp"));
    const std::string passH = Stripped(Read(repo, ext + "meter/MeterPass.h"));
    const std::string rules = Stripped(Read(repo, ext + "meter/MeterRules.h"));
    const std::string hlsl = Stripped(Read(repo, ext + "meter/shaders/Meter.hlsl"));
    const std::string props = Read(repo, ext + "meter/meter.shaders.props");
    const std::string header = Squeezed(Stripped(Read(repo, ext + "diag/StageDump.h")));
    EXPECT(!meterCpp.empty() && !pass.empty() && !rules.empty() && !hlsl.empty() && !header.empty(), "all the meter's files are read");

    // ---- the seam -------------------------------------------------------------------------------------------------------------------------------
    {
        EXPECT(Has(header, "inlineMeterRanFng_meterRan=nullptr;") && Has(header, "inlineMeterReportFng_meterReport=nullptr;"), "the entries are null pointers where the meter is not linked");
        EXPECT(Has(header, "{if(detail::g_meterRan!=nullptr)detail::g_meterRan(id,context,in,out,window);if(!StageOpen(id))return;"),
               "StageRan tells the meter before the capture's gate");
        EXPECT(Has(header, "{if(detail::g_meterReport!=nullptr)detail::g_meterReport(id,outcome,reason);if(!StageOpen(id)||detail::g_stageStatus==nullptr)"),
               "StageReport tells the meter before the capture's gate");
        EXPECT(Count(header, "detail::g_meterRan(") == 1 && Count(header, "detail::g_meterReport(") == 2, "the meter is called from the two entries and the one meter-only skip");
        EXPECT(Has(header, "usingMeterRanFn=void(*)(StageIdid,ID3D11DeviceContext*context,ID3D11ShaderResourceView*in,ID3D11ShaderResourceView*out,constStageWindow&window);"),
               "the run entry carries the stage's own input, output and window");
        // the capture's work did not move: still gated by the open mask
        EXPECT(Has(header, "inlineboolStageOpen(StageIdid){return(detail::g_stageOpen.load(std::memory_order_relaxed)&StageBitOf(id))!=0;}"), "the capture's mask is untouched");
        // the meter registers both entries once, from its own static initialiser
        const std::string registers = Squeezed(BodyOf(meterCpp, "RegisterMeter()"));
        EXPECT(Has(registers, "diag::detail::g_meterRan=&OnRan;") && Has(registers, "diag::detail::g_meterReport=&OnReport;"), "the meter installs its two entries");
        EXPECT(Has(Squeezed(meterCpp), "structRegisterMeter"), "installed by a static object");
    }

    // ---- an unsampled frame pays the counting only, and the meter never waits ---------------------------------------------------------------------
    {
        const std::string onRan = Squeezed(BodyOf(meterCpp, "void OnRan("));
        const std::string onReport = Squeezed(BodyOf(meterCpp, "void OnReport("));
        EXPECT(!onRan.empty() && !onReport.empty(), "both entries are found");
        // the pass starts in exactly one place, behind the schedule's yes
        EXPECT(Count(Squeezed(meterCpp), "s.pass.Start(") == 1 && Has(onRan, "s.pass.Start(context,request)"), "the pass is started from OnRan only");
        EXPECT(!Has(onReport, "pass.") && !Has(onReport, "Start(") && !Has(onReport, "Collect("), "OnReport counts and logs, it never touches the pass");
        const size_t due = onRan.find("s.schedule.Due(id,now,token)");
        const size_t start = onRan.find("s.pass.Start(");
        EXPECT(due != std::string::npos && start != std::string::npos && due < start, "the schedule is asked before the pass is touched");
        EXPECT(Has(onRan, "!s.disabled&&InfoOf(id).metered&&in!=nullptr&&out!=nullptr&&context!=nullptr&&s.schedule.Due(id,now,token)"),
               "a disabled meter, an effect without a picture and a call without views never reach the schedule");
        EXPECT(Has(onRan, "s.book.NoteRan(id,token);") && onRan.find("s.book.NoteRan(id,token);") < due, "the frame is counted first");
        EXPECT(Has(onRan, "request.white=window.white>0.0f?window.white:s.lastWhite;"), "the pass measures against the white the stage handed over, else the last one a stage handed over");
        EXPECT(Has(onRan, "if(window.white>0.0f)s.lastWhite=window.white;"), "a stage's white point is remembered for the stages that have none");
        EXPECT(Has(onReport, "caseStageOutcome::Held:s.book.NoteSkip(id,token,\"heldthelastwholeanswer\");break;"),
               "a held answer is a skipped frame with its own reason");
        // a second entry from a thread that is already inside the meter leaves at once, before the lock (the lock is not recursive)
        for (const std::string* body : { &onRan, &onReport })
        {
            const size_t gate = body->find("ReentryGuardreentry;if(!reentry.entered)return;");
            const size_t lock = body->find("std::lock_guard<std::mutex>guard(s.lock);");
            EXPECT(gate != std::string::npos && lock != std::string::npos && gate < lock, "the re-entry gate comes before the lock");
        }
        EXPECT(Has(Squeezed(meterCpp), "thread_localboolt_inMeter=false;") && Has(Squeezed(meterCpp), "entered(!t_inMeter)"), "the re-entry flag is per thread");
        // one frame token: the present counter, not the stage's own counters
        EXPECT(Has(onRan, "ext::present::FrameCount()") && Has(onReport, "ext::present::FrameCount()"), "a rendered frame is a present interval");
        // the draws are the meter's own work
        EXPECT(Count(Squeezed(meterCpp), "ext::ffxiv::chara::ReplayScopereplay;ext::ffxiv::engine::OwnWorkown;") == 2, "the start and the collect run as own work");
        // never blocking
        EXPECT(Has(Squeezed(pass), "D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT"), "the staging read does not wait");
        EXPECT(Count(Squeezed(pass), "D3D11_ASYNC_GETDATA_DONOTFLUSH") >= 4, "every query read is the no-flush one");
        for (const char* banned : { "WaitForSingleObject", "Sleep(", "->Flush()", "ClearState", "CopyResource(", "GetData(_done.Get(),nullptr,0,0)" })
            EXPECT(!Has(Squeezed(pass), banned) && !Has(Squeezed(meterCpp), banned), "the meter never waits or flushes: %s", banned);
        // a pass that cannot be set up stands the meter down for the session (the counting goes on)
        EXPECT(Has(Squeezed(meterCpp), "s.disabled=true;") && Has(Squeezed(meterCpp), "couldnotbecreated"), "an unusable pass stands down");
        // a result that never lands is given up on
        EXPECT(Has(Squeezed(meterCpp), "s.pass.Abandon();") && Has(passH, "void Abandon()"), "the schedule's give-up reaches the pass");
    }

    // ---- the pass touches only what the guard covers ----------------------------------------------------------------------------------------------
    {
        const std::string p = Squeezed(pass);
        EXPECT(Has(p, "constexprext::core::explicit_state::SpeckSpec{2,0,1};"), "two pixel-shader resources, no sampler, one constant buffer");
        EXPECT(Has(p, "ext::core::explicit_state::GuardstateGuard(context,context1.Get(),device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1,kSpec);stateGuard.Neutralise();"),
               "the state is saved before the first setter and neutralised");
        for (const char* banned : { "VSSetShaderResources", "VSSetConstantBuffers", "GSSet", "CSSet", "HSSet", "DSSet", "UnorderedAccess", "SOSetTargets", "SetPredication", "IASetVertexBuffers",
                                    "IASetIndexBuffer", "PSSetSamplers", "RSSetScissorRects" })
            EXPECT(!Has(p, banned), "the pass uses no state the guard does not cover: %s", banned);
        EXPECT(Has(p, "PSSetShaderResources(0,2,bound)") && Has(p, "PSSetShaderResources(0,2,none)"), "t0 and t1 are bound and cleared");
        EXPECT(Has(p, "PSSetConstantBuffers(0,1,buffers)"), "b0");
        EXPECT(Count(p, "Draw(3,0)") == 1, "one draw per measurement");
        EXPECT(Has(p, "constD3D11_BOXcopyBox{0,0,0,geometry.binsX,geometry.binsY,1};") && Has(p, "CopySubresourceRegion(_staging.Get(),0,0,0,0,_target.Get(),0,&copyBox)"),
               "only the used corner of the target is copied");
        EXPECT(Has(p, "RSSetViewports(1,&viewport)") && Has(p, "constD3D11_VIEWPORTviewport{0.0f,0.0f,float(geometry.binsX),float(geometry.binsY),0.0f,1.0f};"), "the viewport is the used corner");
        EXPECT(Has(p, "constexprDXGI_FORMATkTargetFormat=DXGI_FORMAT_R32G32B32A32_FLOAT;"), "the target format is the mandatory one");
        EXPECT(Has(p, "kEffectCount||!InfoOf(request.id).metered") && Has(p, "context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE"), "an effect without a picture and a deferred context are refused");
        EXPECT(Has(p, "viewDesc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D||viewDesc.Texture2D.MostDetailedMip!=0") && Has(p, "desc[i].SampleDesc.Count!=1||desc[i].ArraySize!=1"),
               "only plain 2D single-sample views are read");
        EXPECT(Has(p, "StageCrop(request.x,request.y,request.width,request.height,desc[0].Width,desc[0].Height)"), "the box is the dump's own crop");
    }

    // ---- one definition: shader, rules and pass agree ----------------------------------------------------------------------------------------------
    {
        const std::string h = Squeezed(hlsl);
        const std::string r = Squeezed(rules);
        const std::string p = Squeezed(pass);
        // the tap hash, constant for constant
        for (const char* c : { "73856093u", "19349663u", "83492791u", "2654435761u", "0x5bd1e995u" })
            EXPECT(Has(h, c) && Has(r, c), "the tap hash constant %s is in the shader and the rules", c);
        EXPECT(Has(h, "h^=h>>13;h*=0x5bd1e995u;h^=h>>15;") && Has(r, "h^=h>>13;h*=0x5bd1e995u;h^=h>>15;"), "the hash steps");
        EXPECT(Has(h, "(h&0xFFFFu)%Geom.z") && Has(h, "(h>>16)%Geom.z") && Has(r, "(h&0xFFFFu)%g.cell") && Has(r, "(h>>16)%g.cell"), "the jitter is the low and the high half of the hash");
        // the region thresholds
        for (const char* c : { "254.5", "7.5", "247.5", "0.5" })
            EXPECT(Has(h, c) && Has(r, c), "the region threshold %s is in both", c);
        // the register layout
        EXPECT(Has(h, "cbufferParams:register(b0)") && Has(h, "Texture2D<float4>In:register(t0);") && Has(h, "Texture2D<float4>Out:register(t1);"), "the registers");
        EXPECT(Has(h, "uint4Box;") && Has(h, "uint4Geom;") && Has(h, "uint4Mode;") && Has(h, "float4Meas;"), "four 16-byte registers");
        EXPECT(Has(p, "uint32_tbox[4];uint32_tgeom[4];uint32_tmode[4];floatmeas[4];"), "the same four in the pass");
        EXPECT(Has(p, "buffer.meas[1]=kVisibleLevels;") && Has(r, "inlineconstexprfloatkVisibleLevels=8.0f;"), "the threshold is the rules' constant, passed in");
        EXPECT(Has(h, "change>Meas.y"), "visible is strictly above the threshold");
        EXPECT(Has(h, "saturate(scaled)*255.0") && Has(r, "Saturate(scaled)*kLevelRange"), "levels are saturated and scaled to 255");
        EXPECT(Has(h, "(Mode.z==0u)?v/Meas.x:v"), "the display unit divides by the white, the unorm unit does not");
        EXPECT(Has(h, "returnfloat4(region,visible,sum,inside);"), "the target texel is (region, visible, sum, taps)");
        const std::string reduce = Squeezed(BodyOf(rules, "inline Totals Reduce("));
        EXPECT(Has(reduce, "t.region+=double(row[x*4+0]);") && Has(reduce, "t.visible+=double(row[x*4+1]);") && Has(reduce, "t.sum+=double(row[x*4+2]);") && Has(reduce, "t.taps+=double(row[x*4+3]);"),
               "the target texel is (region, visible, sum, taps), read in that order");
        // the used pixel-shader state is mirrored by the props
        EXPECT(Has(props, "<Args>/Gis</Args>"), "the pixel shader is compiled with IEEE strictness so its isnan() test stays");
        EXPECT(Has(props, "Meter_Vs_cso") && Has(props, "Meter_Ps_cso") && Has(props, "<Entry>VS</Entry>") && Has(props, "<Entry>PS</Entry>"), "the props declare the two entries");
        EXPECT(Has(p, "Meter_Vs_cso") && Has(p, "Meter_Ps_cso"), "the pass uses the generated blobs");
        // no vertex-shader resource in the shader
        EXPECT(!Has(h, "register(t2)") && !Has(h, "SamplerState") && !Has(h, "RWTexture") && !Has(h, "StructuredBuffer"), "the shader reads two textures and nothing else");
    }

    // ---- the stages: the white point and the cards -------------------------------------------------------------------------------------------------
    {
        struct Site
        {
            const char* file;
            const char* id;
            const char* white; // what the run report passes as the window's fifth member, or null when the stage has none of its own
        };
        const Site sites[] {
            { "engine/ShaftsDraw.cpp", "Shafts", "state.whitePoint" },
            { "engine/GiDraw.cpp", "Gi", "state.whitePoint" },
            { "engine/GodraysDraw.cpp", "Godrays", "state.lastRun.whitePoint" },
            { "engine/AtmoDraw.cpp", "Atmosphere", "state.whitePoint" },
            { "enginelight/EngineLightDraw.cpp", "NightGrade", "state.whitePoint" },
        };
        for (const Site& site : sites)
        {
            const std::string code = Squeezed(Stripped(Read(repo, ext + site.file)));
            const std::string run = std::string("StageRan(ext::ffxiv::diag::StageId::") + site.id + ",";
            const size_t at = code.find(run);
            EXPECT(at != std::string::npos, "%s reports its run", site.file);
            const size_t end = at == std::string::npos ? at : code.find(");", at);
            const std::string call = at == std::string::npos ? std::string() : code.substr(at, end - at);
            EXPECT(call.find(std::string("facts.window.height,") + site.white + "}") != std::string::npos, "%s hands its white point to the meter with the run (%s)", site.file, site.white);
        }
        // the night grade's weight 0 reaches the meter outside the capture's guard
        {
            const std::string night = Squeezed(Stripped(Read(repo, ext + "enginelight/EngineLightDraw.cpp")));
            const size_t skip = night.find("ext::ffxiv::diag::MeterSkipped(ext::ffxiv::diag::StageId::NightGrade,");
            const size_t guardAt = night.find("if(ext::ffxiv::diag::StageOpen(ext::ffxiv::diag::StageId::NightGrade)){ext::ffxiv::diag::StageReport(ext::ffxiv::diag::StageId::NightGrade,ext::ffxiv::diag::StageOutcome::Declined,FormatNoWorkReason(");
            EXPECT(skip != std::string::npos && guardAt != std::string::npos && skip < guardAt, "the day frame is counted before the capture-only report");
        }
        // every effect's card ends with the meter's line
        struct Card
        {
            const char* file;
            const char* ids[2];
        };
        const Card cards[] {
            { "engine/AoMenu.cpp", { "Ao", nullptr } },
            { "engine/AtmoMenu.cpp", { "Atmosphere", nullptr } },
            { "engine/DofMenu.cpp", { "Dof", nullptr } },
            { "engine/GiMenu.cpp", { "Gi", nullptr } },
            { "engine/GlossPuddlesMenu.cpp", { "Gloss", "PuddleTone" } },
            { "engine/GodraysMenu.cpp", { "Godrays", nullptr } },
            { "enginelight/EngineLightMenu.cpp", { "NightGrade", nullptr } },
            { "engine/ShaftsMenu.cpp", { "Shafts", nullptr } },
            { "engine/SsrMenu.cpp", { "Ssr", nullptr } },
            { "relight/RelightMenu.cpp", { "ContactShadows", nullptr } },
            { "shadows/ShadowsMenu.cpp", { "ShadowLift", nullptr } },
        };
        for (const Card& card : cards)
        {
            const std::string code = Squeezed(Stripped(Read(repo, ext + card.file)));
            EXPECT(Has(code, "#include\"ext/ffxiv/meter/MeterCard.h\""), "%s includes the card helper", card.file);
            EXPECT(Count(code, "ext::ffxiv::meter::DrawCardLine(") == (card.ids[1] != nullptr ? 2u : 1u), "%s draws the meter's line once per card", card.file);
            for (const char* id : card.ids)
            {
                if (id == nullptr)
                    continue;
                const std::string call = std::string("ext::ffxiv::meter::DrawCardLine(ext::ffxiv::meter::StageId::") + id + ");";
                const size_t at = code.find(call);
                EXPECT(at != std::string::npos, "%s: %s", card.file, id);
                // it is the last thing the card draws: the Draw function closes right after the call(s)
                const size_t afterCalls = at == std::string::npos ? at : at + call.size();
                const std::string rest = afterCalls == std::string::npos ? std::string() : code.substr(afterCalls, 1);
                EXPECT(rest == "}" || (card.ids[1] != nullptr && std::string(id) == card.ids[0] && code.compare(afterCalls, 11, "ext::ffxiv:") == 0),
                       "%s: the meter's line ends the card", card.file);
            }
        }
        const std::string cardH = Squeezed(Stripped(Read(repo, ext + "meter/MeterCard.h")));
        EXPECT(Has(cardH, "ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));ImGui::TextWrapped(\"%s\",CardLine(id).c_str());ImGui::PopStyleColor();"),
               "the line is disabled text, wrapped, balanced");
        const std::string meterH = Squeezed(Stripped(Read(repo, ext + "meter/Meter.h")));
        EXPECT(!Has(meterH, "imgui") && !Has(Squeezed(rules), "imgui") && !Has(Squeezed(rules), "d3d11"), "the rules and the public header include no ImGui and no D3D");
    }

    // ---- the log ----------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string m = Squeezed(meterCpp);
        const std::string logBody = Squeezed(BodyOf(meterCpp, "void LogIfDue("));
        EXPECT(Has(logBody, "s.cadence.WindowDue(now)") && Has(logBody, "s.cadence.SummaryDue(now)"), "both lines ride on the rules' clocks");
        EXPECT(Has(logBody, "LOG_INFO(\"{}\",FormatEffectLine(id,windowMs,frames,window,s.sample[i]));") && Has(logBody, "LOG_INFO(\"{}\",FormatSummaryLine(id,frames,total,s.sample[i]));"),
               "the lines are the rules' formatters, nothing else");
        EXPECT(Has(logBody, "s.book.CloseWindow();s.cadence.TookWindow(now);") && Has(logBody, "s.cadence.TookSummary(now);"), "each clock moves when its line is written");
        EXPECT(Count(m, "LogIfDue(s,now);") == 2, "both entries tick the log");
        EXPECT(Has(m, "Effectvisibilitysummary:"), "the summary block has a header with the meter's own cost");
        // the card reads what the last window published
        EXPECT(Has(Squeezed(BodyOf(meterCpp, "std::string CardLine(")), "FormatCard(armed,p.frames,p.tally,p.sample)"), "the card shows the last window and the latest sample");
        // one mutex, held by every entry (the menu thread reads)
        EXPECT(Count(m, "std::lock_guard<std::mutex>guard(s.lock);") == 3, "OnRan, OnReport and CardLine hold the lock");
    }

    // ---- no key, no new package file ----------------------------------------------------------------------------------------------------------------
    {
        const std::string config = Read(repo, ext + "FfxivConfig.h");
        EXPECT(config.find("EffectMeter") == std::string::npos && config.find("effectMeter") == std::string::npos, "the meter has no config key (it counts and samples, it never changes a picture)");
    }

    if (g_failures)
    {
        std::printf("meter_pins: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("meter_pins: all checks passed\n");
    return 0;
}
