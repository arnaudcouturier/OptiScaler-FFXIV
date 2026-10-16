#include "pch.h"

#include "ext/ffxiv/ui/UiDiag.h"

#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/ffxiv/ui/UiConfig.h"
#include "ext/ffxiv/ui/UiProbe.h"
#include "ext/hdr/screenshot/Image.h"

#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace ext::ffxiv::ui::diag
{
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t kFramesBudget = 24ull << 20;   // frames.txt
constexpr uint64_t kProbeBudget = 1ull << 20;     // probe.txt
constexpr uint64_t kImageBudget = 200ull << 20;   // raw bytes of all snapshot images of one run
constexpr size_t kMaxShaderRows = 20000;
constexpr size_t kMaxQueue = 512;

std::string Fmt(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    return buffer;
}

int64_t Qpc()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t QpcFreq()
{
    static const int64_t f = []
    {
        LARGE_INTEGER v {};
        QueryPerformanceFrequency(&v);
        return v.QuadPart > 0 ? v.QuadPart : 1;
    }();
    return f;
}

// ---- the writer thread -----------------------------------------------------------------------------------------------
struct Job
{
    enum class Kind : uint8_t { Append, Rewrite, Image } kind = Kind::Append;
    fs::path path;
    std::string text;
    std::vector<uint8_t> bytes;
    uint32_t width = 0, height = 0, format = 0, pitch = 0, frame = 0;
};

bool PngFormat(uint32_t f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

void WriteFile(const fs::path& path, const void* data, size_t size, bool append)
{
    std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (file)
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void Execute(const Job& job)
{
    switch (job.kind)
    {
    case Job::Kind::Append:
        WriteFile(job.path, job.text.data(), job.text.size(), true);
        break;
    case Job::Kind::Rewrite:
    {
        fs::path temp = job.path;
        temp += L".tmp";
        WriteFile(temp, job.text.data(), job.text.size(), false);
        MoveFileExW(temp.c_str(), job.path.c_str(), MOVEFILE_REPLACE_EXISTING);
        break;
    }
    case Job::Kind::Image:
    {
        if (PngFormat(job.format))
        {
            fs::path png = job.path;
            png += L".png";
            try
            {
                ext::hdr::screenshot::image::SavePng(png, job.bytes.data(), job.width, job.height, job.pitch, false,
                                                     static_cast<DXGI_FORMAT>(job.format));
                return;
            }
            catch (...)
            {
                // fall through to the raw container so the capture is not lost
            }
        }
        dfmt::RawHeader header;
        header.width = job.width;
        header.height = job.height;
        header.format = job.format;
        header.rowPitch = job.pitch;
        header.frame = job.frame;
        fs::path raw = job.path;
        raw += L".raw";
        std::ofstream file(raw, std::ios::binary | std::ios::trunc);
        if (file)
        {
            file.write(reinterpret_cast<const char*>(&header), sizeof header);
            file.write(reinterpret_cast<const char*>(job.bytes.data()), static_cast<std::streamsize>(job.bytes.size()));
        }
        break;
    }
    }
}

class Writer
{
  public:
    void Enqueue(Job&& job)
    {
        {
            std::lock_guard lock(_mutex);
            if (_queue.size() >= kMaxQueue && job.kind != Job::Kind::Image)
            {
                ++_dropped;
                return;
            }
            _queue.push_back(std::move(job));
            if (!_started)
            {
                _started = true;
                std::thread([this] { Run(); }).detach();
            }
        }
        _cv.notify_one();
    }
    uint64_t Dropped()
    {
        std::lock_guard lock(_mutex);
        return _dropped;
    }

  private:
    void Run()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); // WIC for the PNG encoder; the thread lives until the process ends
        while (true)
        {
            Job job;
            {
                std::unique_lock lock(_mutex);
                _cv.wait(lock, [this] { return !_queue.empty(); });
                job = std::move(_queue.front());
                _queue.pop_front();
            }
            try
            {
                Execute(job);
            }
            catch (...)
            {
            }
        }
    }

    std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<Job> _queue;
    bool _started = false;
    uint64_t _dropped = 0;
};

// ---- run state ------------------------------------------------------------------------------------------------------
struct Window // one 600 frame window of cost numbers
{
    dfmt::Rolling draws, hookUs, presentDraws;
};

struct State_
{
    std::mutex statusMutex;
    Status status;

    Writer writer;
    bool ready = false, failed = false;
    fs::path dir;
    std::string runId;
    double earlyOutNs = 0;

    dfmt::ByteBudget framesBudget { kFramesBudget }, probeBudget { kProbeBudget };
    uint64_t imageBytes = 0;
    std::string framesBuf;
    uint64_t lastAnomalyFrame = 0;
    uint32_t anomalies = 0;

    std::vector<probe::ModuleInfo> modules;
    Window window;
    std::vector<std::string> windows; // finished windows, one line each
    dfmt::Rolling diagUs;
    int64_t diagTicks = 0;
    uint64_t lastLoggedFrame = 0;
};

State_& S()
{
    static auto* s = new State_; // never destroyed
    return *s;
}

fs::path OptiScalerDir()
{
    wchar_t path[32768] {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OptiScalerDir), &module) ||
        !GetModuleFileNameW(module, path, 32768))
        return {};
    return fs::path(path).parent_path() / "OptiScaler";
}

