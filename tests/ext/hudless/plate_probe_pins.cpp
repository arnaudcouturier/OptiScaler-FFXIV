// Text pins of the developer probes' wiring (the probes are log only and off by default, so what must not drift is where they hang):
// every draw hook remembers its arguments behind the one atomic test, the indexed hook notes the draw before its normal path,
// the vertex shader hook stamps its CRC, the detector is armed by the probe, the draw and frame hooks of the
// boundary detector sit behind the probe test, the multiplier is noted after the selector and the menu interlock, the probe key is listed
// as an ini-only key. No packet source is hooked up (no batch/enqueue/bind detours) and there is no overlay: the draw side keeps
// no packet state, draws.csv has no packet columns, and every row counts as no-packet or not-indexed. Comments and whitespace are ignored.
#include <cstdio>
#include <fstream>
#include <initializer_list>
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

std::string Code(const std::string& repo, const char* relative)
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
            continue;
        }
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            out += text[i];
    }
    return out;
}

size_t Find(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

struct Pin
{
    const char* fragment;
    const char* what;
};

// Each fragment (comments and whitespace removed, like the code it is looked up in) must appear exactly once: an edit that changes the line
// removes it, and a line that is copied elsewhere does not hide the edit.
void Pins(const std::string& code, const char* file, std::initializer_list<Pin> pins)
{
    for (const Pin& pin : pins)
    {
        const size_t n = Count(code, pin.fragment);
        EXPECT(n == 1, "%s: %s (%zu occurrences of %s)", file, pin.what, n, pin.fragment);
    }
}
} // namespace

