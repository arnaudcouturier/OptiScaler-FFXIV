#include "pch.h"

#include "ext/ffxiv/ui/UiDiag.h"

#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/ffxiv/ui/UiConfig.h"
#include "ext/ffxiv/ui/UiDiagRecorder.h"
#include "ext/ffxiv/ui/UiProbe.h"
#include "ext/hdr/screenshot/Image.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

// The disk side of the D1 diagnostics: run folder, one writer thread, probe schedule, menu status. What is written and when
// is UiDiagRecorder.h (tested without a game).
namespace ext::ffxiv::ui::diag
{
namespace fs = std::filesystem;

namespace
{
constexpr uint64_t kImageBudget = 400ull << 20; // raw bytes of all snapshot images of one run (4 presses x 3 images at 4K)
constexpr size_t kMaxQueue = 512;

int64_t Qpc()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double TicksToUs(int64_t ticks)
{
    static const int64_t f = []
    {
        LARGE_INTEGER v {};
        QueryPerformanceFrequency(&v);
        return v.QuadPart > 0 ? v.QuadPart : 1;
    }();
    return double(ticks) * 1e6 / double(f);
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
struct State_;

class WriterSink final : public Sink
{
  public:
    explicit WriterSink(State_& s) : _s(s) {}
    void Append(const char* file, std::string text) override;
    void Rewrite(const char* file, std::string text) override;
    uint64_t Dropped() override;

  private:
    State_& _s;
};

struct State_
{
    std::mutex statusMutex;
    Status status;

    Writer writer;
    WriterSink sink { *this };
    std::unique_ptr<Recorder> recorder;
    bool ready = false, failed = false;
    fs::path dir;
    std::string runId;
    uint64_t imageBytes = 0;
    std::vector<probe::ModuleInfo> modules;
    int64_t entryTicks = 0; // probe work of this frame, charged to the diagnostics cost line
};

State_& S()
{
    static auto* s = new State_; // never destroyed
    return *s;
}

void Queue(State_& s, Job::Kind kind, const char* file, std::string text)
{
    Job j;
    j.kind = kind;
    j.path = s.dir / file;
    j.text = std::move(text);
    s.writer.Enqueue(std::move(j));
}

void WriterSink::Append(const char* file, std::string text) { Queue(_s, Job::Kind::Append, file, std::move(text)); }
void WriterSink::Rewrite(const char* file, std::string text) { Queue(_s, Job::Kind::Rewrite, file, std::move(text)); }
uint64_t WriterSink::Dropped() { return _s.writer.Dropped(); }

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

double BenchArmedNs()
{
    // Upper bound for the disarmed hook cost: the inline Armed() test in a loop (the loop's own cost is included).
    volatile uint32_t sink = 0;
    constexpr int kIterations = 5'000'000;
    const int64_t t0 = Qpc();
    for (int i = 0; i < kIterations; ++i)
        sink = sink + (Armed() ? 1u : 0u);
    return TicksToUs(Qpc() - t0) * 1e3 / double(kIterations);
}

uintptr_t LatchedContext()
{
    return reinterpret_cast<uintptr_t>(ext::ffxiv::lighting::FfxivLightingCapture::context);
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
        const fs::path base = OptiScalerDir();
        if (base.empty())
        {
            s.failed = true;
            return false;
        }
        const fs::path root = base / "hudless-diag";
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

        Recorder::Options options;
        options.runId = s.runId;
        options.earlyOutNs = BenchArmedNs();
        options.latchedContext = &LatchedContext;
        s.recorder = std::make_unique<Recorder>(s.sink, std::move(options));
        s.recorder->Begin();
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

void Publish(Core& core)
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
    auto& s = S();
    if (s.ready)
        Queue(s, Job::Kind::Append, file, std::move(text));
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
    auto& s = S();
    const int64_t t0 = Qpc();
    const uint64_t frame = core.Frame();
    auto& rec = *s.recorder;
    const bool stack = frame <= 5 || frame == 600 || frame == 3600;
    const bool vtable = frame == 1 || frame == 600 || frame == 3600;
    const bool modules = frame == 1 || frame == 600 || frame == 3600 || frame == 7200;
    if (frame == 1)
        rec.AppendProbe("# S10 D1 probe.txt, format " + std::to_string(dfmt::kFormatVersion) + ", run " + s.runId +
                        "\n# Evidence lines are addresses and module paths; lines starting '# inference' are conclusions drawn from them.\n"
                        "# Probe frames: call chain at Present 1-5, 600, 3600; vtables at 1, 600, 3600; module list at 1 and the additions at 600, 3600, 7200.\n\n");
    if (modules && frame == 1)
        rec.AppendProbe(probe::ModulesText(s.modules) + "\n");
    if (stack)
        rec.AppendProbe(probe::StackText(frame) + "\n");
    if (vtable)
        rec.AppendProbe(probe::VtableText() + "\n");
    if (modules && frame != 1)
        rec.AppendProbe(probe::ModulesText(s.modules) + "\n");
    s.entryTicks += Qpc() - t0;
}

void OnFrameComplete(Core& core, FrameOutcome&& o, const ContextTable::Snapshot& contexts)
{
    if (!DiagnosticsOn() || !EnsureRun())
        return;
    auto& s = S();
    const int64_t t0 = Qpc();
    const uint64_t frame = o.record.frame;

    s.recorder->OnFrame(core, std::move(o), contexts);
    if (frame % 30 == 0)
        Publish(core);
    if (frame % 3600 == 0)
    {
        const Counters& c = core.Stats();
        LOG_INFO("HudLess diag: frame {} draws/frame {:.0f} hook {:.0f} us/frame; boundary R1 {} R2 {} same {} none {}; "
                 "shaders after hook {} pre-hook seen {}",
                 frame, core.DrawsPerFrame().Mean(), core.HookUsPerFrame().Mean(), c.framesR1, c.framesR2, c.framesSame,
                 c.framesNone, c.shadersCreatedAfterHook, core.PreHookShaders());
    }

    s.recorder->AddDiagUs(TicksToUs(s.entryTicks + (Qpc() - t0)));
    s.entryTicks = 0;
}
} // namespace ext::ffxiv::ui::diag