void Queue(Job::Kind kind, const char* file, std::string text)
{
    auto& s = S();
    Job j;
    j.kind = kind;
    j.path = s.dir / file;
    j.text = std::move(text);
    s.writer.Enqueue(std::move(j));
}

void AppendProbe(std::string text)
{
    auto& s = S();
    if (s.probeBudget.truncated)
        return;
    if (!s.probeBudget.Take(text.size())) // the first refusal writes the marker, later ones are dropped silently
    {
        Queue(Job::Kind::Append, "probe.txt", dfmt::kTruncatedLine);
        return;
    }
    Queue(Job::Kind::Append, "probe.txt", std::move(text));
}

double BenchArmedNs()
{
    // Upper bound for the disarmed hook cost: the inline Armed() test in a loop (the loop's own cost is included).
    volatile uint32_t sink = 0;
    constexpr int kIterations = 5'000'000;
    const int64_t t0 = Qpc();
    for (int i = 0; i < kIterations; ++i)
        sink = sink + (Armed() ? 1u : 0u);
    const int64_t t1 = Qpc();
    return double(t1 - t0) * 1e9 / double(QpcFreq()) / double(kIterations);
}

bool EnsureRun()
{
    auto& s = S();
    if (s.ready)
        return true;
    if (s.failed)
        return false;
    try
    {
        const fs::path root = OptiScalerDir() / "hudless-diag";
        if (root.parent_path().empty())
        {
            s.failed = true;
            return false;
        }
        std::tm local {};
        const std::time_t now = std::time(nullptr);
        if (localtime_s(&local, &now) != 0)
        {
            s.failed = true;
            return false;
        }
        s.runId = dfmt::FormatRunId(local);
        s.dir = root / s.runId;
        std::error_code ec;
        fs::create_directories(s.dir, ec);
        if (ec || !fs::is_directory(s.dir, ec))
        {
            LOG_WARN("HudLess diagnostics: cannot create {}: {}", s.dir.string(), ec.message());
            s.failed = true;
            return false;
        }

        std::vector<std::string> names;
        ec.clear();
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code isDir;
            if (it->is_directory(isDir) && !isDir)
                names.push_back(it->path().filename().string());
        }
        if (!ec)
            for (const auto& old : dfmt::RunsToPrune(names))
            {
                if (old == s.runId)
                    continue;
                std::error_code rm;
                fs::remove_all(root / old, rm);
            }

        s.earlyOutNs = BenchArmedNs();

        Queue(Job::Kind::Append, "README.txt",
              "S10 D1 diagnostics, run " + s.runId + "\n"
              "Observation only: nothing in these files changes a draw, a copy or a Present.\n\n"
              "probe.txt    who owns the Present call chain and the swapchain Present entries (ReShade / Dalamud position)\n"
              "shaders.csv  one row per pixel shader program (CRC32): stage, UI flag, draws, render targets it drew into\n"
              "frames.txt   first 600 frames, then every 100th, plus anomaly frames: ordered tagged draws and the R1 / R2 / R3 candidates\n"
              "cost.txt     draws per frame and cost of the draw hook, per 600 frame window\n"
              "rt.txt       render target at each candidate vs the swapchain backbuffer; other contexts seen\n"
              "snap-*       menu button 'Save UI diagnostics snapshot': boundary capture + backbuffer at Present entry of the same frame\n\n"
              "R1 = first UI-tagged draw after the last scene/post stage; R2 = first draw into the backbuffer after the last stage-5\n"
              "(fullscreen gamma) draw; r2any = same without the render-target condition; R3 = the earlier of R1 and R2.\n"
              "Stage numbers: 1 tonemap, 2 post-tonemap, 3 and 4 LUT, 5 fullscreen gamma, 6 UI only, ?? shader created before the hook.\n"
              "Send this whole folder back; it is bounded (see the byte budgets in docs/HUDLESS.md section 12).\n");
        Queue(Job::Kind::Append, "frames.txt",
              "# S10 D1 frames.txt, format " + std::to_string(dfmt::kFormatVersion) +
              ". One record per logged frame: `F key=value ...` then indented `d` (draw: ordinal stage render-target-kind shape flags)\n"
              "# and `p` (draw issued while the bridge's Present was running: seq shader stage kind shape) lines. Render-target kind: BB the\n"
              "# swapchain backbuffer (pointer identity), BBLIKE another texture of the same size/format, OTHER, NONE (not looked up).\n"
              "# Listed draws are capped per frame (omitted= counts the rest). 'why': first600 | every100 | anomaly:<what>.\n");
        s.ready = true;
        LOG_INFO("HudLess diagnostics (D1): run folder {}", s.dir.string());
        std::lock_guard lock(s.statusMutex);
        s.status.running = true;
        s.status.runDir = s.dir.string();
        return true;
    }
    catch (...)
    {
        s.failed = true;
        return false;
    }
}

