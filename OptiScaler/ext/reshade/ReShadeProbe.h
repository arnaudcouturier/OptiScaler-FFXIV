#pragma once
// The probe session (S10 package D0): receives the ReShade runtime events, keeps the runtime registry, the F3 frame
// ledger and the report, and (later in this file) runs the armed one-frame probe. Everything ReShade- or D3D-specific
// sits behind IBackend so the logic is tested with a fake (tests/ext/reshade). No ReShade or ImGui types here.

#include "ext/reshade/ReShadeProbeCore.h"
#include "ext/reshade/ReShadeReport.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ext::reshade::probe
{
struct IBackend
{
    virtual ~IBackend() = default;

    virtual int64_t Qpc() = 0;
    virtual int64_t QpcFrequency() = 0;

    // Fresh description of a live runtime (the id is an effect_runtime pointer the caller got from an event a moment
    // ago). `withTechniques` also lists the techniques (valid only after reshade_reloaded_effects). False = failed.
    virtual bool Describe(uint64_t runtime, bool withTechniques, RuntimeInfo& out) = 0;
};

// True on the thread that is inside the probe's own render_effects call: the begin/finish events it fires belong to the
// probe, not to ReShade's end-of-frame pass.
bool InProbeCall();
struct ProbeCallScope
{
    ProbeCallScope();
    ~ProbeCallScope();
    ProbeCallScope(const ProbeCallScope&) = delete;
    ProbeCallScope& operator=(const ProbeCallScope&) = delete;
};

// Snapshot for the menu card.
struct SessionStatus
{
    EventCounts counts;
    uint32_t aliveRuntimes = 0;
    uint64_t framesSeen = 0; // reshade_present events of all runtimes
    std::string reportFolder; // empty until the first line was written
    bool reportFailed = false;
};

class Session
{
  public:
    Session(IBackend& backend, IReport& report);

    // First line of the report: why we are here (registration result, version, options). Safe to call once.
    void Begin(const std::string& registrationLine);

    // Event entry points. Any thread, never throw.
    void OnInitRuntime(uint64_t runtime);
    void OnDestroyRuntime(uint64_t runtime);
    void OnReloadedEffects(uint64_t runtime);
    void OnPresetPath(uint64_t runtime, const char* path);
    void OnEffects(EffectsEvent event);
    void OnReshadePresent(uint64_t runtime);
    void OnQueuePresent();

    // Number of runtimes currently alive / description of one (copy). Used by the probe and the menu.
    uint32_t AliveRuntimes() const;
    bool Runtime(uint64_t runtime, RuntimeInfo& out) const;

    EventCounts Counts() const;
    void Note(const std::string& line); // free-form report line (thread-safe)

    SessionStatus Status(const std::string& reportFolder, bool reportFailed) const;

  private:
    struct Entry
    {
        RuntimeInfo info;
        bool alive = false;
        uint32_t generation = 0; // how many times this pointer value was initialised (ResizeBuffers reuses it)
    };
    static constexpr size_t kRuntimes = 8;

    Entry* FindLocked(uint64_t runtime);
    const Entry* FindLocked(uint64_t runtime) const;
    Entry& FindOrCreateLocked(uint64_t runtime);
    void WriteLines(const std::vector<std::string>& lines);

    IBackend& _backend;
    IReport& _report;
    mutable std::mutex _mutex;
    std::array<Entry, kRuntimes> _runtimes {};
    FrameLedger _ledger;
    std::array<std::atomic<uint64_t>, kEventKinds> _counts {};
    std::atomic<uint64_t> _framesSeen { 0 };
};
} // namespace ext::reshade::probe
