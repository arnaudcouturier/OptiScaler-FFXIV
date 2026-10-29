#include "pch.h"

#include "ext/hudless/PlateProbe.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/hudless/HudlessAccess.h"
#include "ext/hudless/HudlessConfig.h"
#include "ext/hudless/PlateAssocCounters.h"
#include "ext/hudless/PlateProbeAccounting.h"
#include "ext/hudless/PlateProbeFormat.h"
#include "ext/hudless/PlateProbeRing.h"

#include <State.h>

#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

// The output-stream probe: the game thread pushes a Post and an Options event per real frame, the pacer threads push an Output event per
// output present, all into one lock-free ring. A writer thread, started on first use, drains the ring about once a second, puts the events in
// QPC order and runs the accounting, appends outputs.csv and logs the summary line every 10 seconds. The seam bodies only read the clock and
// push; nothing here changes a draw, a state, a resource or the order of anything, and nothing waits on the GPU.
namespace ext::hudless::plate
{
namespace fs = std::filesystem;

namespace
{
constexpr size_t kRingEvents = 8192;
constexpr int64_t kHoldBackMs = 100;
constexpr auto kFlushPeriod = std::chrono::milliseconds(1000);
constexpr int64_t kSummaryEveryMs = 10000;

uint64_t Qpc()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

double TicksPerMs()
{
    static const double f = []
    {
        LARGE_INTEGER v {};
        QueryPerformanceFrequency(&v);
        return v.QuadPart > 0 ? double(v.QuadPart) / 1000.0 : 1.0;
    }();
    return f;
}

struct Job
{
    bool rewrite = false;
    std::string name;
    std::string text;
};

struct Probe
{
    EventRing<kRingEvents> ring;
    std::atomic<uint64_t> lastPostSeq { 0 };
    uint64_t seq = 0;      // game thread only
    bool wasActive = false; // game thread only

    std::once_flag started;
    std::mutex jobsMutex;
    std::deque<Job> jobs;
    std::atomic<bool> failed { false };