// ---- file bodies ----------------------------------------------------------------------------------------------------
std::string CostText(Core& core, const ContextTable::Snapshot& ctx)
{
    auto& s = S();
    const Counters& c = core.Stats();
    const double drawsMean = core.DrawsPerFrame().Mean();
    std::string t = "# S10 D1 cost.txt, format " + std::to_string(dfmt::kFormatVersion) + ". Rewritten every 600 frames.\n";
    t += Fmt("run=%s frame=%llu\n", s.runId.c_str(), static_cast<unsigned long long>(core.Frame()));
    t += Fmt("early_out_ns_per_call=%.2f   # inline Armed() test in a 5M loop at run start (upper bound: includes the loop); the disarmed cost per draw is this one atomic load\n", s.earlyOutNs);
    t += "# cumulative since the first diagnostic Present:\n";
    t += dfmt::RollingText("draws_per_frame", core.DrawsPerFrame()) + "\n";
    t += dfmt::RollingText("hook_us_per_frame", core.HookUsPerFrame()) + "   # QPC around the whole draw hook (PSGetShader, lookups, render target lookups, bookkeeping; includes 2 QPC reads per draw)\n";
    t += Fmt("hook_ns_per_draw_mean=%.1f\n", drawsMean > 0 ? core.HookUsPerFrame().Mean() * 1000.0 / drawsMean : 0.0);
    t += dfmt::RollingText("present_draws_per_frame", core.PresentDrawsPerFrame()) + "   # draws issued while the bridge Present runs (not in hook cost of the game frame)\n";
    t += dfmt::RollingText("diag_us_per_frame", s.diagUs) + "   # render-thread work at Present exit (record building, scheduling), EXCLUDED from hook_us\n";
    t += Fmt("game_draws=%llu present_draws=%llu other_thread_draws=%llu draws_without_pixel_shader=%llu draws_with_prehook_shader=%llu\n",
             (unsigned long long) c.gameDraws, (unsigned long long) c.presentDraws, (unsigned long long) c.otherThreadDraws,
             (unsigned long long) c.noPixelShaderDraws, (unsigned long long) c.preHookDraws);
    t += Fmt("shader_objects_created_after_hook=%llu prehook_shader_objects_seen=%u writer_jobs_dropped=%llu\n",
             (unsigned long long) c.shadersCreatedAfterHook, core.PreHookShaders(), (unsigned long long) s.writer.Dropped());
    t += Fmt("other_context_draws immediate=%llu deferred=%llu\n", (unsigned long long) ctx.immediateDraws,
             (unsigned long long) ctx.deferredDraws);
    t += "# 600 frame windows (draws_per_frame | hook_us_per_frame | present_draws_per_frame), newest last:\n";
    for (const auto& w : s.windows)
        t += w + "\n";
    return t;
}

