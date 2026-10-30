// Wiring pins of the GPU stage timers. The pure rules live in gpu_timer_stats; this test reads the wired
// sources (comments stripped, whitespace collapsed) and pins the exact statements: the GpuTimers key binding, the
// ini fragment, the manifest entry, the seam calls in the bridge bodies (the failed conversion forgets its slot,
// every other early-return path still closes the pair), the fence-gated D3D12 slot reports, the per-slot book, the
// ReShade event subscription with the live-pass guard, the row labels, and the safety rules (DONOTFLUSH reads only,
// no flush, no wait, no new submission, one warning per timer). A pin that stops matching means a site moved:
// re-derive it, do not loosen it. Source text only, no git inside a test.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static std::string Read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Comments out, string literals kept, every run of whitespace one space.
static std::string Normalize(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '"')
        {
            out += s[i++];
            while (i < s.size() && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.size())
                    out += s[i++];
                out += s[i++];
            }
            out += '"';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
            ++i;
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else if (std::isspace(static_cast<unsigned char>(s[i])))
        {
            if (out.empty() || out.back() != ' ')
                out += ' ';
        }
        else
            out += s[i];
    }
    return out;
}

static bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

static size_t Count(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (auto p = text.find(what); p != std::string::npos; p = text.find(what, p + 1))
        ++n;
    return n;
}

