#include "pch.h"

#include "ext/reshade/ReShadeProbeCore.h"

#include <algorithm>
#include <format>

namespace ext::reshade::probe
{
std::string Hex(uint64_t value) { return std::format("{:#x}", value); }

const char* ApiName(Api api)
{
    switch (api)
    {
    case Api::D3D11:
        return "D3D11";
    case Api::D3D12:
        return "D3D12";
    case Api::Other:
        return "other (not D3D11/12)";
    case Api::Unknown:
        break;
    }
    return "unknown";
}

const char* EventName(EventKind kind)
{
    switch (kind)
    {
    case EventKind::InitRuntime:
        return "init_effect_runtime";
    case EventKind::DestroyRuntime:
        return "destroy_effect_runtime";
    case EventKind::ReloadedEffects:
        return "reshade_reloaded_effects";
    case EventKind::PresetPath:
        return "reshade_set_current_preset_path";
    case EventKind::BeginEffects:
        return "reshade_begin_effects";
    case EventKind::FinishEffects:
        return "reshade_finish_effects";
    case EventKind::ReshadePresent:
        return "reshade_present";
    case EventKind::QueuePresent:
        return "present";
    case EventKind::Count:
        break;
    }
    return "?";
}

uint64_t EventCounts::Total() const
{
    uint64_t total = 0;
    for (uint64_t v : n)
        total += v;
    return total;
}

std::string FormatEventCounts(const EventCounts& counts)
{
    std::string out;
    for (size_t i = 0; i < kEventKinds; i++)
    {
        if (!out.empty())
            out += ", ";
        out += std::format("{}={}", EventName(static_cast<EventKind>(i)), counts.n[i]);
    }
    return out;
}

// ---- F1 text --------------------------------------------------------------------------------------------------------
std::vector<std::string> FormatRuntime(const RuntimeInfo& r, std::string_view heading)
{
    std::vector<std::string> lines;
    lines.push_back(std::format("[F1] {} runtime {}: api={} native(swapchain, inferred)={} device={} hwnd={}", heading,
                                Hex(r.id), ApiName(r.api), Hex(r.native), Hex(r.device), Hex(r.hwnd)));
    lines.push_back(std::format("[F1]   back buffers: {} x {}x{} (reshade format value {})", r.backBufferCount,
                                r.backBufferWidth, r.backBufferHeight, r.backBufferFormat));
    lines.push_back(std::format("[F1]   effects enabled: {}", r.effectsEnabled ? "true" : "false"));
    lines.push_back(std::format("[F1]   preset: \"{}\"", r.presetPath));

    if (!r.techniquesKnown)
    {
        lines.push_back("[F1]   techniques: not known yet (effects load after init_effect_runtime; "
                        "see the next reshade_reloaded_effects entry)");
        return lines;
    }

    size_t enabled = 0;
    for (const auto& t : r.techniques)
        enabled += t.enabled ? 1 : 0;
    lines.push_back(std::format("[F1]   techniques: {} total, {} enabled", r.techniques.size(), enabled));

    for (const auto& t : r.techniques)
    {
        if (t.enabled)
            lines.push_back(std::format("[F1]   enabled: {} ({})", t.name, t.effect));
    }

    std::string disabled;
    size_t disabledCount = 0;
    for (const auto& t : r.techniques)
    {
        if (t.enabled)
            continue;
        disabledCount++;
        if (disabled.size() < 3000)
        {
            if (!disabled.empty())
                disabled += ", ";
            disabled += t.name;
        }
    }
    if (disabledCount != 0)
    {
        size_t shown = std::count(disabled.begin(), disabled.end(), ',') + 1;
        lines.push_back(std::format("[F1]   disabled ({}{}): {}", disabledCount,
                                    shown < disabledCount ? std::format(", first {} listed", shown) : std::string(),
                                    disabled));
    }
    return lines;
}

// ---- F3 frame ledger ------------------------------------------------------------------------------------------------
FrameLedger::FrameLedger(int64_t qpcFrequency) : _freq(qpcFrequency > 0 ? qpcFrequency : 1) {}

FrameLedger::Slot* FrameLedger::Find(uint64_t runtime)
{
    for (auto& s : _slots)
    {
        if (s.used && s.runtime == runtime)
            return &s;
    }
    return nullptr;
}

const FrameLedger::Slot* FrameLedger::Find(uint64_t runtime) const
{
    for (const auto& s : _slots)
    {
        if (s.used && s.runtime == runtime)
            return &s;
    }
    return nullptr;
}

FrameLedger::Slot& FrameLedger::FindOrCreate(uint64_t runtime)
{
    if (Slot* s = Find(runtime))
        return *s;
    for (auto& s : _slots)
    {
        if (!s.used)
        {
            s = Slot {};
            s.used = true;
            s.runtime = runtime;
            return s;
        }
    }
    // Table full (more runtimes than the game could plausibly have): recycle the last slot.
    _slots.back() = Slot {};
    _slots.back().used = true;
    _slots.back().runtime = runtime;
    return _slots.back();
}

void FrameLedger::OnEffects(const EffectsEvent& e)
{
    Slot& s = FindOrCreate(e.runtime);
    auto us = [this](int64_t from, int64_t to) { return to > from ? 1e6 * double(to - from) / double(_freq) : 0.0; };

    if (e.begin)
    {
        if (e.fromProbe)
        {
            s.open.probeBegin++;
            s.probeBeginQpc = e.qpc;
        }
        else
        {
            if (s.open.begin == 0)
            {
                s.open.rtv = e.rtv;
                s.open.rtvSrgb = e.rtvSrgb;
                s.open.width = e.width;
                s.open.height = e.height;
                s.open.format = e.format;
            }
            s.open.begin++;
            s.beginQpc = e.qpc;
        }
    }
    else if (e.fromProbe)
    {
        s.open.probeFinish++;
        s.open.probeSpanUs += us(s.probeBeginQpc, e.qpc);
    }
    else
    {
        s.open.finish++;
        s.open.spanUs += us(s.beginQpc, e.qpc);
    }
}

std::vector<std::string> FrameLedger::OnFrameEnd(uint64_t runtime)
{
    std::vector<std::string> lines;
    Slot& s = FindOrCreate(runtime);

    s.frames++;
    s.open.frame = s.frames;
    const FrameRecord f = s.open;
    s.last = f;
    s.open = FrameRecord {};

    // summary window
    s.windowFrames++;
    if (f.begin == 0)
        s.zeroPass++;
    else if (f.begin == 1)
        s.onePass++;
    else
        s.multiPass++;
    if (f.begin != 0)
    {
        s.spanMin = s.spanCount == 0 ? f.spanUs : std::min(s.spanMin, f.spanUs);
        s.spanMax = std::max(s.spanMax, f.spanUs);
        s.spanSum += f.spanUs;
        s.spanCount++;
    }

    if (s.frames <= kDetailFrames)
    {
        lines.push_back(std::format(
            "[F3] runtime {} frame {}: end-of-frame passes begin={} finish={} span={:.0f}us | in-probe begin={} "
            "finish={} | rtv={} srgb={} {}x{} fmt={}",
            Hex(runtime), f.frame, f.begin, f.finish, f.spanUs, f.probeBegin, f.probeFinish, Hex(f.rtv), Hex(f.rtvSrgb),
            f.width, f.height, f.format));
    }
    else if (s.windowFrames >= kSummaryEvery)
    {
        lines.push_back(std::format("[F3] runtime {} summary after frame {}: last {} frames: 0 passes={} 1 pass={} "
                                    "2+ passes={}; pass CPU span us min/avg/max={:.0f}/{:.0f}/{:.0f}",
                                    Hex(runtime), f.frame, s.windowFrames, s.zeroPass, s.onePass, s.multiPass,
                                    s.spanMin, s.spanCount ? s.spanSum / double(s.spanCount) : 0.0, s.spanMax));
        s.windowFrames = s.zeroPass = s.onePass = s.multiPass = 0;
        s.spanMin = s.spanMax = s.spanSum = 0;
        s.spanCount = 0;
    }
    return lines;
}

FrameRecord FrameLedger::Open(uint64_t runtime) const
{
    const Slot* s = Find(runtime);
    return s ? s->open : FrameRecord {};
}

FrameRecord FrameLedger::LastClosed(uint64_t runtime) const
{
    const Slot* s = Find(runtime);
    return s ? s->last : FrameRecord {};
}

uint64_t FrameLedger::Frames(uint64_t runtime) const
{
    const Slot* s = Find(runtime);
    return s ? s->frames : 0;
}

void FrameLedger::Forget(uint64_t runtime)
{
    if (Slot* s = Find(runtime))
        *s = Slot {};
}

// ---- run folders ----------------------------------------------------------------------------------------------------
std::string RunFolderName(int year, int month, int day, int hour, int minute, int second)
{
    return std::format("reshade-{:04}{:02}{:02}-{:02}{:02}{:02}", year, month, day, hour, minute, second);
}

bool IsRunFolderName(std::string_view name)
{
    // reshade-YYYYMMDD-HHMMSS
    if (name.size() != 23 || name.substr(0, 8) != "reshade-" || name[16] != '-')
        return false;
    for (size_t i = 8; i < name.size(); i++)
    {
        if (i == 16)
            continue;
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return true;
}

std::vector<std::string> RunsToPrune(std::vector<std::string> names, size_t keep)
{
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !IsRunFolderName(n); }),
                names.end());
    std::sort(names.begin(), names.end()); // fixed-width timestamps: lexical order is chronological
    std::vector<std::string> out;
    if (names.size() > keep)
        out.assign(names.begin(), names.begin() + (names.size() - keep));
    return out;
}
} // namespace ext::reshade::probe