std::string RtText(Core& core, const ContextTable::Snapshot& ctx)
{
    const Counters& c = core.Stats();
    std::string t = "# S10 D1 rt.txt, format " + std::to_string(dfmt::kFormatVersion) + ". Rewritten every 600 frames.\n";
    t += Fmt("frame=%llu backbuffer_width=%llu\n", static_cast<unsigned long long>(core.Frame()),
             static_cast<unsigned long long>(core.BackbufferWidth()));
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
             (unsigned long long) reinterpret_cast<uintptr_t>(ext::ffxiv::lighting::FfxivLightingCapture::context));
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

std::string ShadersText(Core& core, bool& truncated)
{
    auto rows = core.ShaderRows();
    std::ranges::sort(rows, [](const dfmt::ShaderRow& a, const dfmt::ShaderRow& b)
                      { return a.draws != b.draws ? a.draws > b.draws : a.id < b.id; });
    truncated = rows.size() > kMaxShaderRows;
    if (truncated)
        rows.resize(kMaxShaderRows);
    std::string t = dfmt::ShadersCsv(rows);
    if (truncated)
        t += dfmt::kTruncatedLine;
    return t;
}

void Publish(Core& core, const ContextTable::Snapshot&)
{
    auto& s = S();
    const Counters& c = core.Stats();
    std::lock_guard lock(s.statusMutex);
    auto& st = s.status;
    st.frames = c.frames;
    st.drawsPerFrame = core.DrawsPerFrame().Mean();
    st.hookUsPerFrame = core.HookUsPerFrame().Mean();
    st.hookNsPerDraw = st.drawsPerFrame > 0 ? st.hookUsPerFrame * 1000.0 / st.drawsPerFrame : 0.0;
    st.shadersAfterHook = c.shadersCreatedAfterHook;
    st.preHookShaders = core.PreHookShaders();
    st.framesR1 = c.framesR1;
    st.framesR2 = c.framesR2;
    st.framesSame = c.framesSame;
    st.framesNone = c.framesNone;
    st.framesWithScene = c.framesWithScene;
    st.droppedJobs = s.writer.Dropped();
}
} // namespace

std::string RunDir()
{
    auto& s = S();
    std::lock_guard lock(s.statusMutex);
    return s.status.runDir;
}

Status GetStatus()
{
    auto& s = S();
    std::lock_guard lock(s.statusMutex);
    return s.status;
}

void SetSnapshotStatus(uint32_t presses, uint32_t images, const std::string& state)
{
    auto& s = S();
    std::lock_guard lock(s.statusMutex);
    s.status.snapshotPresses = presses;
    s.status.snapshotImages = images;
    s.status.snapshotState = state;
}

void AppendText(const char* file, std::string text)
{
    if (!S().ready)
        return;
    Queue(Job::Kind::Append, file, std::move(text));
}

bool ImageBudgetLeft(uint64_t bytes)
{
    auto& s = S();
    if (s.imageBytes + bytes > kImageBudget)
        return false;
    s.imageBytes += bytes;
    return true;
}

void SubmitImage(std::string baseName, std::vector<uint8_t> pixels, uint32_t width, uint32_t height, uint32_t dxgiFormat,
                 uint32_t rowPitch, uint32_t frame)
{
    auto& s = S();
    if (!s.ready)
        return;
    Job j;
    j.kind = Job::Kind::Image;
    j.path = s.dir / baseName;
    j.bytes = std::move(pixels);
    j.width = width;
    j.height = height;
    j.format = dxgiFormat;
    j.pitch = rowPitch;
    j.frame = frame;
    s.writer.Enqueue(std::move(j));
}