// text between the start of `from` and the next occurrence of `to` after it ("" when missing)
static std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const auto a = text.find(from);
    if (a == std::string::npos)
        return {};
    const auto b = text.find(to, a + from.size());
    return b == std::string::npos ? std::string {} : text.substr(a, b - a);
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const auto ext = repo + "/OptiScaler/ext/";
    const auto present = ext + "present/";
    const auto timers = Normalize(Read(present + "GpuBridgeTimers.cpp"));
    const auto bridge = Normalize(Read(ext + "hdr/BridgeHdr.cpp"));
    const auto seams = Normalize(Read(ext + "Seams.cpp"));
    const auto addon = Normalize(Read(ext + "reshade/ReShadeAddon.cpp"));
    const auto trace = Normalize(Read(present + "FramePacingTrace.cpp"));
    const auto access = Normalize(Read(ext + "dx11bridge/BridgeAccess.h"));
    const auto core = Normalize(Read(ext + "reshade/ReShadeBoundaryCore.cpp"));
    const auto coreHeader = Normalize(Read(ext + "reshade/ReShadeBoundaryCore.h"));
    const auto seamsHeader = Normalize(Read(present + "PresentSeams.h"));
    const auto ini = Read(ext + "ini/FramePacing.ini");
    CHECK(timers.size() > 5000 && bridge.size() > 1000 && seams.size() > 1000 && addon.size() > 1000 &&
          trace.size() > 1000 && ini.size() > 500 && access.size() > 1000 && core.size() > 1000); // the files were read

    // ---- the key: bound once, ini-only, documented --------------------------------------------------------------
    CHECK(Has(Normalize(Read(present + "FramePacingConfig.h")), "CustomOptional<bool, NoDefault> gpuTimers {};"));
    CHECK(Has(Normalize(Read(present + "FramePacingConfig.h")), "GpuTimersKey()"));
    CHECK(Has(Normalize(Read(ext + "core/ExtConfig.cpp")), "b.Add(\"FramePacing\", \"GpuTimers\", gpuTimers);"));
    CHECK(Has(Normalize(Read(ext + "core/MenuManifest.h")), "\"[FramePacing] GpuTimers\""));
    CHECK(Has(ini, "\nGpuTimers=auto"));
    {
        const auto at = ini.find("\nGpuTimers=auto");
        CHECK(at != std::string::npos);
        const auto start = ini.rfind("\n\n", at);
        const std::string block = ini.substr(start == std::string::npos ? 0 : start, at - (start == std::string::npos ? 0 : start));
        CHECK(block.find("No menu control") != std::string::npos);
        CHECK(block.find("Frame pacing GPU [") != std::string::npos);
        CHECK(block.find("same present") != std::string::npos);
        CHECK(block.find("same frame") == std::string::npos);
    }

    // ---- capacity for the rows: the time-row list holds the NR row plus the four timer rows ----------------------
    CHECK(Has(Normalize(Read(ext + "core/MenuRegistry.h")), "kMaxTimeRows = 10"));
    CHECK(Has(Normalize(Read(ext + "core/MenuRegistry.cpp")), "std::array<TimeRow, kMaxTimeRows> rows {};"));

    // ---- the trace decision is shared, not duplicated -------------------------------------------------------------
    CHECK(Has(trace, "bool TraceIsOn() { return TraceOn(); }"));
    CHECK(Has(Normalize(Read(present + "PresentSeams.h")), "bool TraceIsOn();"));
    // the on/off decision is published to an atomic at Present exit; the rows read it, no other entry point needs it
    CHECK(!Has(seamsHeader, "bool GpuTimersOn();"));
    CHECK(Has(seamsHeader, "void GpuTimersCancelInteropCopy(Dx11wDx12SC& sc);"));
    CHECK(Count(timers, "PublishedOn().load(std::memory_order_relaxed)") == 4); // one per menu row
    CHECK(Count(timers, "PublishedOn().store(on, std::memory_order_relaxed)") == 1); // published at Present exit only

    // ---- the frame counter and the Present hook --------------------------------------------------------------------
    {
        const auto ctor = Between(bridge, "PresentScope::PresentScope() {", "PresentScope::~PresentScope()");
        CHECK(!ctor.empty());
        CHECK(Has(ctor, "ext::present::NativeBegin();"));
        CHECK(ctor.find("ext::present::NativeBegin();") < ctor.find("ext::present::GpuTimersBeginFrame();"));
        CHECK(Count(bridge, "ext::present::GpuTimersBeginFrame();") == 1);
        const auto dtor = Between(bridge, "PresentScope::~PresentScope() {", "void BeforeSharedCopy(");
        CHECK(!dtor.empty());
        CHECK(dtor.find("ext::present::GpuTimersOnPresent();") < dtor.find("window::OnPresentEnd();"));
        CHECK(Count(bridge, "ext::present::GpuTimersOnPresent();") == 1);
    }

    // ---- the present copy-in pair (D3D11) ----------------------------------------------------------------------------
    CHECK(Has(bridge, "ext::present::GpuTimersBeginCopyIn(sc);"));
    CHECK(Count(bridge, "ext::present::GpuTimersBeginCopyIn(sc);") == 1);
    CHECK(Has(seams, "ext::present::GpuTimersEndCopyIn(context);"));
    CHECK(Count(seams, "ext::present::GpuTimersEndCopyIn(context);") == 1);
    // a pair always ends on the context it was opened on
    CHECK(Has(timers, "ID3D11DeviceContext* ctx = t.openCtx;"));
    // the D3D11 ring holds exactly the sampled frames of a cycle
    CHECK(Has(timers, "static_assert(D3D11Stage::kRing == kSampledSlots"));

    // ---- the interop copy pair (D3D12): the failed conversion forgets its slot, every other path closes it ------------
    CHECK(Count(bridge, "ext::present::GpuTimersBeginInteropCopy(sc, slot);") == 1);
    CHECK(Count(bridge, "ext::present::GpuTimersEndInteropCopy(sc, slot);") == 1);
    CHECK(Count(bridge, "ext::present::GpuTimersCancelInteropCopy(sc);") == 1);
    {
        // the failed conversion forgets the slot (its list is closed, never executed) before returning false ...
        const auto fail = Between(bridge, "if (!copySource) {", "before = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;");
        CHECK(!fail.empty());
        CHECK(Has(fail, "ext::present::GpuTimersCancelInteropCopy(sc);"));
        CHECK(Has(fail, "return false;"));
        // ... and the screenshot seam closes it before its own early returns
        const auto shot = Between(bridge, "void PrepareScreenshot(", "PresentScope::PresentScope()");
        CHECK(!shot.empty());
        CHECK(shot.find("ext::present::GpuTimersEndInteropCopy(sc, slot);") <
              shot.find("screenshotSdrSource")); // first: the screenshot early returns cannot skip it
        CHECK(Has(Between(bridge, "bool ConvertCopySource(", "if (!Access::Bridge::HdrOutput(sc))"),
                    "ext::present::GpuTimersBeginInteropCopy(sc, slot);")); // before the HDR early return: SDR is timed too
    }
    // one pending slot per timer slot, fenced: a slot reports once the fence passed the value covering its resolve
    CHECK(Has(timers, "void GpuTimersCancelInteropCopy(Dx11wDx12SC& sc)"));
    CHECK(Has(timers, "book.Cancel(t.openSlot)"));
    // the list-mismatch branch forgets the slot: ending on another list would pair stamps of two executions
    CHECK(Has(Between(timers, "if (slot != t.openCopySlot)", "auto* list ="), "t.book.Cancel(t.openSlot)"));
    // the close arms the slot with this list covering fence value and tag, so only its resolve can report it
    CHECK(Has(Between(timers, "void GpuTimersEndInteropCopy(", "void GpuTimersCancelInteropCopy("),
                "CopyAllocatorFenceValue(sc, slot), slot)"));
    CHECK(Has(timers, "Verdict::Recycle"));
    // the read walks every slot, consumes on report (exactly once), and drops a torn-down interop stage
    {
        const auto read12 = Between(timers, "void ReadD3D12(int64_t nowUs)", "void ClearD3D11(");
        CHECK(!read12.empty());
        CHECK(Has(read12, "for (unsigned s = 0; s < SlotBook::kSlots; ++s)"));
        CHECK(Has(read12, "t.book.Consume(s);"));
        CHECK(Has(read12, "Access::Bridge::Dx12Device(*t.bridge) != t.device.Get()"));
        CHECK(Has(read12, "Access::Bridge::CopyFence(*t.bridge) != t.fence.Get()"));
        CHECK(Has(read12, "DropD3D12(t);"));
    }
    // a replaced bridge recovers cleanly: the creation guard drops the stage too, with a fresh warning latch
    CHECK(Has(Between(timers, "bool EnsureD3D11(", "bool EnsureD3D12("), "DropD3D11(t);"));
    CHECK(Has(Between(timers, "bool EnsureD3D12(", "if (t.dead)"), "DropD3D12(t);"));
    CHECK(Has(timers, "GetCompletedValue()"));
    CHECK(Has(timers, "CopyAllocatorFenceValue"));
    CHECK(Has(timers, "openCopySlot = slot;"));
    // the bridge accessors the fence rule reads through (fork file, bounds-checked list value)
    CHECK(Has(access, "ID3D12Fence* CopyFence(Dx11wDx12SC& sc) { return sc._copyFence; }"));
    CHECK(Has(access, "CopyAllocatorFenceValue(const Dx11wDx12SC& sc, UINT slot)"));

    // ---- the ReShade effects pair: the live pass is left out by its own guard, the first runtime wins -----------------
    CHECK(Has(addon, "ext::present::InstallGpuTimerEvents()"));
    CHECK(Count(addon, "ext::present::InstallGpuTimerEvents()") == 1);
    CHECK(addon.find("EnsureSubscribed(") < addon.find("ext::present::InstallGpuTimerEvents()"));
    CHECK(addon.find("ext::present::InstallGpuTimerEvents()") < addon.find("InstallWindowEvent()"));
    CHECK(Has(addon, "subscribed to the effect begin/finish events for the GPU stage timers")); // the add-on INFO line
    CHECK(Has(timers, "register_event<addon_event::reshade_begin_effects>"));
    CHECK(Has(timers, "register_event<addon_event::reshade_finish_effects>"));
    // the handlers take the full event signature (runtime, list, both views): a short signature does not register
    CHECK(Has(timers, "TimerBeginEffects(effect_runtime* runtime, command_list* cmd, resource_view, resource_view)"));
    CHECK(Has(timers, "TimerEndEffects(effect_runtime* runtime, command_list*, resource_view, resource_view)"));
    CHECK(Has(timers, "InProbeCall()")); // the diagnostic probe's own call is not ReShade's end-of-frame work
    CHECK(Has(timers, "InLiveEffectsCall()")); // nor is the live in-place preset pass
    CHECK(Has(timers, "Verdict::KeepOut")); // an unread sample of this frame waits: the first runtime wins
    // the begin handler guards its span per frame: the same frame keeps out, an earlier frame is reaped
    {
        const auto beginFx = Between(timers, "void TimerBeginEffects(", "void TimerEndEffects(");
        CHECK(!beginFx.empty());
        CHECK(Has(beginFx, "if (t.open)"));
        CHECK(Has(beginFx, "DecideEffectsBegin("));
        CHECK(Has(beginFx, "EffectsBegin::KeepOut"));
        CHECK(Has(beginFx, "oc->End(t.end[t.openSlot].Get());")); // the stale span ends where it opened
        CHECK(Has(beginFx, "t.book.Cancel(t.openSlot);")); // its slot is disarmed, never reported
    }
    // the live guard sits around the live call: the scope, then the render_effects call it marks
    CHECK(Has(coreHeader, "bool InLiveEffectsCall();"));
    CHECK(Has(coreHeader, "struct LiveEffectsScope"));
    {
        const auto apply = Between(core, "BoundaryRelease release { _backend };", "const int64_t t1 = _backend.Qpc();");
        CHECK(!apply.empty());
        CHECK(Has(apply, "LiveEffectsScope liveScope;"));
        CHECK(apply.find("LiveEffectsScope liveScope;") < apply.find("_backend.RenderEffects(context, runtime, b);"));
    }
    // the end handler closes the pair on the begin context, never on the finish-event context
    {
        const auto endFx = Between(timers, "void TimerEndEffects(", "bool InstallGpuTimerEvents()");
        CHECK(!endFx.empty());
        CHECK(Has(endFx, "ID3D11DeviceContext* ctx = t.openCtx;"));
        CHECK(Has(endFx, "t.openRuntime != reinterpret_cast<uint64_t>(runtime)")); // only its own span
        CHECK(!Has(endFx, "NativeD3D11Context"));
    }
    CHECK(Has(timers, "device_api::d3d11"));
    CHECK(Has(timers, "GuardedCall("));

    // ---- the rows: one derived plus three timed, beside the untouched upstream Interop row -------------------------------
    CHECK(Has(timers, "RegisterTimeRow({ \"present copy-in\","));
    CHECK(Has(timers, "RegisterTimeRow({ \"ReShade effects\","));
    CHECK(Has(timers, "RegisterTimeRow({ \"interop copy\","));
    CHECK(Has(timers, "RegisterTimeRow({ \"Bridge excl. NR\","));
    CHECK(Count(timers, "RegisterTimeRow({") == 4);
    CHECK(Has(timers, "DlssNr::LastGpuTime()"));
    CHECK(Has(timers, "DlssNr::IsRunning()")); // a failed or idle NR lends no time to the row
    CHECK(Has(timers, "d.name != \"Interop\""));
    CHECK(Has(timers, "BridgeExclNr("));
    CHECK(!Has(timers, "TimedValue")); // neither side carries a frame id: latest values read at the same present
    CHECK(!Has(timers, "SnapshotExcl")); // the row is computed in its row function, never snapshotted
    CHECK(!Has(timers, "ClearExcl"));
    CHECK(!Has(timers, "ExclCache"));
    {
        // the derived row reads its inputs on the menu thread and holds no TimerMutex: the published flag, frame
        // generation really producing frames, NR reporting running, then upstream Interop minus NR own time
        const auto exclRow = Between(timers, "std::optional<double> RowExcl()", "struct GpuTimerRowRegistration");
        CHECK(!exclRow.empty());
        CHECK(Has(exclRow, "DlssNr::IsRunning()"));
        CHECK(Has(exclRow, "IsActive()"));
        CHECK(Has(exclRow, "IsPaused()"));
        CHECK(Has(exclRow, "DlssNr::LastGpuTime()"));
        CHECK(Has(exclRow, "BridgeExclNr("));
        CHECK(!Has(exclRow, "TimerMutex"));
        CHECK(!Has(exclRow, "TraceOn")); // never the trace decision (it can log) from the menu thread
        CHECK(!Has(exclRow, "LOG_"));
    }
    {
        // the Present exit keeps no excl snapshot behind: no per-present cache, window or log line for the row
        const auto onPresent = Between(timers, "void GpuTimersOnPresent() {", "void GpuTimersBeginCopyIn(");
        CHECK(!onPresent.empty());
        CHECK(!Has(onPresent, "SnapshotExcl"));
        CHECK(!Has(onPresent, "Excl()"));
        CHECK(!Has(onPresent, "ClearExcl"));
        CHECK(!Has(onPresent, "BridgeExclNr"));
        // the way off forgets every stage: both D3D11 stages and the interop stage are cleared
        const auto offBranch = Between(onPresent, "if (!on)", "return;");
        CHECK(!offBranch.empty());
        CHECK(Has(offBranch, "ClearD3D11(CopyInStage());"));
        CHECK(Has(offBranch, "ClearD3D11(EffectsStage());"));
        CHECK(Has(offBranch, "ClearD3D12();"));
    }
    {
        // the menu rows never run the trace decision and never log: the published flag, then the last values
        const auto rows = Between(timers, "std::optional<double> RowCopyIn()", "struct GpuTimerRowRegistration");
        CHECK(!rows.empty() && !Has(rows, "TraceOn") && !Has(rows, "LOG_"));
    }
    // ---- the off transition empties the published samples --------------------------------------------------------------
    // The rows show empty while the timers are off, not the last numbers: the way off clears every stage and stores
    // the empty sentinel into all three published samples. A missing store would keep the last number on screen.
    {
        const auto offSentinels = Between(timers, "if (!on)", "return;");
        CHECK(!offSentinels.empty());
        CHECK(Count(offSentinels, "PublishedCopyInMs().store(-1.0, std::memory_order_relaxed);") == 1);
        CHECK(Count(offSentinels, "PublishedEffectsMs().store(-1.0, std::memory_order_relaxed);") == 1);
        CHECK(Count(offSentinels, "PublishedInteropMs().store(-1.0, std::memory_order_relaxed);") == 1);
    }
    // ---- the rows read the published samples, never the lock ----------------------------------------------------------
    // Each row loads the flag and sample published at the last Present exit; a missing sample is published as -1.0 (not
    // zero: zero would claim the stage cost nothing) and reads as empty. No row takes the timer lock: the menu thread
    // never waits on the present thread.
    {
        const auto allRows = Between(timers, "std::optional<double> RowCopyIn()", "struct GpuTimerRowRegistration");
        CHECK(!allRows.empty() && !Has(allRows, "TimerMutex"));
        CHECK(Count(allRows, "PublishedOn().load(std::memory_order_relaxed)") == 4); // one per row
        CHECK(Count(allRows, "PublishedCopyInMs().load(std::memory_order_relaxed)") == 1);
        CHECK(Count(allRows, "PublishedEffectsMs().load(std::memory_order_relaxed)") == 1);
        CHECK(Count(allRows, "PublishedInteropMs().load(std::memory_order_relaxed)") == 1);
        CHECK(Count(allRows, "ms < 0.0 ? std::nullopt : std::optional<double>(ms);") == 3); // one per timed row
        CHECK(Count(timers, "value_or(-1.0), std::memory_order_relaxed);") == 3); // the on-path publishes a missing sample as empty
    }

    CHECK(Has(timers, "FormatGpuLine(GpuStageName("));

    // ---- safety: DONOTFLUSH reads only, no flush, no wait, no map on the frame path, one warning per timer ------------
    {
        // every GetData on the frame path carries DONOTFLUSH (a flag-0 GetData can flush the D3D11 command buffer);
        // each call is checked through its own closing paren, so two calls sharing one statement are still checked
        // one by one
        size_t at = 0, reads = 0;
        bool allFlushFree = true;
        while ((at = timers.find("GetData(", at)) != std::string::npos)
        {
            ++reads;
            size_t end = std::string::npos;
            int depth = 0;
            for (size_t i = at + 7; i < timers.size(); ++i) // at the opening paren of this call
            {
                if (timers[i] == '(')
                    ++depth;
                else if (timers[i] == ')')
                {
                    if (--depth == 0)
                    {
                        end = i;
                        break;
                    }
                }
            }
            if (end == std::string::npos || timers.substr(at, end - at).find("DONOTFLUSH") == std::string::npos)
                allFlushFree = false;
            ++at; // past this hit: two calls can share one statement (the begin/end pair read)
        }
        CHECK(reads == 3 && allFlushFree);
    }
    CHECK(!Has(timers, "->Flush("));
    CHECK(Count(timers, "->Map(") == 1); // only the readback creation maps, once, never on the frame path
    CHECK(Has(timers, "Map(0, nullptr, &mapped)"));
    CHECK(Has(timers, "->Unmap(0, nullptr)")); // a dropped stage ends its persistent map
    CHECK(!Has(timers, "ExecuteCommandLists("));
    CHECK(!Has(timers, "CreateCommandList"));
    CHECK(!Has(timers, "Signal(") && !Has(timers, "->Wait(")); // the timers add no fence edge
    CHECK(!Has(timers, "SetEventOnCompletion")); // completion is read, never waited on
    CHECK(!Has(timers, "WaitForSingleObject"));
    CHECK(Has(timers, "ResolveQueryData"));
    CHECK(Count(timers, "warn.Claim()") == 7); // every creation failure latches: null context/device/queue/fence,
                                               // timestamp frequency, GetDevice, query heap, readback
    CHECK(Count(timers, "warn.Reset()") == 2); // a new device restarts each stage with a fresh latch
    CHECK(Count(timers, "LOG_WARN") == 3); // one warning call per Begin path, after the mutex is released

    // ---- the seam catalog names the new calls -----------------------------------------------------------------------------
    {
        const std::string catalog = Read(repo + "/docs/SEAMS-catalog.md");
        CHECK(Has(catalog, "GpuTimersBeginFrame()"));
        CHECK(Has(catalog, "GpuTimersBeginCopyIn"));
        CHECK(Has(catalog, "GpuTimersBeginInteropCopy"));
        CHECK(Has(catalog, "InstallGpuTimerEvents"));
    }

    {
        // The bridge pointer never outlives the Present that set it: the Present exit clears it after the reads, and
        // the stage detects a new bridge through its own device, queue and fence references, not the pointer.
        const auto onPresent = Between(timers, "void GpuTimersOnPresent() {", "void GpuTimersBeginCopyIn(");
        CHECK(!onPresent.empty() && Count(onPresent, "InteropStage().bridge = nullptr;") == 1);
        CHECK(onPresent.find("ReadD3D12(nowUs);") < onPresent.find("InteropStage().bridge = nullptr;"));
        const auto ensure = Between(timers, "bool EnsureD3D12(", "if (t.dead)");
        CHECK(!ensure.empty() && !Has(ensure, "t.bridge != &sc"));
    }

    {
        // Exact statements, so a negated or dropped gate fails here (a name that is merely present would survive it).
        const auto exclRowExact = Between(timers, "std::optional<double> RowExcl()", "struct GpuTimerRowRegistration");
        CHECK(Count(exclRowExact, "if (!PublishedOn().load(std::memory_order_relaxed)) return std::nullopt;") == 1);
        CHECK(Count(exclRowExact, "if (!fg || !fg->IsActive() || fg->IsPaused()) return std::nullopt;") == 1);
        CHECK(Count(exclRowExact, "if (!DlssNr::IsRunning()) return std::nullopt;") == 1);
        CHECK(Count(exclRowExact, "if (d.name != \"Interop\") continue;") == 1);
        CHECK(Count(exclRowExact, "if (!interop.has_value()) return std::nullopt;") == 1);
        CHECK(Count(exclRowExact, "return BridgeExclNr(interop, DlssNr::LastGpuTime());") == 1);
        const auto beginFxExact = Between(timers, "void TimerBeginEffects(", "void TimerEndEffects(");
        CHECK(Count(beginFxExact, "if (DecideEffectsBegin(true, t.book.Frame(t.openSlot), frame) == EffectsBegin::KeepOut) return;") == 1);
        CHECK(Count(beginFxExact, "AnnotateEnd(oc, t); oc->End(t.end[t.openSlot].Get()); oc->End(t.disjoint[t.openSlot].Get());") == 1);
        CHECK(Count(beginFxExact, "t.book.Cancel(t.openSlot); t.open = false; t.openRuntime = 0; t.openCtx = nullptr;") == 1);
    }

    if (g_failed != 0)
    {
        std::printf("gpu_timer_wiring: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("gpu_timer_wiring: all passed\n");
    return 0;
}