    // flush thread only
    fs::path dir;
    bool ready = false;
    Accounting accounting;
    EventMerger merger { static_cast<uint64_t>(double(kHoldBackMs) * TicksPerMs()) };
    ByteBudget outputsBudget { kOutputsBytes };
    uint64_t qpcZero = 0;
    bool outputsHeaderWritten = false;
};

Probe& P()
{
    static Probe* probe = new Probe; // never destroyed: no destructor may run under the loader lock
    return *probe;
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

void Fail(const std::string& why)
{
    Probe& p = P();
    if (p.failed.exchange(true))
        return;
    LOG_WARN("Plate probe: {}; the probes are switched off for this session", why);
    ext::SetActive(ext::F::PlateProbe, false);
}

bool EnsureRun(Probe& p)
{
    if (p.ready)
        return true;
    try
    {
        const fs::path base = OptiScalerDir();
        if (base.empty())
        {
            Fail("the OptiScaler folder could not be found");
            return false;
        }
        const fs::path root = base / "plate-probe";
        std::tm local {};
        const std::time_t now = std::time(nullptr);
        if (localtime_s(&local, &now) != 0)
        {
            Fail("the local time could not be read");
            return false;
        }
        const std::string id = FormatRunId(local);
        p.dir = root / id;
        std::error_code ec;
        fs::create_directories(p.dir, ec);
        if (ec || !fs::is_directory(p.dir, ec))
        {
            Fail("cannot create " + p.dir.string() + ": " + ec.message());
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
            for (const auto& old : RunsToPrune(names))
            {
                if (old == id)
                    continue;
                std::error_code rm;
                fs::remove_all(root / old, rm);
            }
        p.ready = true;
        LOG_INFO("Plate probe: run folder {}", p.dir.string());
        return true;
    }
    catch (...)
    {
        Fail("the run folder could not be set up");
        return false;
    }
}

bool WriteFile(const fs::path& path, const std::string& text, bool append)
{
    std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!file)
        return false;
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    return file.good();
}

void RunJobs(Probe& p)
{
    std::deque<Job> work;
    {
        std::lock_guard lock(p.jobsMutex);
        work.swap(p.jobs);
    }
    for (const Job& j : work)
    {
        if (p.failed.load() || !EnsureRun(p))
            return;
        bool ok = true;
        if (j.rewrite)
        {
            fs::path temp = p.dir / j.name;
            temp += ".tmp";
            ok = WriteFile(temp, j.text, false);
            if (ok)
            {
                std::error_code ec;
                fs::rename(temp, p.dir / j.name, ec);
                ok = !ec;
            }
        }
        else
            ok = WriteFile(p.dir / j.name, j.text, true);
        if (!ok)
        {
            Fail("a write to " + (p.dir / j.name).string() + " failed");
            return;
        }
    }
}

void Process(Probe& p, const std::vector<Event>& ordered, std::string& rows)
{
    for (const Event& e : ordered)
    {
        if (p.qpcZero == 0 && e.type == EventType::Post)
            p.qpcZero = e.qpc;
        OutputRecord record;
        if (p.accounting.Process(e, record))
            rows += OutputsRow(record, p.qpcZero ? p.qpcZero : record.qpc, TicksPerMs());
    }
}

void Flush(Probe& p, bool all)
{
    std::vector<Event> chunk(1024);
    for (;;)
    {
        const size_t n = p.ring.Drain(chunk.data(), chunk.size());
        for (size_t i = 0; i < n; ++i)
            p.merger.Push(chunk[i]);
        if (n < chunk.size())
            break;
    }
    std::vector<Event> ordered;
    if (all)
        p.merger.ReleaseAll(ordered);
    else
        p.merger.Release(Qpc(), ordered);

    std::string rows;
    Process(p, ordered, rows);
    if (!rows.empty() && !p.outputsBudget.truncated)
    {
        std::string text;
        if (!p.outputsHeaderWritten)
        {
            text = OutputsHeaderText();
            p.outputsHeaderWritten = true;
        }
        text += rows;
        // past the cap the file gets its marker line once and the accounting (and the summary line) carry on
        const bool fits = p.outputsBudget.Take(text.size());
        std::lock_guard lock(p.jobsMutex);
        p.jobs.push_back({ false, "outputs.csv", fits ? std::move(text) : std::string(kTruncatedLine) });
    }
    RunJobs(p);
}

void Loop()
{
    Probe& p = P();
    int64_t lastSummary = static_cast<int64_t>(GetTickCount64());
    while (!p.failed.load())
    {
        std::this_thread::sleep_for(kFlushPeriod);
        const bool shutting = State::Instance().isShuttingDown;
        try
        {
            Flush(p, shutting);
        }
        catch (...)
        {
            Fail("an internal error stopped the writer");
            return;
        }
        const int64_t now = static_cast<int64_t>(GetTickCount64());
        if (shutting || now - lastSummary >= kSummaryEveryMs)
        {
            lastSummary = now;
            const Summary& s = p.accounting.GetSummary();
            SummaryExtras x;
            x.assoc = Counters().Snapshot();
            // the draw probe's packet association counts too: with frame generation off there are no outputs and no real-frame events
            if (s.outputs || s.posts || x.assoc.Any())
            {
                x.ringDrops = p.ring.Drops();
                x.mergedLate = p.merger.Late();
                x.reordered = p.merger.Reordered();
                x.ticksPerMs = TicksPerMs();
                LOG_INFO("{}", SummaryLine(s, x));
                // the next line's "this interval" lag values start from zero (only after a line was printed: they belong to it)
                p.accounting.ResetInterval();
            }
        }
        if (shutting)
            return;
    }
}

void EnsureWriter()
{
    Probe& p = P();
    std::call_once(p.started, [] { std::thread(Loop).detach(); }); // a detached thread: it must not be joined under the loader lock
}

void Push(const Event& e)
{
    Probe& p = P();
    EnsureWriter();
    p.ring.Push(e);
}
} // namespace

bool OverlayWanted() { return Enabled() && ext::Cfg().hudless.plateOverlay.value_or_default(); }

// After a failure the writer loop has ended and nobody consumes the job queue: a config reload can set the feature bit again, and a job queued
// then would stay for good (0.5 MB per 1800 frames, no bound). So both entry points return first thing once the probe has failed.
void AppendFile(const char* name, std::string text)
{
    if (!Enabled())
        return;
    Probe& p = P();
    if (p.failed.load())
        return;
    EnsureWriter();
    std::lock_guard lock(p.jobsMutex);
    p.jobs.push_back({ false, name, std::move(text) });
}

void RewriteFile(const char* name, std::string text)
{
    if (!Enabled())
        return;
    Probe& p = P();
    if (p.failed.load())
        return;
    EnsureWriter();
    std::lock_guard lock(p.jobsMutex);
    p.jobs.push_back({ true, name, std::move(text) });
}
} // namespace ext::hudless::plate

// ---- the seam bodies (HudlessSeams.h) --------------------------------------------------------------------------------------------
namespace ext::hudless::detail
{
// bridge Present, after the interop copy: one real frame
void PlateProbePost(Dx11wDx12SC& sc)
{
    using namespace plate;
    const uint64_t now = Qpc();
    Probe& p = P();
    auto* fg = Access::Hudless::Fg(sc);
    const bool active = fg != nullptr && fg->IsActive() && !fg->IsPaused();
    if (active && !p.wasActive)
    {
        Event reset;
        reset.type = EventType::Reset;
        reset.qpc = now;
        Push(reset);
    }
    p.wasActive = active;

    Event e;
    e.type = EventType::Post;
    e.qpc = now;
    e.thread = GetCurrentThreadId();
    e.seq = ++p.seq;
    e.frame = fg != nullptr ? static_cast<uint64_t>(fg->FrameCount()) : 0;
    p.lastPostSeq.store(e.seq, std::memory_order_release);
    Push(e);
}

// end of AdjustDlssgOptions: outputs per real frame the frame generator was dispatched for (0 = unknown)
void PlateProbeExpected(uint32_t outputsPerFrame)
{
    using namespace plate;
    Probe& p = P();
    const uint64_t seq = p.lastPostSeq.load(std::memory_order_acquire);
    if (seq == 0)
        return;
    Event e;
    e.type = EventType::Options;
    e.qpc = Qpc();
    e.thread = GetCurrentThreadId();
    e.seq = seq;
    e.multiplier = outputsPerFrame;
    Push(e);
}

// the output hook: once per output present, on the presenting thread
void PlateProbeOutput(IDXGISwapChain* swapChain, ID3D12CommandQueue*)
{
    using namespace plate;
    Event e;
    e.type = EventType::Output;
    e.qpc = Qpc();
    e.thread = GetCurrentThreadId();
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain3;
    if (swapChain != nullptr && SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&chain3))))
        e.backBuffer = static_cast<int32_t>(chain3->GetCurrentBackBufferIndex());
    Push(e);
}
} // namespace ext::hudless::detail