void OnPresentEntry(Core& core)
{
    if (!DiagnosticsOn() || !EnsureRun())
        return;
    const int64_t t0 = Qpc();
    const uint64_t frame = core.Frame();
    auto& s = S();
    const bool stack = frame <= 5 || frame == 600 || frame == 3600;
    const bool vtable = frame == 1 || frame == 600 || frame == 3600;
    const bool modules = frame == 1 || frame == 600 || frame == 3600 || frame == 7200;
    if (frame == 1)
        AppendProbe("# S10 D1 probe.txt, format " + std::to_string(dfmt::kFormatVersion) + ", run " + s.runId +
                    "\n# Evidence lines are addresses and module paths; lines starting '# inference' are conclusions drawn from them.\n"
                    "# Probe frames: call chain at Present 1-5, 600, 3600; vtables at 1, 600, 3600; module list at 1 and the additions at 600, 3600, 7200.\n\n");
    if (modules && frame == 1)
        AppendProbe(probe::ModulesText(s.modules) + "\n");
    if (stack)
        AppendProbe(probe::StackText(frame) + "\n");
    if (vtable)
        AppendProbe(probe::VtableText() + "\n");
    if (modules && frame != 1)
        AppendProbe(probe::ModulesText(s.modules) + "\n");
    s.diagTicks += Qpc() - t0;
}

void OnFrameComplete(Core& core, FrameOutcome&& o, const ContextTable::Snapshot& contexts)
{
    if (!DiagnosticsOn() || !EnsureRun())
        return;
    auto& s = S();
    const int64_t t0 = Qpc();
    const uint64_t frame = o.record.frame;

    s.window.draws.Add(double(o.record.summary.draws));
    s.window.hookUs.Add(o.hookUs);
    s.window.presentDraws.Add(double(o.record.presentDraws));

    std::string why;
    if (dfmt::LogThisFrame(frame))
        why = frame <= 600 ? "first600" : "every100";
    else if (o.listWorthy && frame - s.lastAnomalyFrame >= 20 && s.anomalies < 400)
    {
        why = std::string("anomaly:") + o.anomaly;
        s.lastAnomalyFrame = frame;
        ++s.anomalies;
    }
    if (!why.empty())
    {
        o.record.reason = why;
        std::string text = dfmt::FrameRecordText(o.record);
        if (!s.framesBudget.truncated)
        {
            if (s.framesBudget.Take(text.size()))
                s.framesBuf += text;
            else
                s.framesBuf += dfmt::kTruncatedLine; // marker once, later records are dropped
        }
    }
    if (!s.framesBuf.empty() && (s.framesBuf.size() >= 32768 || frame % 300 == 0 || frame == 120))
    {
        Queue(Job::Kind::Append, "frames.txt", std::move(s.framesBuf));
        s.framesBuf.clear();
    }

    if (frame % 30 == 0)
        Publish(core, contexts);

    if (frame % 600 == 0)
    {
        s.windows.push_back(Fmt("frames %llu-%llu | draws mean=%.0f max=%.0f | hook_us mean=%.1f max=%.1f | present_draws mean=%.1f max=%.0f",
                                (unsigned long long) (frame - 599), (unsigned long long) frame, s.window.draws.Mean(),
                                s.window.draws.max, s.window.hookUs.Mean(), s.window.hookUs.max,
                                s.window.presentDraws.Mean(), s.window.presentDraws.max));
        if (s.windows.size() > 120)
            s.windows.erase(s.windows.begin());
        s.window = Window {};
    }

    const bool costNow = frame == 60 || frame == 300 || frame % 600 == 0;
    if (costNow)
    {
        Queue(Job::Kind::Rewrite, "cost.txt", CostText(core, contexts));
        Queue(Job::Kind::Rewrite, "rt.txt", RtText(core, contexts));
    }
    if (frame == 120 || frame == 600 || frame % 1800 == 0)
    {
        bool truncated = false;
        Queue(Job::Kind::Rewrite, "shaders.csv", ShadersText(core, truncated));
    }
    if (frame % 3600 == 0)
    {
        const Counters& c = core.Stats();
        LOG_INFO("HudLess diag: frame {} draws/frame {:.0f} hook {:.0f} us/frame; boundary R1 {} R2 {} same {} none {}; "
                 "shaders after hook {} pre-hook seen {}",
                 frame, core.DrawsPerFrame().Mean(), core.HookUsPerFrame().Mean(), c.framesR1, c.framesR2, c.framesSame,
                 c.framesNone, c.shadersCreatedAfterHook, core.PreHookShaders());
    }

    s.diagTicks += Qpc() - t0;
    s.diagUs.Add(double(s.diagTicks) * 1e6 / double(QpcFreq()));
    s.diagTicks = 0;
}
} // namespace ext::ffxiv::ui::diag
