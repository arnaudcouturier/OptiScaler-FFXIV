#pragma once
// Bind-time mipmap bias: the control logic between the game's events and the engine (MipBindEngine.h), without Config, State,
// logging or Detours, so tests drive it with a real WARP device and a log collector. MipBindHooks.cpp feeds it the settings and
// the sizes it reads from the game and prints what it logs.
//
//   OnSamplerCreated  any thread, from the hook on CreateSamplerState
//   OnEvaluate        the immediate-context timeline, from the evaluate seam (every DX11 NGX evaluate)
//   InstallPlan       which hooks to attach when (device seam, first upscaler create, menu), and what happened

#include "ext/upscaler/MipBindEngine.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace ext::upscaler::mipbind
{
enum class LogLevel
{
    Info,
    Warn,
};
using LogSink = void (*)(LogLevel, const std::string&);

// What the glue reads from the config for one evaluate.
struct Settings
{
    bool autoEnabled = false;     // [Mipmap] MipmapBiasAuto / AutomaticFFXIV / the game default
    bool manualOverride = false;  // [Mipmap] MipmapBiasOverride has a value: it always wins
    double offset = 0.0;          // [Mipmap] MipmapBiasAutoOffset
    double outputScaling = 1.0;   // the multiplier the upscaler really applies
    bool observe = false;         // [Mipmap] MipmapBiasObserve
};

struct EvalInput
{
    unsigned renderW = 0;
    unsigned outputW = 0;
    std::uint64_t nowMs = 0;
    // The evaluate's context is not the one the hooks serve, but is an immediate context of the hooked class: move to it.
    ID3D11DeviceContext* relatchTo = nullptr;
};

class Controller
{
  public:
    Controller(Engine& engine, LogSink sink) : _engine(engine), _sink(sink) {}

    // The game made a sampler (`game` is what it asked for, as seen above upstream's own hook). Samplers the fork made
    // itself (`bypassed`) never take part but still refresh the table, so an address a released game sampler had cannot keep
    // its entry. A hand-set MipmapBiasOverride does NOT stop the classification: the stored bias is the game's own, and the
    // substitution resumes at once when the override is cleared.
    SamplerTable::Result OnSamplerCreated(ID3D11SamplerState* sampler, ID3D11Device* device, const D3D11_SAMPLER_DESC& game,
                                          bool bypassed)
    {
        D3D11_SAMPLER_DESC created = game;
        sampler->GetDesc(&created);
        const bool eligible = !bypassed && IsEligible(game.MipLODBias, game.MinLOD, game.MaxLOD, static_cast<unsigned>(game.Filter));
        const auto result = _engine.NoteCreated(sampler, device, game, created, eligible);

        if (result == SamplerTable::Result::Full)
        {
            ++_engine.counters().tableFull;
            if (!_fullLogged.exchange(true))
                Log(LogLevel::Warn, std::format("Bind-time mip bias: more than {} eligible samplers; the rest keep the game's "
                                                "own bias",
                                                kTableCapacity));
        }
        else if (eligible && result != SamplerTable::Result::Ignored && result != SamplerTable::Result::Invalidated)
        {
            ++_engine.counters().eligibleCreated;
        }
        return result;
    }

    // One DX11 evaluate. Everything except the sampler creates happens here, on the context's own thread.
    void OnEvaluate(const Settings& settings, const EvalInput& in)
    {
        if (in.relatchTo != nullptr && in.relatchTo != _engine.Latched())
        {
            Log(LogLevel::Warn, std::format("Bind-time mip bias: the hooks were serving context {:#x}, the evaluate context {:#x} "
                                            "is of the same kind and is the one the game uses; moving the hooks to it",
                                            reinterpret_cast<std::uintptr_t>(_engine.Latched()),
                                            reinterpret_cast<std::uintptr_t>(in.relatchTo)));
            _engine.Relatch(in.relatchTo);
            _watch.Reset();
        }

        // What the game created since the last evaluate: the add/replace detector and the observe log learn from it whether or
        // not the substitution runs, so a hand-set value that is cleared later finds the mode already known.
        const Mode before = _detector.mode();
        _engine.DrainNewEntries(
            [&](const SamplerTable::Entry& e)
            {
                const float original = e.original.load(std::memory_order_relaxed);
                const double base = e.baseCreate.load(std::memory_order_relaxed);
                if (settings.observe)
                    if (const auto line = _observer.Note(base, original))
                        Log(LogLevel::Info, *line);

                const Decision d = _detector.OnObserved(e.family, original, base);
                if (d.any())
                    Log(LogLevel::Info, DecisionLine(d));
            });
        const bool modeMoved = _detector.mode() != before;

        // A manual value always wins; so does switching the setting off.
        const bool run = BindShouldRun(settings.autoEnabled, settings.manualOverride);
        if (!run)
        {
            if (_engine.Active())
            {
                _engine.SetActive(false);
                Log(LogLevel::Info, std::format("Bind-time mip bias: off ({}); the game's own samplers are bound again",
                                                settings.manualOverride ? "a manual Mipmap Bias value is set" : "the setting is off"));
            }

            // The setting is on but a hand-set value blocks it: say so once, otherwise nothing in the log tells why nothing
            // happens. The automatic bias takes over again as soon as the value is cleared.
            if (settings.autoEnabled && settings.manualOverride && !_manualLogged)
            {
                _manualLogged = true;
                Log(LogLevel::Warn,
                    "Bind-time mip bias: not applied because MipmapBiasOverride is set by hand (it always wins); set "
                    "MipmapBiasOverride=auto in the ini or press Reset in the Mipmap Bias card: the automatic bias then "
                    "resumes at once, without a restart");
            }
            else if (!settings.manualOverride)
            {
                _manualLogged = false;
            }
            return;
        }
        _manualLogged = false;

        const auto base = BaseFromEvaluate(in.renderW, in.outputW, settings.outputScaling);
        if (!base)
            return; // not a game scene: the previous target stays

        const GateResult gate = _gate.OnEvaluate(in.outputW, *base, in.nowMs);
        if (gate.kind == GateResult::Kind::Ignored && !_engine.HasTarget())
            return;

        double useBase = _engine.HasTarget() ? _engine.Base() : *base;
        if (gate.kind == GateResult::Kind::Applied)
        {
            useBase = gate.base;
            _lastRender = in.renderW;
            _lastOutput = in.outputW;
        }

        bool changed = _engine.SetTarget(useBase, settings.offset, _detector.mode());
        if (changed)
        {
            Log(LogLevel::Info, TargetLine(_lastRender, _lastOutput, useBase, settings.offset, _engine.Target(), _detector.mode(),
                                           !_logged));
            _logged = true;
        }
        (void) modeMoved;

        if (!_engine.Active())
        {
            _engine.SetActive(true);
            changed = true;
        }

        if (changed)
            _engine.Refresh();
        else
            _engine.Service();

        if (_watch.OnEvaluate(_engine.Active(), _engine.table().Count(), _engine.SubstitutedBinds()))
            Log(LogLevel::Warn, std::format("Bind-time mip bias: {} evaluates with {} eligible samplers known and not one bind "
                                            "substituted; the hooks may sit on a context the game does not use",
                                            LatchWatch::kEvaluates, _engine.table().Count()));
    }

    BindStatus Status(bool installed, bool manualWins) const
    {
        BindStatus s;
        s.installed = installed;
        s.manualWins = manualWins;
        s.running = _engine.Active();
        s.hasTarget = _engine.HasTarget();
        s.target = _engine.Target();
        s.mode = _engine.CurrentMode();
        s.decided = _detector.final();
        s.substitutedBinds = _engine.SubstitutedBinds();
        s.lazy = _engine.counters().lazyClassified.load();
        s.notReady = _engine.counters().notReadyBinds.load();
        s.variants = _engine.counters().variantsCreated.load();
        s.tableFull = _engine.counters().tableFull.load();
        s.failures = _engine.counters().variantFailures.load();
        s.samplers = _engine.table().Count();
        return s;
    }

    const ModeDetector& detector() const { return _detector; }

  private:
    void Log(LogLevel level, const std::string& text)
    {
        if (_sink != nullptr)
            _sink(level, text);
    }

    Engine& _engine;
    LogSink _sink;
    ModeDetector _detector;
    Observer _observer;
    TargetGate _gate;
    LatchWatch _watch;
    std::atomic<bool> _fullLogged { false };
    bool _manualLogged = false;
    bool _logged = false;
    unsigned _lastRender = 0;
    unsigned _lastOutput = 0;
};

// Which hooks to attach when, and what happened. The six SetSamplers/GetSamplers pairs go in at the device seam (they share no
// function with upstream); the CreateSamplerState hook goes in at the first upscaler create, after upstream's own hook on that
// function, and comes out first (Detours transactions on one function must be undone in the reverse order). Nothing is tried
// twice after a failure: the older create-time module takes over.
class InstallPlan
{
  public:
    enum class Action
    {
        None,
        InstallBind,
        InstallCreate,
    };

    Action OnDeviceHooked(bool enabled)
    {
        _deviceKnown = true;
        return enabled ? NextBind() : Action::None;
    }

    Action OnSettingChanged(bool enabled) { return (_deviceKnown && enabled) ? NextBind() : Action::None; }

    Action OnFeatureCreate()
    {
        _featureSeen = true;
        return NextCreate();
    }

    // The result of an InstallBind; returns the follow-up (the create hook, when a feature was already created).
    Action BindResult(bool ok)
    {
        if (!ok)
        {
            _bindFailed = true;
            return Action::None;
        }
        _bindInstalled = true;
        return NextCreate();
    }

    void CreateResult(bool ok)
    {
        if (ok)
            _createHooked = true;
        else
            _createFailed = true;
    }

    // The create hook is detached first, then the bind hooks.
    void CreateRemoved() { _createHooked = false; }
    void BindRemoved()
    {
        _bindInstalled = false;
        _createHooked = false;
    }

    bool bindInstalled() const { return _bindInstalled; }
    bool bindFailed() const { return _bindFailed; }
    bool createHooked() const { return _createHooked; }
    bool createFailed() const { return _createFailed; }
    bool deviceKnown() const { return _deviceKnown; }
    // The older create-time module stands down only while the bind-time hooks are in place.
    bool owns() const { return _bindInstalled; }

  private:
    Action NextBind() const { return (!_bindInstalled && !_bindFailed) ? Action::InstallBind : Action::None; }
    Action NextCreate() const
    {
        return (_bindInstalled && _featureSeen && !_createHooked && !_createFailed) ? Action::InstallCreate : Action::None;
    }

    bool _deviceKnown = false;
    bool _featureSeen = false;
    bool _bindInstalled = false;
    bool _bindFailed = false;
    bool _createHooked = false;
    bool _createFailed = false;
};
} // namespace ext::upscaler::mipbind
