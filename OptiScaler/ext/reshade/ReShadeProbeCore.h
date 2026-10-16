#pragma once
// Pure logic of the ReShade probe (S10 package D0): runtime description and its report text, the per-frame effect-pass
// ledger (F3 passive counters), event counters, and the run-folder naming/pruning rules. No ReShade, D3D or ImGui
// types, no globals: everything here is unit-tested without ReShade (tests/ext/reshade).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ext::reshade::probe
{
// ---- runtime description (F1) ---------------------------------------------------------------------------------------
enum class Api : uint8_t
{
    Unknown,
    D3D11,
    D3D12,
    Other
};
const char* ApiName(Api api);

struct TechniqueInfo
{
    std::string name;
    std::string effect;
    bool enabled = false;
};

struct RuntimeInfo
{
    uint64_t id = 0;     // the effect_runtime pointer
    Api api = Api::Unknown;
    uint64_t native = 0; // effect_runtime::get_native() (a swapchain by the header docs: inference)
    uint64_t device = 0; // the device's native pointer
    uint64_t hwnd = 0;
    uint32_t backBufferCount = 0;
    uint32_t backBufferWidth = 0;
    uint32_t backBufferHeight = 0;
    uint32_t backBufferFormat = 0; // reshade::api::format value (raw)
    bool effectsEnabled = false;
    std::string presetPath;
    bool techniquesKnown = false; // false until reshade_reloaded_effects (init_effect_runtime fires before effects load)
    std::vector<TechniqueInfo> techniques;
};

// "[F1] ..." lines for one runtime: pointers, device API, window, back buffers, effects state, preset path, techniques.
std::vector<std::string> FormatRuntime(const RuntimeInfo& runtime, std::string_view heading);

// ---- events ---------------------------------------------------------------------------------------------------------
enum class EventKind : uint8_t
{
    InitRuntime,
    DestroyRuntime,
    ReloadedEffects,
    PresetPath,
    BeginEffects,
    FinishEffects,
    ReshadePresent,
    QueuePresent,
    Count
};
inline constexpr size_t kEventKinds = static_cast<size_t>(EventKind::Count);
const char* EventName(EventKind kind);

struct EventCounts
{
    std::array<uint64_t, kEventKinds> n {};
    uint64_t Total() const;
};

// One reshade_begin_effects / reshade_finish_effects delivery.
struct EffectsEvent
{
    bool begin = true;     // false = finish
    uint64_t runtime = 0;
    uint64_t cmdList = 0;
    uint64_t rtv = 0;
    uint64_t rtvSrgb = 0;
    uint32_t width = 0;    // size / format of the render target the view points to (0 = could not be resolved)
    uint32_t height = 0;
    uint32_t format = 0;
    int64_t qpc = 0;
    bool fromProbe = false; // fired inside our own render_effects call
};

// Effect passes of one frame (frame = reshade_present to reshade_present of a runtime).
struct FrameRecord
{
    uint64_t frame = 0;
    uint32_t begin = 0;      // end-of-frame passes (not ours)
    uint32_t finish = 0;
    uint32_t probeBegin = 0; // passes fired inside the probe's render_effects call
    uint32_t probeFinish = 0;
    double spanUs = 0;       // CPU time between begin and finish of the end-of-frame pass(es)
    double probeSpanUs = 0;
    uint64_t rtv = 0;        // first end-of-frame pass
    uint64_t rtvSrgb = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
};

class FrameLedger
{
  public:
    static constexpr uint64_t kDetailFrames = 120; // every frame is logged for the first N frames of a runtime
    static constexpr uint64_t kSummaryEvery = 600; // then one summary line every N frames

    explicit FrameLedger(int64_t qpcFrequency);

    void OnEffects(const EffectsEvent& event);

    // reshade_present of `runtime`: closes its frame and returns the lines to write (possibly none).
    std::vector<std::string> OnFrameEnd(uint64_t runtime);

    FrameRecord Open(uint64_t runtime) const;       // passes seen so far in the current frame
    FrameRecord LastClosed(uint64_t runtime) const; // the frame that ended last
    uint64_t Frames(uint64_t runtime) const;
    void Forget(uint64_t runtime);                  // runtime destroyed

  private:
    struct Slot
    {
        uint64_t runtime = 0;
        bool used = false;
        FrameRecord open;
        FrameRecord last;
        int64_t beginQpc = 0;
        int64_t probeBeginQpc = 0;
        uint64_t frames = 0;
        // summary window
        uint64_t windowFrames = 0;
        uint64_t zeroPass = 0;
        uint64_t onePass = 0;
        uint64_t multiPass = 0;
        double spanMin = 0;
        double spanMax = 0;
        double spanSum = 0;
        uint64_t spanCount = 0;
    };
    static constexpr size_t kSlots = 8;

    Slot* Find(uint64_t runtime);
    const Slot* Find(uint64_t runtime) const;
    Slot& FindOrCreate(uint64_t runtime);

    int64_t _freq;
    std::array<Slot, kSlots> _slots {};
};

// ---- run folder -----------------------------------------------------------------------------------------------------
// "reshade-YYYYMMDD-HHMMSS": the only folder names this module creates and prunes (package A's diagnostics use bare
// timestamps in the same parent folder and must not touch these).
std::string RunFolderName(int year, int month, int day, int hour, int minute, int second);
bool IsRunFolderName(std::string_view name);
// Names (valid run folders only) to delete so that at most `keep` remain, oldest first.
std::vector<std::string> RunsToPrune(std::vector<std::string> names, size_t keep);

// ---- misc text ------------------------------------------------------------------------------------------------------
std::string Hex(uint64_t value); // "0x1562b089470"
// One line: counters of every event kind, e.g. for the report and the menu.
std::string FormatEventCounts(const EventCounts& counts);
} // namespace ext::reshade::probe