int main(int, char** argv)
{
    const std::string repo = argv[1];

    const std::string hooks = Code(repo, "ffxiv/lighting/FfxivLightingCapture.h");
    EXPECT(Has(hooks, "#include\"ext/ffxiv/ui/PlateDraws.h\""), "include");
    EXPECT(Has(hooks, "#include\"ext/ffxiv/ui/PlateLayoutTag.h\""), "layout tags are written directly, with no plugin callback");
    for (const char* call : { "NoteDraw('D',n,1,start,0);Around(c,\"Draw\"", "NoteDraw('N',n,i,start,0);Around(c,\"DrawInstanced\"",
                              "NoteDraw('X',n,i,start,b);autonormal", "NoteDraw('Y',0,0,0,0);Around(c,\"DrawInstancedIndirect\"",
                              "NoteDraw('Y',0,0,0,0);Around(c,\"DrawIndexedInstancedIndirect\"" })
        EXPECT(Has(hooks, std::string("ext::ffxiv::ui::plate::") + call), "draw hook: %s", call);
    // the indexed hook notes the draw first, then runs its normal path; no replacement layer takes it over
    const size_t note = Find(hooks, "ext::ffxiv::ui::plate::NoteDraw('I',n,1,start,b);");
    const size_t normal = Find(hooks, "normal();}", note);
    EXPECT(note != std::string::npos && normal != std::string::npos && note < normal, "indexed hook order");
    // the instanced hook forwards every draw: a 0-instance draw still reaches the detector, CharaView and the original
    EXPECT(!Has(hooks, "if(i)normal()"), "no instance-count guard on the instanced forward");
    EXPECT(Count(hooks, "};normal();}") == 2, "both the indexed and the instanced hook end in the unconditional forward");
    EXPECT(!Has(hooks, "companionCallbacks"), "no plugin callback on the lighting hooks");
    EXPECT(!Has(hooks, "interceptVisibilityIndexed"), "no visibility intercept");
    EXPECT(!Has(hooks, "OnInterceptedDraw"), "no intercepted draw");
    EXPECT(Has(hooks, "if(SUCCEEDED(hr)&&out&&*out)ext::ffxiv::ui::plate::TagLayout(*out,desc,count,code,size);"), "layout tagging");
    EXPECT(Has(hooks, "if(SUCCEEDED(hr)&&out&&*out)ext::ffxiv::ui::plate::OnVertexShaderCreated(*out,code,size);"), "vertex shader stamp");

    const std::string header = Code(repo, "ffxiv/ui/PlateDraws.h");
    EXPECT(Has(header, "if(ext::ActiveAny(ext::F::PlateProbe,ext::F::DrawDiag))g_args={kind,count,instances,start,baseVertex};"),
           "NoteDraw is behind one atomic test of both probes' bits and only remembers the draw");
    EXPECT(!Has(header, "g_packetPending") && !Has(header, "g_packetHas") && !Has(header, "AssignPacket"),
           "no packet record travels with the draw anymore");
    EXPECT(Has(header, "if(ext::hudless::plate::Enabled())detail::VertexShaderCreated(shader,code,size);"), "vertex shader behind the probe test");
    EXPECT(!Has(header, "InterceptedDraw"), "no intercepted draw in the probe");

    const std::string boundary = Code(repo, "ffxiv/ui/FfxivUiBoundary.cpp");
    EXPECT(Has(boundary, "if(res.counted&&ext::hudless::plate::Enabled())plate::OnGameDraw(context,core,res.ordinal,res.stage,res.cand.r1);"), "draw hook");
    const size_t frameEnd = Find(boundary, "if(ext::hudless::plate::Enabled())plate::OnFrameEnd(C().Frame());");
    const size_t complete = Find(boundary, "FrameOutcomeoutcome=C().CompleteFrame();");
    EXPECT(frameEnd != std::string::npos && complete != std::string::npos && frameEnd < complete, "the frame's rows are handed over before the frame is closed");

    const std::string config = Code(repo, "ffxiv/ui/UiConfig.cpp");
    EXPECT(Has(config, "boolEvaluateArmed(){returnDiagnosticsOn()||ext::hudless::Wanted()||ext::hudless::plate::Enabled();}"), "the probe arms the detector");

    // ---- no packet side anymore: the draw side keeps no packet state and publishes no overlay
    EXPECT(Code(repo, "ffxiv/ui/PlatePackets.h").empty() && Code(repo, "ffxiv/ui/PlatePackets.cpp").empty(), "the packet side files are gone");
    EXPECT(Code(repo, "ffxiv/ui/PlateOverlay.cpp").empty(), "the overlay file is gone");
    const std::string draws = Code(repo, "ffxiv/ui/PlateDraws.cpp");
    EXPECT(!Has(draws, "CurrentPacket") && !Has(draws, "ApplyPacket") && !Has(draws, "PacketInfo"), "no packet lookup on the draw side");
    EXPECT(!Has(draws, "PublishOverlay") && !Has(draws, "CopyOverlay") && !Has(draws, "LastLoggedFrame"), "no overlay is published");
    EXPECT(!Has(draws, "OverlayWanted") && !Has(draws, "CheckOverlaySlot") && !Has(draws, "SetFrameHint"), "no overlay or frame-hint calls");
    EXPECT(!Has(draws, "PlatePackets.h") && !Has(draws, "PlateProbePackets.h"), "no packet headers on the draw side");
    EXPECT(Has(draws, "pf::Counters().NoteRow(row.kind=='I'?pf::RowOutcome::NoPacket:pf::RowOutcome::NotIndexed,pf::BoxReason::Ok);"),
           "every row counts as no-packet or not-indexed");

    // ---- the row cap of draws.csv: asked before a row is built, r0 rows are priority rows, a frame that lost rows says so
    {
        const size_t logged = Find(draws, "if(!pf::LogThisFrame(p.frame)||!(uiPhase||r0))return;if(!p.cap.Admit(r0))return;pf::DrawRowrow;");
        EXPECT(logged != std::string::npos, "game draws: the cap is asked after the schedule and before the row is built, an r0 draw is a priority row");
        EXPECT(!Has(draws, "cap.Admit(true)"), "no priority branch: nothing takes draws over any more");
        EXPECT(Has(draws, "if(p.cap.Capped()){p.rows+=pf::CapMarker(frame,p.cap);pf::Counters().NoteCappedFrame(p.cap.dropped,p.cap.priorityDropped);}p.cap.NextFrame();"),
               "a capped frame gets its marker rows and is counted, then the cap starts again");
        const size_t marker = Find(draws, "p.rows+=pf::CapMarker(frame,p.cap);");
        const size_t flush = Find(draws, "pf::AppendFile(\"draws.csv\",std::move(text));");
        EXPECT(marker != std::string::npos && flush != std::string::npos && marker < flush, "the marker is part of the frame's text that is written");
        EXPECT(!Has(draws, "rowCount") && !Has(draws, "kMaxDrawRowsPerFrame"), "no second, silent cap in the row builder");
    }
    EXPECT(Has(draws, "AddRow(p,row);"), "the row builder hands the row over with no packet step");

    const std::string probe = Code(repo, "hudless/PlateProbe.cpp");
    EXPECT(Has(probe, "if(s.outputs||s.posts||x.assoc.Any())"), "the summary line also prints for the draw probe alone");
    EXPECT(Has(probe, "LOG_INFO(\"{}\",SummaryLine(s,x));p.accounting.ResetInterval();"), "the interval lag values restart after a line was printed");
    // a failed probe takes no more jobs: its writer loop has ended, nobody would drain the queue
    EXPECT(Has(probe, "voidAppendFile(constchar*name,std::stringtext){if(!Enabled())return;Probe&p=P();if(p.failed.load())return;EnsureWriter();"),
           "AppendFile returns once the probe has failed");
    EXPECT(Has(probe, "voidRewriteFile(constchar*name,std::stringtext){if(!Enabled())return;Probe&p=P();if(p.failed.load())return;EnsureWriter();"),
           "RewriteFile returns once the probe has failed");
    EXPECT(Has(probe, "while(!p.failed.load()){"), "the writer loop ends when the probe has failed (which is why the jobs would be left)");
    EXPECT(!Has(probe, "OverlayWanted"), "no overlay switch on the probe");

    const std::string dlssg = Code(repo, "fg/dlssg/DlssgSeams.cpp");
    const size_t adjust = Find(dlssg, "voidAdjustDlssgOptions(IFGFeature*fg,sl::DLSSGOptions&options){");
    const size_t selector = Find(dlssg, "AutoMfgAdjustOptions(fg,options);", adjust);
    const size_t interlock = Find(dlssg, "ApplyMenuDlssgInterlock(options);", adjust);
    const size_t noted = Find(dlssg, "ext::hudless::NoteExpectedMultiplier(options.mode==sl::DLSSGMode::eOn?options.numFramesToGenerate+1:options.mode==sl::DLSSGMode::eOff?1u:0u);", adjust);
    EXPECT(adjust != std::string::npos && selector < interlock && interlock < noted && noted != std::string::npos,
           "the multiplier is noted after the selector and the interlock (eOn: frames + 1, eOff: 1, dynamic: unknown)");

    const std::string manifest = Code(repo, "core/MenuManifest.h");
    EXPECT(Has(manifest, "\"[HudLess]PlateDiagnostics\",") || Has(manifest, "\"[HudLess] PlateDiagnostics\","), "the probe key is ini-only");
    EXPECT(!Has(manifest, "PlateOverlay"), "the retired overlay key is not an ini-only key anymore");

    const std::string cfg = Code(repo, "hudless/HudlessConfig.cpp");
    EXPECT(Has(cfg, "ext::SetActive(ext::F::PlateProbe,plateDiagnostics.value_or_default());"), "the key publishes the probe bit at load");
    EXPECT(!Has(cfg, "plateOverlay"), "the retired overlay key is not bound anymore");
    const std::string cfgHeader = Code(repo, "hudless/HudlessConfig.h");
    EXPECT(Has(cfgHeader, "CustomOptional<bool>plateDiagnostics{false};") && !Has(cfgHeader, "plateOverlay"), "only the probe key is stored");


    // ---- wiring lines the probes' behaviour hangs on (limits, schedules, event fields, counters, the vertex run, the bind), one fragment each
    {
        Pins(probe, "probe", {
            { R"x(constexprint64_tkSummaryEveryMs=10000;)x", "the summary line every 10 seconds" },
            { R"x(constexprint64_tkHoldBackMs=100;)x", "events are held back 100 ms before they are merged" },
            { R"x(constexprsize_tkRingEvents=8192;)x", "the event ring holds 8192 events" },
            { R"x(if(!ec)for(constauto&old:RunsToPrune(names)))x", "run folders are pruned by the rule, once the listing worked" },
            { R"x(if(it->is_directory(isDir)&&!isDir))x", "only folders are listed for pruning" },
            { R"x(if(p.qpcZero==0&&e.type==EventType::Post))x", "the zero of t_ms is the first Post" },
            { R"x(OutputsRow(record,p.qpcZero?p.qpcZero:record.qpc,TicksPerMs()))x", "rows are timed from that zero" },
            { R"x(constboolfits=p.outputsBudget.Take(text.size());)x", "the outputs.csv byte cap is asked before the rows are written" },
            { R"x(p.outputsHeaderWritten=true;)x", "the outputs.csv header is written once" },
            { R"x(p.wasActive=active;)x", "the frame generator state is remembered between frames" },
            { R"x(fg->IsActive()&&!fg->IsPaused())x", "a paused frame generator is not active" },
            { R"x(if(active&&!p.wasActive))x", "one Reset event when the frame generator starts" },
            { R"x(if(seq==0)return;)x", "an Options event before the first Post is dropped" },
            { R"x(e.seq=seq;e.multiplier=outputsPerFrame;)x", "the Options event carries the ordinal of the Post it belongs to" },
            { R"x(e.frame=fg!=nullptr?static_cast<uint64_t>(fg->FrameCount()):0;)x", "the Post event carries the game frame number" },
            { R"x(e.type=EventType::Post;e.qpc=now;e.thread=GetCurrentThreadId();)x", "the Post event carries its thread" },
            { R"x(ext::SetActive(ext::F::PlateProbe,false);)x", "a failed probe clears its feature bit" },
            { R"x(if(p.failed.exchange(true))return;)x", "a failed probe warns once" },
            { R"x(if(all)p.merger.ReleaseAll(ordered);)x", "the flush at shutdown releases everything" },
            { R"x(e.backBuffer=static_cast<int32_t>(chain3->GetCurrentBackBufferIndex());)x", "the Output event carries the back buffer index" },
            { R"x(e.type=EventType::Output;e.qpc=Qpc();e.thread=GetCurrentThreadId();)x", "the Output event carries its thread" },
            { R"x(ok=WriteFile(temp,j.text,false);)x", "a rewrite replaces the file instead of appending" },
            { R"x(if(!ok){Fail("awriteto"+(p.dir/j.name).string()+"failed");return;})x", "a failed write is reported and ends the probe" },
        });
        Pins(draws, "draws", {
            { R"x(constintwhich=frame<=pf::kEarlyFrames?0:1;)x", "frame 600 still belongs to the early byte budget" },
            { R"x(if(pf::RewriteShadersThisFrame(frame)))x", "shaders-plate.csv is rewritten on its own schedule" },
            { R"x(p.budget.Take(frame,text.size()))x", "the draws.csv byte budget is charged what is written" },
            { R"x(elseif(!p.truncatedSent[which]))x", "the truncated marker is written once per budget" },
            { R"x(ordinal>=firstUiOrdinal)x", "the first UI draw belongs to the UI phase" },
            { R"x(constbooldepthPasses=depth.dsvBound&&depth.depthEnable&&depth.depthFunc==pf::kCompareGreaterEqual&&!depth.depthWrite&&!depth.stencilEnable;)x", "the depth part of rule r0 as the cheap prefilter" },
            { R"x(if(uiPhase||depthPasses))x", "the render target is looked up for UI-phase draws and draws that pass the depth part" },
            { R"x(constboolr0=pf::IsCandidate(depth,rtClass);)x", "rule r0 is asked with the looked-up render target class" },
            { R"x(p.table.Add(index,p.frame,r0,rtClass);)x", "the shader table counts the r0 result" },
        });

    }

    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: probe wiring pins (draw hooks, indexed order, arming, frame hand-over, multiplier note, ini-only keys, defaults)");
    return 0;
}
