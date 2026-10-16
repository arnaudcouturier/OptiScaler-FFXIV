#pragma once
// S10 package A: what D1 writes and when (docs/HUDLESS.md section 12), separated from how it reaches the disk so the WARP
// test can run a whole session against an in-memory sink. UiDiag.cpp supplies the real sink (the writer thread).
//
// Files and schedule (frame numbers are the Present counter):
//   README.txt   once, at Begin()
//   frames.txt   the first 600 frames, then every 100th, plus anomaly frames (>= 20 frames apart, at most 400); buffered,
//                flushed at 32 KB, every 300 frames and at frame 120; byte budget 24 MiB then a TRUNCATED marker
//   cost.txt     rewritten at frames 60, 300 and every 600
//   rt.txt       rewritten with cost.txt
//   shaders.csv  rewritten at frames 120, 600 and every 1800 (at most 20000 rows)
//   probe.txt    appended through AppendProbe(), byte budget 1 MiB then a TRUNCATED marker

#include "ext/ffxiv/ui/UiBoundaryCore.h"
#include "ext/ffxiv/ui/UiDiagFormat.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace ext::ffxiv::ui::diag
{
struct Sink
{
    virtual ~Sink() = default;
    virtual void Append(const char* file, std::string text) = 0;  // add to the end of the file
    virtual void Rewrite(const char* file, std::string text) = 0; // replace the whole file
    virtual uint64_t Dropped() { return 0; }                      // jobs the sink had to drop
};

class Recorder
{
  public:
    static constexpr uint64_t kFramesBudget = 24ull << 20;
    static constexpr uint64_t kProbeBudget = 1ull << 20;
    static constexpr size_t kMaxShaderRows = 20000;

    struct Options
    {
        std::string runId;
        double earlyOutNs = 0;
        uintptr_t (*latchedContext)() = nullptr; // the immediate context the draw hooks latched onto (for rt.txt)
        uint64_t framesBudget = kFramesBudget;   // overridable for tests
        uint64_t probeBudget = kProbeBudget;
    };

    Recorder(Sink& sink, Options options) : _sink(sink), _opt(std::move(options))
    {
        _framesBudget.limit = _opt.framesBudget;
        _probeBudget.limit = _opt.probeBudget;
    }

    void Begin()
    {
        _sink.Append("README.txt",
                     "S10 D1 diagnostics, run " + _opt.runId + "\n"
                     "Observation only: nothing in these files changes a draw, a copy or a Present.\n\n"
                     "probe.txt    who owns the Present call chain and the swapchain Present entries (ReShade / Dalamud position)\n"
                     "shaders.csv  one row per pixel shader program (CRC32): stage, UI flag, draws, render targets it drew into\n"
                     "frames.txt   first 600 frames, then every 100th, plus anomaly frames: ordered tagged draws and the R1 / R2 / R3 candidates\n"
                     "cost.txt     draws per frame and cost of the draw hook, per 600 frame window\n"
                     "rt.txt       render target at each candidate vs the swapchain backbuffer; other contexts seen\n"
                     "snap-*       menu button 'Save UI diagnostics snapshot': boundary capture(s) + backbuffer at Present entry of the same frame\n\n"
                     "R1 = first UI-tagged draw after the last scene/post stage; R2 = first draw into the backbuffer after the last stage-5\n"
                     "(fullscreen gamma) draw; r2any = same without the render-target condition; R3 = the earlier of R1 and R2.\n"
                     "Stage numbers: 1 tonemap, 2 post-tonemap, 3 and 4 LUT, 5 fullscreen gamma, 6 UI only, ?? shader created before the hook.\n"
                     "Send this whole folder back; it is bounded (byte budgets per file, see docs/HUDLESS.md section 12).\n");
        _sink.Append("frames.txt",
                     "# S10 D1 frames.txt, format " + std::to_string(dfmt::kFormatVersion) +
                     ". One record per logged frame: `F key=value ...` then indented `d` (draw: ordinal stage render-target-kind shape flags)\n"
                     "# and `p` (draw issued while the bridge's Present was running: seq shader stage kind shape) lines. Render-target kind: BB the\n"
                     "# swapchain backbuffer (pointer identity), BBLIKE another texture of the same size/format, OTHER, NONE (not looked up).\n"
                     "# Listed draws are capped per frame (omitted= counts the rest). 'why': first600 | every100 | anomaly:<what>.\n");
    }

    void AppendProbe(std::string text)
    {
        if (_probeBudget.truncated)
            return;
        if (!_probeBudget.Take(text.size())) // the first refusal writes the marker, later ones are dropped silently
        {
            _sink.Append("probe.txt", dfmt::kTruncatedLine);
            return;
        }
        _sink.Append("probe.txt", std::move(text));
    }

    // Present exit.
    void OnFrame(Core& core, FrameOutcome&& o, const ContextTable::Snapshot& contexts)
    {
        const uint64_t frame = o.record.frame;
        _window.draws.Add(double(o.record.summary.draws));
        _window.hookUs.Add(o.hookUs);
        _window.presentDraws.Add(double(o.record.presentDraws));

        std::string why;
        if (dfmt::LogThisFrame(frame))
            why = frame <= 600 ? "first600" : "every100";
        else if (o.listWorthy && frame - _lastAnomalyFrame >= 20 && _anomalies < 400)
        {
            why = std::string("anomaly:") + o.anomaly;
            _lastAnomalyFrame = frame;
            ++_anomalies;
        }
        if (!why.empty() && !_framesBudget.truncated)
        {
            o.record.reason = why;
            std::string text = dfmt::FrameRecordText(o.record);
            if (_framesBudget.Take(text.size()))
                _framesBuf += text;
            else
                _framesBuf += dfmt::kTruncatedLine; // marker once, later records are dropped
        }
        if (!_framesBuf.empty() && (_framesBuf.size() >= 32768 || frame % 300 == 0 || frame == 120))
        {
            _sink.Append("frames.txt", std::move(_framesBuf));
            _framesBuf.clear();
        }

        if (frame % 600 == 0)
        {
            _windows.push_back(Fmt("frames %llu-%llu | draws mean=%.0f max=%.0f | hook_us mean=%.1f max=%.1f | present_draws mean=%.1f max=%.0f",
                                   (unsigned long long) (frame - 599), (unsigned long long) frame, _window.draws.Mean(),
                                   _window.draws.max, _window.hookUs.Mean(), _window.hookUs.max,
                                   _window.presentDraws.Mean(), _window.presentDraws.max));
            if (_windows.size() > 120)
                _windows.erase(_windows.begin());
            _window = Window {};
        }

        if (frame == 60 || frame == 300 || frame % 600 == 0)
        {
            _sink.Rewrite("cost.txt", CostText(core, contexts));
            _sink.Rewrite("rt.txt", RtText(core, contexts));
        }
        if (frame == 120 || frame == 600 || frame % 1800 == 0)
            _sink.Rewrite("shaders.csv", ShadersText(core));
    }

    // Render-thread microseconds the diagnostics themselves spent on a frame (probes, record building), for cost.txt.
    void AddDiagUs(double us) { _diagUs.Add(us); }

    // Final flush of the buffered frame records (snapshot of what exists when the caller wants everything on disk).
    void Flush()
    {
        if (!_framesBuf.empty())
        {
            _sink.Append("frames.txt", std::move(_framesBuf));
            _framesBuf.clear();
        }
    }

    const dfmt::Rolling& DiagUs() const { return _diagUs; }
    uint32_t Anomalies() const { return _anomalies; }
    const dfmt::ByteBudget& FramesBudget() const { return _framesBudget; }

    std::string CostText(Core& core, const ContextTable::Snapshot& ctx) const
    {
        const Counters& c = core.Stats();
        const double drawsMean = core.DrawsPerFrame().Mean();
        std::string t = "# S10 D1 cost.txt, format " + std::to_string(dfmt::kFormatVersion) + ". Rewritten every 600 frames.\n";
        t += Fmt("run=%s frame=%llu\n", _opt.runId.c_str(), (unsigned long long) core.Frame());
        t += Fmt("early_out_ns_per_call=%.2f   # inline Armed() test in a 5M loop at run start (upper bound: includes the loop); the disarmed cost per draw is this one atomic load\n", _opt.earlyOutNs);
        t += "# cumulative since the first diagnostic Present:\n";
        t += dfmt::RollingText("draws_per_frame", core.DrawsPerFrame()) + "\n";
        t += dfmt::RollingText("hook_us_per_frame", core.HookUsPerFrame()) + "   # QPC around the whole draw hook (PSGetShader, lookups, render target lookups, bookkeeping; includes 2 QPC reads per draw)\n";
        t += Fmt("hook_ns_per_draw_mean=%.1f\n", drawsMean > 0 ? core.HookUsPerFrame().Mean() * 1000.0 / drawsMean : 0.0);
        t += dfmt::RollingText("present_draws_per_frame", core.PresentDrawsPerFrame()) + "   # draws issued while the bridge Present runs (not in hook cost of the game frame)\n";
        t += dfmt::RollingText("diag_us_per_frame", _diagUs) + "   # render-thread work at Present entry/exit (probes, scheduling), EXCLUDED from hook_us\n";
        t += Fmt("game_draws=%llu present_draws=%llu other_thread_draws=%llu draws_without_pixel_shader=%llu draws_with_prehook_shader=%llu render_thread_changes=%llu\n",
                 (unsigned long long) c.gameDraws, (unsigned long long) c.presentDraws, (unsigned long long) c.otherThreadDraws,
                 (unsigned long long) c.noPixelShaderDraws, (unsigned long long) c.preHookDraws,
                 (unsigned long long) c.renderThreadChanges);
        t += Fmt("shader_objects_created_after_hook=%llu prehook_shader_objects_seen=%u writer_jobs_dropped=%llu\n",
                 (unsigned long long) c.shadersCreatedAfterHook, core.PreHookShaders(), (unsigned long long) _sink.Dropped());
        t += Fmt("other_context_draws immediate=%llu deferred=%llu\n", (unsigned long long) ctx.immediateDraws,
                 (unsigned long long) ctx.deferredDraws);
        t += "# 600 frame windows (draws_per_frame | hook_us_per_frame | present_draws_per_frame), newest last:\n";
        for (const auto& w : _windows)
            t += w + "\n";
        return t;
    }

    std::string RtText(Core& core, const ContextTable::Snapshot& ctx) const
    {
        const Counters& c = core.Stats();
        std::string t = "# S10 D1 rt.txt, format " + std::to_string(dfmt::kFormatVersion) + ". Rewritten every 600 frames.\n";
        t += Fmt("frame=%llu backbuffer_width=%llu\n", (unsigned long long) core.Frame(), (unsigned long long) core.BackbufferWidth());
        t += Fmt("frames=%llu with_scene_stage=%llu with_ui_draws=%llu r1=%llu r2=%llu r3=%llu both_same_draw=%llu r1_earlier=%llu "
                 "r2_earlier=%llu scene_but_no_boundary=%llu ui_before_last_scene=%llu with_unknown_shaders=%llu superseded=%llu\n",
                 (unsigned long long) c.frames, (unsigned long long) c.framesWithScene, (unsigned long long) c.framesWithUi,
                 (unsigned long long) c.framesR1, (unsigned long long) c.framesR2, (unsigned long long) c.framesR3,
                 (unsigned long long) c.framesSame, (unsigned long long) c.framesR1First, (unsigned long long) c.framesR2First,
                 (unsigned long long) c.framesNone, (unsigned long long) c.framesUiBeforeScene,
                 (unsigned long long) c.framesWithUnknown, (unsigned long long) c.framesSuperseded);
        t += "# render target bound at the candidate draw, per rule (counts of frames whose candidate draw was listed):\n";
        for (int rule = 1; rule <= 3; ++rule)
            t += Fmt("R%d  BB=%llu BBLIKE=%llu OTHER=%llu NONE=%llu\n", rule, (unsigned long long) c.rtAt[rule][1],
                     (unsigned long long) c.rtAt[rule][2], (unsigned long long) c.rtAt[rule][3],
                     (unsigned long long) c.rtAt[rule][0]);
        t += "# distinct (rule, render target) combinations, first 64:\n";
        for (const auto& e : core.RtEvents())
            t += Fmt("R%u  %s  %s  first_frame=%llu count=%llu\n", unsigned(e.rule), dfmt::RtKindName(e.kind),
                     dfmt::ShapeText(e.shape).c_str(), (unsigned long long) e.firstFrame, (unsigned long long) e.count);
        t += Fmt("# latched immediate context (lighting hooks): 0x%llX\n",
                 (unsigned long long) (_opt.latchedContext ? _opt.latchedContext() : 0));
        t += Fmt("# draws that reached the hook from another context: immediate=%llu deferred=%llu\n",
                 (unsigned long long) ctx.immediateDraws, (unsigned long long) ctx.deferredDraws);
        for (const auto& o : ctx.contexts)
            t += Fmt("  context 0x%llX type=%s draws=%llu first_frame=%llu\n", (unsigned long long) reinterpret_cast<uintptr_t>(o.ptr),
                     o.type == D3D11_DEVICE_CONTEXT_DEFERRED ? "DEFERRED" : "IMMEDIATE", (unsigned long long) o.draws,
                     (unsigned long long) o.firstFrame);
        t += "# CreateDeferredContext calls are NOT observable without a new hook (none is installed by D1). This section only shows\n"
             "# draws of deferred contexts that reached the shared draw hooks; zero here does not prove deferred contexts are unused.\n";
        return t;
    }

    static std::string ShadersText(Core& core)
    {
        auto rows = core.ShaderRows();
        std::ranges::sort(rows, [](const dfmt::ShaderRow& a, const dfmt::ShaderRow& b)
                          { return a.draws != b.draws ? a.draws > b.draws : a.id < b.id; });
        const bool truncated = rows.size() > kMaxShaderRows;
        if (truncated)
            rows.resize(kMaxShaderRows);
        std::string t = dfmt::ShadersCsv(rows);
        if (truncated)
            t += dfmt::kTruncatedLine;
        return t;
    }

  private:
    struct Window
    {
        dfmt::Rolling draws, hookUs, presentDraws;
    };

    static std::string Fmt(const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof buffer, format, args);
        va_end(args);
        return buffer;
    }

    Sink& _sink;
    Options _opt;
    dfmt::ByteBudget _framesBudget, _probeBudget;
    std::string _framesBuf;
    uint64_t _lastAnomalyFrame = 0;
    uint32_t _anomalies = 0;
    Window _window;
    std::vector<std::string> _windows;
    dfmt::Rolling _diagUs;
};
} // namespace ext::ffxiv::ui::diag
