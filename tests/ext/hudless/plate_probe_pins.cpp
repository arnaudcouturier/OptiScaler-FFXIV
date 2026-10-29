// Text pins of the developer probes' wiring (the probes are log only and off by default, so what must not drift is where they hang):
// every draw hook remembers its arguments behind the one atomic test, the indexed hook does it before the visibility intercept and
// reports an intercepted draw, the vertex shader hook stamps its CRC, the detector is armed by the probe, the draw and frame hooks of the
// boundary detector sit behind the probe test, the multiplier is noted after the selector and the menu interlock, the two keys are listed
// as ini-only keys. The packet association hangs on four more hooks (UI batch functions, draw command enqueue, draw state bind, the draw hooks'
// hand-over), each behind the same test and placed after the visibility layer's own call; its code reads only through the active build, never
// touches the visibility layer, only try-locks, and reads packets only on the schedule. The overlay registers once with a slot check and draws
// only while wanted. Comments and whitespace are ignored.
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
    for (const char* call : { "NoteDraw('D',n,1,start,0);Around(c,\"Draw\"", "NoteDraw('N',n,i,start,0);Around(c,\"DrawInstanced\"",
                              "NoteDraw('X',n,i,start,b);autonormal", "NoteDraw('Y',0,0,0,0);Around(c,\"DrawInstancedIndirect\"",
                              "NoteDraw('Y',0,0,0,0);Around(c,\"DrawIndexedInstancedIndirect\"" })
        EXPECT(Has(hooks, std::string("ext::ffxiv::ui::plate::") + call), "draw hook: %s", call);
    // the indexed hook notes the draw first, then lets the visibility layer have it, and records a draw the layer took
    const size_t note = Find(hooks, "ext::ffxiv::ui::plate::NoteDraw('I',n,1,start,b);");
    const size_t intercept = Find(hooks, "if(c==context&&!inside&&companionCallbacks.interceptVisibilityIndexed&&");
    const size_t taken = Find(hooks, "{ext::ffxiv::ui::plate::OnInterceptedDraw(c);return;}");
    EXPECT(note != std::string::npos && intercept != std::string::npos && taken != std::string::npos && note < intercept && intercept < taken,
           "indexed hook order");
    EXPECT(Has(hooks, "if(SUCCEEDED(hr)&&out&&*out)ext::ffxiv::ui::plate::OnVertexShaderCreated(*out,code,size);"), "vertex shader stamp");

    const std::string header = Code(repo, "ffxiv/ui/PlateDraws.h");
    EXPECT(Has(header, "if(ext::hudless::plate::Enabled()){g_args={kind,count,instances,start,baseVertex};if(g_packetPending||g_packetHas)detail::AssignPacket();}"),
           "NoteDraw is behind the probe test and hands the bound packet record to the draw it is for");
    EXPECT(Has(header, "if(ext::hudless::plate::Enabled())detail::VertexShaderCreated(shader,code,size);"), "vertex shader behind the probe test");
    EXPECT(Has(header, "if(ext::hudless::plate::Enabled())detail::InterceptedDraw(context);"), "intercepted draw behind the probe test");

    const std::string boundary = Code(repo, "ffxiv/ui/FfxivUiBoundary.cpp");
    EXPECT(Has(boundary, "if(res.counted&&ext::hudless::plate::Enabled())plate::OnGameDraw(context,core,res.ordinal,res.stage,res.cand.r1);"), "draw hook");
    const size_t frameEnd = Find(boundary, "if(ext::hudless::plate::Enabled())plate::OnFrameEnd(C().Frame());");
    const size_t complete = Find(boundary, "FrameOutcomeoutcome=C().CompleteFrame();");
    EXPECT(frameEnd != std::string::npos && complete != std::string::npos && frameEnd < complete, "the frame's rows are handed over before the frame is closed");

    const std::string config = Code(repo, "ffxiv/ui/UiConfig.cpp");
    EXPECT(Has(config, "boolEvaluateArmed(){returnDiagnosticsOn()||ext::hudless::Wanted()||ext::hudless::plate::Enabled();}"), "the probe arms the detector");

    // ---- the packet association
    const std::string native = Code(repo, "ffxiv/companion/CompanionNative.cpp");
    EXPECT(Has(native, "#include\"ext/ffxiv/ui/PlatePackets.h\""), "native include");
    EXPECT(Has(native, "{Layer::EmitVisibilityCommand(command);ext::ffxiv::ui::plate::OnEnqueue(command);returnenqueue(context,command);}"),
           "enqueue: the visibility layer first, then the probe, then the game");
    EXPECT(Has(native, "{bindState(context,state,instances);Layer::BindVisibilityCommand(context,state);ext::ffxiv::ui::plate::OnBindState(context,state);}"),
           "bind: the game, the visibility layer, then the probe");
    EXPECT(Has(native, "VisibilityScopescope(renderer,2);ext::ffxiv::ui::plate::BatchScopeprobe(base,renderer,2);Invoke(batch16,renderer,mode);"), "batch16 scope");
    EXPECT(Has(native, "VisibilityScopescope(renderer,4);ext::ffxiv::ui::plate::BatchScopeprobe(base,renderer,4);Invoke(batch32,renderer,mode);"), "batch32 scope");

    const std::string packetsHeader = Code(repo, "ffxiv/ui/PlatePackets.h");
    EXPECT(Has(packetsHeader, "if(ext::hudless::plate::Enabled())_active=detail::BeginBatch(base,renderer,indexBytes);"), "batch scope behind the probe test");
    EXPECT(Has(packetsHeader, "if(ext::hudless::plate::Enabled())detail::Enqueue(command);"), "enqueue behind the probe test");
    EXPECT(Has(packetsHeader, "if(ext::hudless::plate::Enabled())detail::Bind(nativeContext,state);"), "bind behind the probe test");

    const std::string packets = Code(repo, "ffxiv/ui/PlatePackets.cpp");
    EXPECT(Has(packets, "if(!pf::ReadOnFrame(hint))returnfalse;"), "packets are read only on the logged-frame schedule");
    // ---- reading the packets of a batch call takes limited time: block reads, a hard limit on the list, a time budget, and no partial set
    {
        EXPECT(Has(packets, "constpf::ListCheckcheck=pf::CheckPacketList(count);"), "the list length is checked against the hard limit");
        EXPECT(Has(packets, "if(check==pf::ListCheck::TooMany){Count(pf::Counters().headerCap);returnfalse;}"),
               "a list over the limit is counted and not read at all");
        EXPECT(!Has(packets, "count>8192") && !Has(packets, "ReadProcessBytes(entry[1]"), "no header-by-header loop and no second list limit in the batch code");
        EXPECT(Has(packets, "pf::CollectRanges(t_entries,allocation,HeaderOffsetsOf(build),static_cast<uint32_t>(ext::ffxiv::kNativeBatchHeaderBytes),b.budget,"),
               "the headers are collected as blocks under the batch's budget");
        const size_t cut = Find(packets, "if(result==pf::CollectResult::Cut){NoteCut(b);b.ranges.Clear();returnfalse;}b.active=true;returntrue;");
        const size_t collect = Find(packets, "pf::CollectRanges(");
        EXPECT(cut != std::string::npos && collect != std::string::npos && collect < cut, "a batch that ran out of time is not made active (no partial set of packets)");
        EXPECT(Has(packets, "b.budget.Begin(BatchBudgetTicks());"), "the budget starts with the batch call");
        EXPECT(Has(packets, "if(b.budget.Exhausted()){NoteCut(b);return;}Spendspend(b);"), "enqueue stops reading when the batch's time is used up, and charges what it reads");
        EXPECT(Has(packets, "~Spend(){b.budget.Add(from,Ticks());}") && Has(packets, "QueryPerformanceCounter(&v);"), "the time is the performance counter");
    }
    EXPECT(Has(packets, "if(!ext::ffxiv::CompanionOffsetsInBounds(build))returnfalse;"), "header offsets are bounds-checked");
    EXPECT(Has(packets, "ActiveBuild().companion"), "offsets come from the active build");
    for (const char* literal : { "0x570", "0x580", "0x588", "0x500", "0x504", "0x1e0", "0x17b8", "0x217c760", "0x240b80", "0x22d750" })
        EXPECT(!Has(packets, literal), "no game offset written as a literal: %s", literal);
    EXPECT(!Has(packets, "VisibilityQueue") && !Has(packets, "Layer::") && !Has(packets, "tracking"), "the probe never touches the visibility layer");
    EXPECT(!Has(packets, "lock_guardlock(shared.mutex)") && !Has(packets, "lock_guardlock(S().mutex)"), "the shared map is never locked for good");
    EXPECT(Has(packets, "std::unique_locklock(shared.mutex,std::try_to_lock);"), "the shared map is try-locked");
    EXPECT(Has(packets, "std::unique_locklock(shared.overlayMutex,std::try_to_lock);"), "the overlay snapshot is try-locked by its reader");
    EXPECT(Has(packets, "if(!g_packetPending||g_args.kind!='I')return;"), "only an indexed draw takes a bound record");
    EXPECT(Has(packets, "pf::MatchDraw(*record,g_args.kind,g_args.count,g_args.start,g_args.baseVertex,NowMs())"), "the record is checked against the draw");

    const std::string draws = Code(repo, "ffxiv/ui/PlateDraws.cpp");
    EXPECT(Has(draws, "SetFrameHint(frame+1);"), "the packet side learns the frame at the frame end");
    EXPECT(Has(draws, "pf::ResolveBox(record->run,record->stride,tagged,layout,tagged?BoundStride(c):0,record->used,box)"),
           "the box needs the strides to agree, and a row without one says why");
    EXPECT(Has(draws, "constbooltagged=!record->run.empty()&&PositionOfBoundLayout(c,layout);"),
           "the layout is looked at only when a vertex run exists");
    EXPECT(Has(draws, "pf::Counters().NoteRow(applied.outcome,applied.box);"), "both the outcome and the no-box reason are counted");

    // ---- the row cap of draws.csv: asked before a row is built, r0 and intercepted rows are priority rows, a frame that lost rows says so
    {
        const size_t logged = Find(draws, "if(!pf::LogThisFrame(p.frame)||!(uiPhase||r0))return;if(!p.cap.Admit(r0))return;pf::DrawRowrow;");
        EXPECT(logged != std::string::npos, "game draws: the cap is asked after the schedule and before the row is built, an r0 draw is a priority row");
        EXPECT(Has(draws, "if(p.core==nullptr||!pf::LogThisFrame(p.frame))return;if(!p.cap.Admit(true))return;"), "intercepted draws are priority rows");
        EXPECT(Has(draws, "if(p.cap.Capped()){p.rows+=pf::CapMarker(frame,p.cap);pf::Counters().NoteCappedFrame(p.cap.dropped,p.cap.priorityDropped);}p.cap.NextFrame();"),
               "a capped frame gets its marker rows and is counted, then the cap starts again");
        const size_t marker = Find(draws, "p.rows+=pf::CapMarker(frame,p.cap);");
        const size_t flush = Find(draws, "pf::AppendFile(\"draws.csv\",std::move(text));");
        EXPECT(marker != std::string::npos && flush != std::string::npos && marker < flush, "the marker is part of the frame's text that is written");
        EXPECT(!Has(draws, "rowCount") && !Has(draws, "kMaxDrawRowsPerFrame"), "no second, silent cap in the row builder");
    }
    EXPECT(Has(draws, "row.boxSource=\"vtx\";"), "box_src says vtx");
    {
        size_t at = 0, n = 0;
        while ((at = Find(draws, "ApplyPacket(c,row,CurrentPacket())", at)) != std::string::npos)
        {
            ++n;
            at += 10;
        }
        EXPECT(n == 2, "both row builders apply the packet: %zu", n);
    }
    EXPECT(Has(draws, "if(ext::hudless::plate::OverlayWanted())CheckOverlaySlot();"), "the overlay slot is checked when the overlay is wanted");

    const std::string probe = Code(repo, "hudless/PlateProbe.cpp");
    EXPECT(Has(probe, "if(s.outputs||s.posts||x.assoc.Any())"), "the summary line also prints for the draw probe alone");
    EXPECT(Has(probe, "LOG_INFO(\"{}\",SummaryLine(s,x));p.accounting.ResetInterval();"), "the interval lag values restart after a line was printed");
    // a failed probe takes no more jobs: its writer loop has ended, nobody would drain the queue
    EXPECT(Has(probe, "voidAppendFile(constchar*name,std::stringtext){if(!Enabled())return;Probe&p=P();if(p.failed.load())return;EnsureWriter();"),
           "AppendFile returns once the probe has failed");
    EXPECT(Has(probe, "voidRewriteFile(constchar*name,std::stringtext){if(!Enabled())return;Probe&p=P();if(p.failed.load())return;EnsureWriter();"),
           "RewriteFile returns once the probe has failed");
    EXPECT(Has(probe, "while(!p.failed.load()){"), "the writer loop ends when the probe has failed (which is why the jobs would be left)");
    EXPECT(Has(probe, "boolOverlayWanted(){returnEnabled()&&ext::Cfg().hudless.plateOverlay.value_or_default();}"), "the overlay needs the probe");

    const std::string overlay = Code(repo, "ffxiv/ui/PlateOverlay.cpp");
    EXPECT(Has(overlay, "if(!pf::OverlaySlotFree(count,ext::menu::kMaxOverlays))"), "the overlay table size is checked");
    EXPECT(Has(overlay, "ext::menu::RegisterOverlay({DrawOverlay,WantsFrame});"), "one overlay is registered");
    EXPECT(Has(overlay, "boolWantsFrame(){returnpf::OverlayShouldDraw(pf::OverlayWanted(),probe::LastLoggedFrame());}"),
           "the menu frame is kept alive only while the overlay is wanted and has a frame to show");
    EXPECT(Has(overlay, "if(!WantsFrame())return;"), "nothing is drawn unless wanted");
    EXPECT(Has(overlay, "ImGui::GetForegroundDrawList()"), "outlines go on the foreground draw list");
    EXPECT(!Has(overlay, "Cfg()"), "no new key: the reading switch is a file-level static");

    const std::string dlssg = Code(repo, "fg/dlssg/DlssgSeams.cpp");
    const size_t adjust = Find(dlssg, "voidAdjustDlssgOptions(IFGFeature*fg,sl::DLSSGOptions&options){");
    const size_t selector = Find(dlssg, "AutoMfgAdjustOptions(fg,options);", adjust);
    const size_t interlock = Find(dlssg, "ApplyMenuDlssgInterlock(options);", adjust);
    const size_t noted = Find(dlssg, "ext::hudless::NoteExpectedMultiplier(options.mode==sl::DLSSGMode::eOn?options.numFramesToGenerate+1:options.mode==sl::DLSSGMode::eOff?1u:0u);", adjust);
    EXPECT(adjust != std::string::npos && selector < interlock && interlock < noted && noted != std::string::npos,
           "the multiplier is noted after the selector and the interlock (eOn: frames + 1, eOff: 1, dynamic: unknown)");

    const std::string manifest = Code(repo, "core/MenuManifest.h");
    EXPECT(Has(manifest, "\"[HudLess]PlateDiagnostics\",") || Has(manifest, "\"[HudLess] PlateDiagnostics\","), "ini-only key 1");
    EXPECT(Has(manifest, "\"[HudLess]PlateOverlay\",") || Has(manifest, "\"[HudLess] PlateOverlay\","), "ini-only key 2");

    const std::string cfg = Code(repo, "hudless/HudlessConfig.cpp");
    EXPECT(Has(cfg, "ext::SetActive(ext::F::PlateProbe,plateDiagnostics.value_or_default());"), "the key publishes the probe bit at load");
    const std::string cfgHeader = Code(repo, "hudless/HudlessConfig.h");
    EXPECT(Has(cfgHeader, "CustomOptional<bool>plateDiagnostics{false};") && Has(cfgHeader, "CustomOptional<bool>plateOverlay{false};"), "defaults are false");


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
            { R"x(if(pf::LogThisFrame(frame)){OverlayFrameshown;shown.frame=frame;)x", "the overlay frame is published for the logged frame itself, only on logged frames" },
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
        Pins(packets, "packets", {
            { R"x(t_batchOrdinal=t_ordinals.NextBatch(hint);if(!pf::ReadOnFrame(hint))returnfalse;)x", "the batch ordinal is counted before the schedule gate" },
            { R"x(if(b.active)returnfalse;)x", "a batch call inside a batch call leaves the outer state alone" },
            { R"x(if(check==pf::ListCheck::Empty)returnfalse;)x", "an empty packet list is not read" },
            { R"x(Count(pf::Counters().batchReads);if(check==pf::ListCheck::TooMany))x", "batch reads are counted before the too-many check" },
            { R"x(b.budget.Add(started,Ticks());)x", "the list read is charged to the batch budget" },
            { R"x(if(b.cutCounted)return;)x", "a cut batch is counted once" },
            { R"x(if(!fields.ok||fields.type!=build.drawCommandType)return;constuint32_tdrawOrdinal=t_ordinals.NextDraw();)x", "only draw commands take a draw ordinal" },
            { R"x(if(!pf::UsableIndexCount(fields.indexCount))return;)x", "unusable index counts are not read" },
            { R"x(if(use.mapped==0)Count(pf::Counters().unmapped);)x", "draws that reach no packet are counted" },
            { R"x(if(shared.map.Store(std::move(record),NowMs())==pf::AssocMap::Put::Full)Count(pf::Counters().refusedCapacity);elseCount(pf::Counters().made);)x", "made and refused records are counted apart" },
            { R"x(record->batch=t_batchOrdinal;)x", "the record carries the batch ordinal" },
            { R"x(record->batchDraw=drawOrdinal;)x", "the record carries the draw ordinal" },
            { R"x(ReadProcessBytes(b.allocation+slotOffset+uintptr_t(plan.first)*stride)x", "the vertex run is read at slot offset plus first vertex times stride" },
            { R"x(record->run.resize(size_t(plan.count)*stride);)x", "the vertex run is count times stride bytes" },
            { R"x(casepf::AssocMap::Bind::Stale:t_boundFailure=pf::RowOutcome::Stale;)x", "a stale record is reported as stale" },
            { R"x(if(state<build.drawStateCommandBackPointerOffset))x", "a state address below the back pointer offset is not a command" },
            { R"x(constuintptr_tcommand=state-build.drawStateCommandBackPointerOffset;)x", "the command is found through the back pointer offset" },
            { R"x(t_bound.reset();t_boundFailure=pf::RowOutcome::NoPacket;g_packetPending=false;if(g_mapSize)x", "a bind drops the previous pending record first" },
            { R"x(if(!lock){Count(pf::Counters().refusedBusy);return;}if(!shared.map.Has(command)))x", "a busy shared map is counted" },
            { R"x(record->hasKey&&record->entries>0)x", "a record counts as a packet only with a key and entries" },
            { R"x(t_boundFailure=pf::RowOutcome::NoPacket;t_bound.reset();return;)x", "a record without a packet resets the failure latch" },
            { R"x(casepf::DrawMatch::Mismatch:t_currentOutcome=pf::RowOutcome::Mismatch;break;)x", "a mismatch is reported as a mismatch" },
            { R"x(info.outcome=g_args.kind=='I'?pf::RowOutcome::NoPacket:pf::RowOutcome::NotIndexed;)x", "a draw without a record is NoPacket only when indexed" },
            { R"x(g_overlayFrame.store(frame.frame,std::memory_order_relaxed);)x", "the overlay frame number is published" },
            { R"x(slot>=0&&slot<=2&&)x", "the renderer has three vertex slots" },
            { R"x(||!cpu||(uint64_t(start)+count)*b.indexBytes>capacity))x", "the index list is checked against the index buffer capacity" },
            { R"x((flags&0x11))x", "the index buffer flag mask" },
        });
        Pins(overlay, "overlay", {
            { R"x(g_noSlot=true;)x", "a full overlay table is remembered" },
            { R"x(if(done.exchange(true))return;)x", "the overlay slot warning runs once" },
            { R"x(if(!registered)LOG_WARN)x", "the warning is for a failed registration" },
        });
    }

    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: probe wiring pins (draw hooks, intercept order, arming, frame hand-over, multiplier note, ini-only keys, defaults)");
    return 0;
}
