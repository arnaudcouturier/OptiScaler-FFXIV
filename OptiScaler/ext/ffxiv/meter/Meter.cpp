#include "pch.h"

#include "ext/ffxiv/meter/Meter.h"
#include "ext/ffxiv/chara/CharaDispatch.h"   // the character preview's replay marker: this module's own draws are not the game's
#include "ext/ffxiv/diag/StageDump.h"        // the seam this module listens at (stage-side header only: the entries are function pointers)
#include "ext/ffxiv/engine/EnginePass.h"     // OwnWork: this module's own draws are never re-classified
#include "ext/ffxiv/meter/MeterPass.h"
#include "ext/present/PresentSeams.h"        // the output-frame counter: a "rendered frame" is a present interval in which some effect was reached

#include <cstring>
#include <mutex>

// Effect meter: the runtime (Meter.h). Two entries called by the stage-dump seam on every frame: OnRan (a stage ran: count it, and on its sampled frame measure its
// picture) and OnReport (a stage declined or held: count it with its reason). Everything is under one mutex because the menu thread reads the published card
// lines; the draw thread holds it for microseconds. A frame that is not sampled pays the counting and nothing on the GPU (the schedule says no before the pass is
// touched), a stage that is off is never reached and costs nothing at all.
namespace ext::ffxiv::meter
{
namespace
{
// What the menu's card shows: the last finished window's tally and frames, and the latest sample.
struct Published
{
    bool windowed = false; // a window has closed since the start (before that the card reads the book as it stands)
    uint64_t frames = 0;
    Tally tally;
    Sample sample;
};

struct State
{
    std::mutex lock;
    Book book;
    Schedule schedule;
    Cadence cadence;
    Pass pass;
    Sample sample[kEffectCount];
    Published published[kEffectCount];
    float lastWhite = kFallbackWhite;
    uint32_t seed = 0;
    bool disabled = false; // the pass could not create its objects: the meter stands down for the session (counting goes on)
    // the meter's own GPU cost
    double gpuMsSum = 0.0, gpuMsMax = 0.0;
    uint32_t gpuMsCount = 0;
    uint32_t started = 0, failedStarts = 0;
    uint64_t loggedFailures[kEffectCount] {}; // a bit per distinct failure text seen for an effect (hash of the pointer's text, folded to 64)
};

State& S()
{
    static State state;
    return state;
}

// The meter's own draws and copies go through the same hooked context as the game's: if any hook that sees them were to report a stage (a decline of a replay, say), the
// report would land here again on the same thread while the lock is held. A thread that is inside the meter leaves a second entry at once.
thread_local bool t_inMeter = false;
struct ReentryGuard
{
    bool entered;
    ReentryGuard() : entered(!t_inMeter)
    {
        if (entered)
            t_inMeter = true;
    }
    ~ReentryGuard()
    {
        if (entered)
            t_inMeter = false;
    }
    ReentryGuard(const ReentryGuard&) = delete;
    ReentryGuard& operator=(const ReentryGuard&) = delete;
};

bool ArmedOf(StageId id)
{
    const auto armed = ext::ffxiv::diag::detail::g_stageArmed[size_t(id)];
    return armed != nullptr && armed();
}

uint32_t FailureBit(const char* text)
{
    uint32_t h = 2166136261u;
    for (const char* c = text; *c != '\0'; ++c)
        h = (h ^ uint8_t(*c)) * 16777619u;
    return h % 64u;
}

// Publishes the effect's sample (after a finished measurement) for the card.
void PublishSample(State& s, StageId id)
{
    s.published[uint32_t(id)].sample = s.sample[uint32_t(id)];
}

// The ~ten second lines and the summary block, written from whichever entry notices the clock first (no thread of its own).
void LogIfDue(State& s, uint64_t now)
{
    if (s.cadence.WindowDue(now))
    {
        const uint64_t windowMs = s.cadence.WindowMs(now);
        const uint64_t frames = s.book.WindowFrames();
        for (uint32_t i = 0; i < kEffectCount; ++i)
        {
            const StageId id = StageId(i);
            const Tally& window = s.book.Window(id);
            Published& p = s.published[i];
            p.windowed = true;
            p.frames = frames;
            p.tally = window;
            p.sample = s.sample[i];
            if (window.reached != 0)
                LOG_INFO("{}", FormatEffectLine(id, windowMs, frames, window, s.sample[i]));
        }
        s.book.CloseWindow();
        s.cadence.TookWindow(now);
    }
    if (s.cadence.SummaryDue(now))
    {
        const uint64_t frames = s.book.TotalFrames();
        const double gpuMean = s.gpuMsCount != 0 ? s.gpuMsSum / double(s.gpuMsCount) : 0.0;
        LOG_INFO("Effect visibility summary: {} rendered frames so far; the meter's own pass cost {:.3f} ms on the GPU on average over {} samples (largest {:.3f} ms), "
                 "one effect per sampled frame, about every {} s each",
                 frames, gpuMean, s.gpuMsCount, s.gpuMsMax, kSampleMs / 1000);
        for (uint32_t i = 0; i < kEffectCount; ++i)
        {
            const StageId id = StageId(i);
            const Tally total = s.book.Total(id);
            if (total.reached == 0 && (!InfoOf(id).metered || !ArmedOf(id)))
                continue;
            LOG_INFO("{}", FormatSummaryLine(id, frames, total, s.sample[i]));
        }
        s.cadence.TookSummary(now);
    }
}

void NoteFailure(State& s, StageId id, const char* failure)
{
    if (failure == nullptr)
        return;
    ++s.failedStarts;
    const uint64_t bit = 1ull << FailureBit(failure);
    if ((s.loggedFailures[uint32_t(id)] & bit) == 0)
    {
        s.loggedFailures[uint32_t(id)] |= bit;
        LOG_INFO("Effect meter: {} was not measured ({})", InfoOf(id).name, failure);
    }
    // A pass that cannot make its own objects will not make them next time: stand down (the counting goes on).
    if (std::strstr(failure, "could not be created") != nullptr && !s.disabled)
    {
        s.disabled = true;
        LOG_WARN("Effect meter: the measuring pass could not be set up ({}), so no visibility is measured this session; the frame counts go on", failure);
    }
}

void Collect(State& s, ID3D11DeviceContext* context)
{
    if (!s.pass.InFlight() || context == nullptr)
        return;
    if (!s.schedule.InFlight())
    {
        s.pass.Abandon(); // the schedule gave up on it (its result never landed in kMaxPendingMs)
        return;
    }
    Result result;
    bool got = false;
    {
        ext::ffxiv::chara::ReplayScope replay;
        ext::ffxiv::engine::OwnWork own;
        got = s.pass.Collect(context, result);
    }
    if (!s.pass.InFlight())
        s.schedule.Collected();
    if (!got)
        return;
    Sample& sample = s.sample[uint32_t(result.id)];
    sample.have = true;
    sample.haveRegion = result.totals.HaveRegion();
    sample.share = result.totals.Share();
    sample.mean = result.totals.Mean();
    sample.coverage = result.totals.Coverage();
    ++sample.count;
    if (result.haveGpuMs)
    {
        s.gpuMsSum += result.gpuMs;
        s.gpuMsMax = result.gpuMs > s.gpuMsMax ? result.gpuMs : s.gpuMsMax;
        ++s.gpuMsCount;
    }
    PublishSample(s, result.id);
}

void OnRan(StageId id, ID3D11DeviceContext* context, ID3D11ShaderResourceView* in, ID3D11ShaderResourceView* out, const ext::ffxiv::diag::StageWindow& window)
{
    if (uint32_t(id) >= kEffectCount)
        return;
    ReentryGuard reentry;
    if (!reentry.entered)
        return;
    State& s = S();
    std::lock_guard<std::mutex> guard(s.lock);
    const uint64_t now = GetTickCount64();
    const uint64_t token = ext::present::FrameCount();
    s.cadence.Tick(now);
    s.book.NoteRan(id, token);
    if (window.white > 0.0f)
        s.lastWhite = window.white;
    Collect(s, context);
    // The sampled frame: ONE effect per frame, ~3 s apart each. Everything below is behind the schedule's yes; an unsampled frame never reaches the pass.
    if (!s.disabled && InfoOf(id).metered && in != nullptr && out != nullptr && context != nullptr && s.schedule.Due(id, now, token))
    {
        Request request;
        request.in = in;
        request.out = out;
        request.id = id;
        request.x = window.x;
        request.y = window.y;
        request.width = window.width;
        request.height = window.height;
        request.white = window.white > 0.0f ? window.white : s.lastWhite;
        request.seed = ++s.seed;
        bool started = false;
        {
            ext::ffxiv::chara::ReplayScope replay;
            ext::ffxiv::engine::OwnWork own;
            started = s.pass.Start(context, request);
        }
        if (started)
        {
            s.schedule.Started(id, now, token);
            ++s.started;
        }
        else
        {
            s.schedule.Declined(id, now);
            NoteFailure(s, id, s.pass.Failure());
        }
    }
    LogIfDue(s, now);
}

void OnReport(StageId id, ext::ffxiv::diag::StageOutcome outcome, const char* reason)
{
    using ext::ffxiv::diag::StageOutcome;
    if (uint32_t(id) >= kEffectCount)
        return;
    ReentryGuard reentry;
    if (!reentry.entered)
        return;
    State& s = S();
    std::lock_guard<std::mutex> guard(s.lock);
    const uint64_t now = GetTickCount64();
    const uint64_t token = ext::present::FrameCount();
    s.cadence.Tick(now);
    switch (outcome)
    {
    case StageOutcome::Ran:
    case StageOutcome::Combined: // the shadow lift riding on the contact pass's combine is the lift running
        s.book.NoteRan(id, token);
        break;
    case StageOutcome::Declined:
        s.book.NoteSkip(id, token, reason != nullptr ? reason : "left alone");
        break;
    case StageOutcome::Held:
        s.book.NoteSkip(id, token, "held the last whole answer");
        break;
    case StageOutcome::None:
        break;
    }
    LogIfDue(s, now);
}

struct RegisterMeter
{
    RegisterMeter()
    {
        ext::ffxiv::diag::detail::g_meterRan = &OnRan;
        ext::ffxiv::diag::detail::g_meterReport = &OnReport;
    }
} g_registerMeter;
} // namespace

std::string CardLine(StageId id)
{
    if (uint32_t(id) >= kEffectCount)
        return std::string();
    State& s = S();
    const bool armed = ArmedOf(id);
    std::lock_guard<std::mutex> guard(s.lock);
    const Published& p = s.published[uint32_t(id)];
    if (p.windowed)
        return FormatCard(armed, p.frames, p.tally, p.sample);
    // no window has closed yet: the book as it stands
    return FormatCard(armed, s.book.TotalFrames(), s.book.Total(id), s.sample[uint32_t(id)]);
}
} // namespace ext::ffxiv::meter
