#include "pch.h"

#include "ext/ffxiv/chara/CharaProbe.h"

#include "ext/core/ExtConfig.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaEnhance.h"
#include "ext/ffxiv/chara/CharaFlood.h"
#include "ext/ffxiv/chara/CharaIdentify.h"
#include "ext/ffxiv/chara/CharaNr.h"
#include "ext/ffxiv/chara/CharaProbeChannel.h"
#include "ext/ffxiv/chara/CharaProbeCommands.h"
#include "ext/ffxiv/chara/CharaProbeDump.h"
#include "ext/ffxiv/chara/CharaProbeFiles.h"
#include "ext/ffxiv/chara/CharaProbeHold.h"
#include "ext/ffxiv/chara/CharaProbeHooks.h"
#include "ext/ffxiv/chara/CharaProbeRules.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTags.h"
#include "ext/ffxiv/ui/FfxivUiBoundary.h"
#include "ext/ffxiv/ui/UiDiagFormat.h"

#include <array>
#include <dxgi.h>
#include <atomic>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>

namespace ext::ffxiv::chara::probe
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint64_t kBackbufferCacheMs = 250; // the swapchain is asked at most this often
constexpr uint64_t kActivityMs = 250;        // the per-draw scan runs this long after the preview renderer was last seen
constexpr uint64_t kGaugeMs = 100;           // the derived-texture count shown in the menu is refreshed this often
constexpr uint32_t kMaxSwapLines = 64;       // consumer swap traces over the session
constexpr UINT kScanSlots = 8;               // pixel-shader resources compared per draw
constexpr uint64_t kTickMs = 100;            // dump pump and request handling cadence
constexpr uint64_t kExtraImageMs = 2000;     // a preview output other than entry 0 is dumped when a draw sampled it this recently
constexpr size_t kMaxExtraImages = 3;

struct BackbufferInfo
{
    bool valid = false;
    uintptr_t texture = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
};

// The last resource a draw was seen sampling for each preview output (table entries 0..7), kept alive for the dump.
struct Held
{
    ComPtr<ID3D11Resource> resource;
    uint64_t lastSampled = 0;
};

// One preview or preview-derived texture a draw sampled (pointer values only, no reference).
struct HitFact
{
    UINT slot = 0;
    uintptr_t texture = 0;
    int entry = -1;
    bool derived = false;
    int sourceEntry = -1;
    uint32_t hops = 0;
    DerivedKind kind = DerivedKind::Copy;
};

// Facts of the last draw that sampled the preview or a texture derived from it.
struct DrawFacts
{
    bool valid = false;
    uint64_t serial = 0; // OnDraw call number: equal to the serial of a swap means the same draw
    uint64_t tick = 0;
    DrawApi api = DrawApi::Draw;
    bool crcKnown = false;
    uint32_t crc = 0;
    uint32_t shaderSize = 0;
    uint32_t roles = 0;
    bool haveTarget = false;
    uintptr_t target = 0;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    uint32_t targetFormat = 0;
    int backbuffer = -1; // 1 the target is the swapchain buffer, 0 it is not, -1 not known
    int viewportWidth = 0;
    int viewportHeight = 0;
    size_t hitCount = 0;
    std::array<HitFact, 8> hits {};
};

struct StoredSwap
{
    bool valid = false;
    SwapRecord record;
    DrawFacts draw;       // the facts of the last sampler draw at the time of the report
    bool sameDraw = false; // that draw is the one that was swapped
    uint64_t tick = 0;
    uint64_t count = 0;
};

struct State
{
    bool on = false;    // PreviewProbe: log lines, sink, counters
    bool flood = false; // PreviewDebugFlood: the scan runs on every draw and copies are followed (silently), so the flood reaches copies
    std::array<Held, 8> held;
    DrawFacts lastDraw;
    std::array<StoredSwap, kSwapKindCount> lastSwap; // by SwapKind
    uint64_t drawSerial = 0;
    uint64_t lastTick = 0;
    uint64_t lastPoll = 0;
    std::optional<CommandChannel> channel; // chara-probe.cmd / chara-probe.out, while the probe is on
    DerivedSet derived;
    ResolveHookState resolveHook; // whether the hook layer's ResolveSubresource detour is wanted and attached
    DumpService dumpService;      // when a requested image dump is executed (at a point where the preview image is complete)
    bool outputFailureLogged = false; // the one log line about an answer file that cannot be written
    CaseLog<kCopyCaseCap> copyCases;
    CaseLog<kSamplerCaseCap> samplerCases;
    BackbufferInfo backbuffer;
    bool backbufferKnown = false; // backbuffer holds an answer (valid or not) taken at backbufferStamp
    uint64_t backbufferStamp = 0;
    uint64_t backbufferFrame = 0; // g_stats.frames when the answer was taken: a new frame asks again
    uint64_t lastGauge = 0;
    std::array<bool, kSwapKindCount> swapSeen {};
    std::array<uint32_t, kSwapKindCount> swapBurst {};
};

// Never destroyed: the process may unload this module under the loader lock, where releasing objects is not safe.
State& S()
{
    static auto* state = new State;
    return *state;
}

std::string FormatText(uint32_t format) { return std::format("{}({})", ext::ffxiv::ui::dfmt::FormatName(format), format); }

const char* UsageText(D3D11_USAGE usage)
{
    switch (usage)
    {
    case D3D11_USAGE_DEFAULT: return "default";
    case D3D11_USAGE_IMMUTABLE: return "immutable";
    case D3D11_USAGE_DYNAMIC: return "dynamic";
    case D3D11_USAGE_STAGING: return "staging";
    default: return "unknown";
    }
}

const char* YesNo(bool v) { return v ? "yes" : "no"; }

// A dump request from the menu thread (or a command): the draw thread starts it from Update. g_onFlag mirrors State::on for the menu.
std::atomic<bool> g_dumpPending { false };
std::atomic<bool> g_onFlag { false };

// The swapchain buffer, looked up at most every kBackbufferCacheMs and once per presented frame when the bridge reports frames
// (a swapchain with several buffers may hand out a different texture each frame). Only the pointer value and the size are kept.
const BackbufferInfo& Backbuffer(uint64_t now)
{
    State& s = S();
    const uint64_t frame = g_stats.frames.load(std::memory_order_relaxed);
    if (!s.backbufferKnown || frame != s.backbufferFrame || now < s.backbufferStamp || now - s.backbufferStamp >= kBackbufferCacheMs)
    {
        s.backbufferFrame = frame;
        BackbufferInfo info;
        info.valid = ext::ffxiv::ui::BackbufferIdentity(&info.texture, &info.width, &info.height, &info.format);
        s.backbuffer = info;
        s.backbufferKnown = true;
        s.backbufferStamp = now;
    }
    return s.backbuffer;
}

constexpr bool IsPreviewEntry(int entry) { return entry >= 0 && entry < kPreviewEntries; }

// Whether the table holds a valid preview output (entries 0..7): pointers only, no COM call.
bool AnyPreviewOutput(const table::Snapshot& snapshot)
{
    for (int i = 0; i < kPreviewEntries; ++i)
    {
        if (snapshot.entries[static_cast<size_t>(i)].Valid())
            return true;
    }
    return false;
}

// ---- Consumer swap trace (sink of SwapTrace) -----------------------------------------------------------------------------------

// Every swap is traced while the probe is on (state.txt shows the last one of each kind); only the log line is limited to once per
// burst of preview activity and kind, and to kMaxSwapLines in all.
bool ArmSwap(SwapKind)
{
    return S().on;
}

void ReportSwap(const SwapRecord& r)
{
    State& s = S();
    const size_t k = static_cast<size_t>(r.kind);
    if (k >= s.lastSwap.size())
        return;
    StoredSwap& stored = s.lastSwap[k];
    stored.valid = true;
    stored.record = r;
    stored.draw = s.lastDraw;
    stored.sameDraw = SameDraw(s.lastDraw.valid, s.lastDraw.serial, s.drawSerial);
    s.dumpService.NoteSwap(); // all the images of this frame exist now: a waiting dump request may start at the next draw
    stored.tick = GetTickCount64();
    ++stored.count;

    const uint32_t burst = identify::BurstNumber();
    if (g_stats.probeSwapLines.load(std::memory_order_relaxed) >= kMaxSwapLines || (s.swapSeen[k] && s.swapBurst[k] == burst))
        return;
    s.swapSeen[k] = true;
    s.swapBurst[k] = burst;
    ++g_stats.probeSwapLines;
    LOG_INFO("CharaView probe: consumer swap [{}]: t{} before {:#x} after-swap {:#x} after-draw {:#x} (replacement {:#x})",
             SwapKindName(r.kind), r.slot, r.before, r.afterSwap, r.afterDraw, r.replacement);
}

const SwapSink kSink { &ArmSwap, &ReportSwap };

// ---- The ResolveSubresource hook ------------------------------------------------------------------------------------------------
std::atomic<const ResolveHookControl*> g_resolveControl { nullptr };
std::atomic<bool> g_resolveIncomplete { false }; // mirror of ResolveHookState::Incomplete() for the menu thread

// Attaches the detour when a debug aid (the probe or the flood test) was switched on, removes it when both are off (ResolveHookState
// decides). Draw thread; costs one comparison per call when nothing changes. A failed attach is reported once per switch-on and only
// costs the resolve observation: the other hooks never depended on it.
void SyncResolveHook()
{
    State& s = S();
    const ResolveHookControl* control = g_resolveControl.load(std::memory_order_acquire);
    const ResolveHookState::Step step = s.resolveHook.Decide(s.on || s.flood);
    switch (step)
    {
    case ResolveHookState::Step::Attach:
    {
        const bool ok = control != nullptr && control->attach != nullptr && control->attach();
        s.resolveHook.AttachResult(ok);
        if (ok)
            LOG_INFO("CharaView probe: ResolveSubresource is observed (the detour has its own hook transaction)");
        else
            LOG_WARN("CharaView probe: copy tracking incomplete: resolve not hooked (copies made by ResolveSubresource are not followed; the other "
                     "hooks are not affected)");
        break;
    }
    case ResolveHookState::Step::Detach:
        if (control != nullptr && control->detach != nullptr)
            control->detach();
        s.resolveHook.Detached();
        break;
    case ResolveHookState::Step::None: break;
    }
    if (step != ResolveHookState::Step::None)
        g_resolveIncomplete.store(s.resolveHook.Incomplete(), std::memory_order_relaxed);
}

// ---- Switching ------------------------------------------------------------------------------------------------------------

// The derived textures, the cached backbuffer and the per-burst trace limits (what the `reset` command forgets too).
void ForgetDerived()
{
    State& s = S();
    s.derived.Clear();
    s.backbuffer = BackbufferInfo {};
    s.backbufferKnown = false;
    s.swapSeen = {};
    s.swapBurst = {};
    g_stats.probeDerivedLive.store(0, std::memory_order_relaxed);
}

// Everything the probe holds: the preview textures kept for the dump, a dump that is in flight and a request that is waiting.
void ReleaseHeld()
{
    State& s = S();
    for (Held& h : s.held)
        h = Held {};
    s.lastDraw = DrawFacts {};
    s.lastSwap = {};
    g_dumpPending.store(false, std::memory_order_relaxed);
    dump::Release();
}

void ForgetAll()
{
    ForgetDerived();
    // A fresh start: copy tracking back on and the case logs empty, so a second session in one game session is not silent because the first
    // one filled the caps, and a "copies off" from an earlier session does not silently stop the flood-only mode from following copies.
    State& s = S();
    s.derived.Reset();
    s.copyCases.Clear();
    s.samplerCases.Clear();
    ReleaseHeld();
}

void SwitchOff(const char* why)
{
    State& s = S();
    g_swapSink.store(nullptr, std::memory_order_relaxed);
    detail::g_live.store(s.flood, std::memory_order_relaxed);
    s.on = false;
    g_onFlag.store(false, std::memory_order_relaxed);
    s.channel.reset();
    LOG_INFO("CharaView probe: off ({}); copy cases logged {}, sampler cases logged {}, consumer swap traces {}, derived textures seen {}",
             why, g_stats.probeCopyLines.load(), g_stats.probeSamplerLines.load(), g_stats.probeSwapLines.load(),
             g_stats.probeDerivedAdded.load());
    ForgetAll();
}

// ---- Copies ---------------------------------------------------------------------------------------------------------------

struct CopyDetail
{
    CopyVia via = CopyVia::CopyResource;
    UINT destSub = 0;
    UINT x = 0, y = 0, z = 0;
    UINT sourceSub = 0;
    const D3D11_BOX* box = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

std::string DetailText(const CopyDetail& d)
{
    switch (d.via)
    {
    case CopyVia::CopyResource: return "CopyResource, whole resource";
    case CopyVia::Resolve:
        return std::format("ResolveSubresource, destination subresource {}, source subresource {}, format {}", d.destSub, d.sourceSub,
                           FormatText(static_cast<uint32_t>(d.format)));
    case CopyVia::CopyRegion:
    default:
        break;
    }
    std::string text = std::format("CopySubresourceRegion, destination subresource {} at ({},{},{}), source subresource {}, ", d.destSub,
                                   d.x, d.y, d.z, d.sourceSub);
    if (d.box != nullptr)
        text += std::format("source box left {} top {} front {} right {} bottom {} back {}", d.box->left, d.box->top, d.box->front,
                            d.box->right, d.box->bottom, d.box->back);
    else
        text += "whole source subresource";
    return text;
}

// The size and format of a 2D texture resource (false for a buffer or anything else).
bool TextureInfoOf(ID3D11Resource* resource, TextureInfo& out)
{
    ComPtr<ID3D11Texture2D> texture;
    if (resource == nullptr || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || texture == nullptr)
        return false;
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    out = { desc.Width, desc.Height, static_cast<uint32_t>(desc.Format) };
    return true;
}

// The live derived entry of a texture a draw samples. An entry that holds a reference to its texture is the texture; one without (a
// swapchain buffer) is checked against the texture's actual size and format, so a recycled address with another size does not match.
const Derived* FindDerived(State& s, ID3D11Resource* resource, uintptr_t texture, uint64_t now)
{
    const Derived* found = s.derived.Find(texture, now);
    if (found == nullptr || found->hold)
        return found;
    TextureInfo actual;
    if (!TextureInfoOf(resource, actual))
        return nullptr;
    return s.derived.Find(texture, now, &actual);
}

void HandleCopy(ID3D11Resource* dest, ID3D11Resource* source, const CopyDetail& detail)
{
    State& s = S();
    if ((!s.on && !s.flood) || dest == nullptr || source == nullptr || !table::Ready())
        return;

    const uint64_t now = GetTickCount64();
    const table::Snapshot& snap = table::Current(now);
    if (!snap.ready)
        return;

    // Pointer compares first: only a copy of a preview output or of a derived texture costs more.
    const uintptr_t src = reinterpret_cast<uintptr_t>(source);
    const uintptr_t dst = reinterpret_cast<uintptr_t>(dest);
    const int entry = table::IndexOfPointer(snap, src);
    const bool fromPreview = IsPreviewEntry(entry);
    Derived from {};
    if (!fromPreview)
    {
        const Derived* found = s.derived.Find(src, now);
        if (found == nullptr)
            return;
        from = *found; // the record may be overwritten by the next derive
    }

    ++g_stats.probeCopies;

    // A copy of the preview itself shows the renderer is active. A copy of a derived texture neither extends that nor keeps the
    // set alive: lineage dies out about kExpiryMs after the preview stops rendering, however often the game still touches the
    // derived texture. (The case is logged either way.)
    const uint64_t activity = detail::g_lastActivity.load(std::memory_order_relaxed);
    const bool recent = activity != 0 && now >= activity && now - activity <= kActivityMs;
    const bool follow = fromPreview || recent;
    if (fromPreview)
        NoteActivity(now);

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(dest->QueryInterface(IID_PPV_ARGS(&texture))) || texture == nullptr)
        return; // a buffer cannot hold the image
    D3D11_TEXTURE2D_DESC td {};
    texture->GetDesc(&td);

    const TextureInfo info { td.Width, td.Height, static_cast<uint32_t>(td.Format) };
    const bool destIsPreviewEntry = table::IndexOfPointer(snap, dst) >= 0;

    // Only a copy of the WHOLE image into a texture of its own is followed (WholeImageCopy): a region copied into an atlas, a mip or an
    // array slice would make the whole destination "preview-derived". Partial copies are logged below as "not followed".
    bool wholeCopy = false;
    {
        ComPtr<ID3D11Texture2D> sourceTexture;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&sourceTexture))) && sourceTexture != nullptr)
        {
            D3D11_TEXTURE2D_DESC sd {};
            sourceTexture->GetDesc(&sd);
            CopyFacts facts;
            facts.via = detail.via;
            facts.destSub = detail.destSub;
            facts.x = detail.x;
            facts.y = detail.y;
            facts.z = detail.z;
            facts.sourceSub = detail.sourceSub;
            facts.hasBox = detail.box != nullptr;
            if (detail.box != nullptr)
            {
                facts.boxLeft = detail.box->left;
                facts.boxTop = detail.box->top;
                facts.boxFront = detail.box->front;
                facts.boxRight = detail.box->right;
                facts.boxBottom = detail.box->bottom;
                facts.boxBack = detail.box->back;
            }
            facts.source = { sd.Width, sd.Height, static_cast<uint32_t>(sd.Format) };
            facts.dest = info;
            facts.resolveFormat = static_cast<uint32_t>(detail.format);
            wholeCopy = WholeImageCopy(facts);
        }
    }

    if (follow && wholeCopy)
    {
        // The derived entry keeps the destination texture alive (so its address cannot be reused by another texture while the entry lives),
        // except a swapchain buffer: a held buffer makes ResizeBuffers fail.
        const BackbufferInfo& bbForHold = Backbuffer(now);
        const std::shared_ptr<void> hold = HoldOf(dest, bbForHold.valid && bbForHold.texture == dst);
        const DeriveResult result = fromPreview ? s.derived.FromPreview(entry, src, dst, info, destIsPreviewEntry, DerivedKind::Copy, now, hold)
                                                : s.derived.FromDerived(src, dst, info, destIsPreviewEntry, DerivedKind::Copy, now, hold);
        if (!Accepted(result))
        {
            ++g_stats.probeCopiesRefused;
            return;
        }
        if (result == DeriveResult::Added || result == DeriveResult::Replaced)
            ++g_stats.probeDerivedAdded;
        if (!fromPreview)
            s.derived.Touch(src, now);
    }
    else
        ++g_stats.probeCopiesRefused; // the preview is not rendering: not followed, but worth a line

    if (!s.on)
        return; // only the flood test is on: the copy is followed, nothing is logged

    const int sourceEntry = fromPreview ? entry : from.sourceEntry;
    const uint32_t hops = fromPreview ? 1 : from.hops + 1;
    const uint64_t sourceTag = fromPreview ? static_cast<uint64_t>(entry) : 1000 + static_cast<uint64_t>(from.sourceEntry) * 16 + from.hops;
    const uint64_t key = CaseKey(sourceTag, dst, (static_cast<uint64_t>(td.Width) << 32) | td.Height,
                                 static_cast<uint64_t>(td.Format) | (static_cast<uint64_t>(detail.via) << 32) | (follow && wholeCopy ? 1ull << 40 : 0));
    if (!s.copyCases.Offer(key))
        return;

    // The line is about to be written: this is the only place that asks the swapchain for the backbuffer on the copy path.
    const BackbufferInfo& bb = Backbuffer(now);
    const bool isBackbuffer = bb.valid && bb.texture == dst;
    const bool renderTarget = (td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
    const bool shaderResource = (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    const bool screenClass = renderTarget && bb.valid && IsScreenSizeIntermediate(info, { bb.width, bb.height, bb.format }); // at least half of the backbuffer's area

    std::string what = fromPreview ? std::format("copy of preview entry {}", entry)
                                   : std::format("copy of derived texture {:#x} (entry {}, hops {})", src, from.sourceEntry, from.hops);
    std::string flag;
    if (isBackbuffer)
        flag = " <-- destination is the swapchain backbuffer";
    else if (screenClass)
        flag = " <-- destination is a screen-size render target (at least half of the backbuffer's area)";
    if (!(follow && wholeCopy))
        flag += follow ? " <-- partial copy, not followed (a region, another subresource, or another size or format than the source)"
                       : " <-- the preview was not rendering: not followed";
    ++g_stats.probeCopyLines;
    LOG_INFO("CharaView probe: {} -> texture {:#x} {}x{} format {} (backbuffer? {}, render target? {}, shader resource? {}, usage {}, "
             "screen-size render target? {}); preview entry {}, hops {}; via {}{}",
             what, dst, td.Width, td.Height, FormatText(static_cast<uint32_t>(td.Format)), YesNo(isBackbuffer), YesNo(renderTarget),
             YesNo(shaderResource), UsageText(td.Usage), YesNo(screenClass), sourceEntry, hops, DetailText(detail), flag);
    if (s.copyCases.Count() == kCopyCaseCap)
        LOG_INFO("CharaView probe: the limit of {} copy cases is reached; further new cases are only counted (send the command `reset`, or switch the probe off and on, to start new logs)", kCopyCaseCap);
}

// ---- Preview image dump and state.txt --------------------------------------------------------------------------------------------

std::string PointerText(uintptr_t pointer) { return std::format("{:#x}", pointer); }

std::string TextureText(uintptr_t pointer, uint32_t width, uint32_t height, const std::string& format)
{
    return std::format("{} {}x{} format {}", PointerText(pointer), width, height, format);
}

void AddDrawFacts(StateText& t, const std::string& prefix, const DrawFacts& f, uint64_t now)
{
    t.AddBool(prefix + ".valid", f.valid);
    if (!f.valid)
        return;
    t.Add(prefix + ".ageMs", now >= f.tick ? now - f.tick : 0);
    t.Add(prefix + ".api", DrawApiName(f.api));
    t.Add(prefix + ".pixelShaderCrc", f.crcKnown ? std::format("{:08X}", f.crc) : std::string("unknown (shader made before the hooks)"));
    t.Add(prefix + ".pixelShaderBytes", f.crcKnown ? std::to_string(f.shaderSize) : std::string("unknown"));
    t.Add(prefix + ".roles", std::format("{:#x}", f.roles));
    t.Add(prefix + ".renderTarget",
          f.haveTarget ? TextureText(f.target, f.targetWidth, f.targetHeight, FormatText(f.targetFormat)) : std::string("none"));
    t.Add(prefix + ".renderTargetIsBackbuffer", f.backbuffer < 0 ? "unknown" : (f.backbuffer ? "yes" : "no"));
    t.Add(prefix + ".viewport", std::format("{}x{}", f.viewportWidth, f.viewportHeight));
    std::string srvs;
    for (size_t n = 0; n < f.hitCount; ++n)
    {
        const HitFact& h = f.hits[n];
        if (!srvs.empty())
            srvs += "; ";
        srvs += std::format("t{} {} ({})", h.slot, PointerText(h.texture),
                            h.derived ? std::format("{} of entry {}, hops {}", h.kind == DerivedKind::Draw ? "draw" : "copy", h.sourceEntry, h.hops)
                                      : std::format("preview entry {}", h.entry));
    }
    t.Add(prefix + ".previewResources", srvs.empty() ? std::string("none") : srvs);
}

// The text of state.txt (also written alone by the `state` command). Draw thread.
std::string BuildState(uint64_t now)
{
    State& s = S();
    StateText t;
    t.Add("state.time", LogTimeNow());
    t.Add("state.tickMs", now);
    const std::filesystem::path optiscaler = dump::OptiScalerDirectory();
    t.Add("game.folder", optiscaler.empty() ? std::string("unknown") : Utf8(optiscaler.parent_path()));
    t.AddBool("probe.on", s.on);
    t.AddBool("probe.floodActive", s.flood);
    t.AddBool("probe.copyTracking", s.derived.CopiesEnabled());
    t.AddBool("probe.resolveHooked", s.resolveHook.Attached());
    if (s.resolveHook.Incomplete())
        t.Add("probe.copyTrackingNote", "copy tracking incomplete: resolve not hooked (copies made by ResolveSubresource are not followed)");

    const auto& cfg = ext::Cfg().ffxiv.chara;
    const auto onOff = [](bool v) { return v ? "on" : "off"; };
    t.Add("config.FFXIV.PreviewDLSS", onOff(cfg.previewDlss.value_or_default()));
    t.Add("config.FFXIV.PreviewAA", onOff(cfg.previewAA.value_or_default()));
    t.Add("config.FFXIV.PreviewUpscale", onOff(cfg.previewUpscale.value_or_default()));
    t.Add("config.FFXIV.PreviewGuideCapture", onOff(cfg.previewGuideCapture.value_or_default()));
    t.Add("config.FFXIV.PreviewDebugTint", onOff(cfg.previewDebugTint.value_or_default()));
    t.Add("config.HDR.PreviewNRTrace", onOff(cfg.previewNrTrace.value_or_default()));
    t.Add("config.FFXIV.PreviewProbe", onOff(cfg.previewProbe.value_or_default()));
    t.Add("config.FFXIV.PreviewDebugFlood", onOff(cfg.previewDebugFlood.value_or_default()));

    for (const auto& fact : ext::ffxiv::ui::BridgeFacts())
        t.Add(fact.first, fact.second);

    const auto count = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    t.Add("counters.frames", count(g_stats.frames));
    t.Add("counters.drawsClassified", count(g_stats.classified));
    t.Add("counters.passes", count(g_stats.passes));
    t.Add("counters.passesLive", count(g_stats.passesLive));
    t.Add("counters.finisherAccepted", count(g_stats.finisherAccepted));
    t.Add("counters.finisherRejected", count(g_stats.finisherRejected));
    t.Add("counters.equipShaderSeen", count(g_stats.equipShaderSeen));
    t.Add("counters.equipMatched", count(g_stats.equipMatched));
    t.Add("counters.aaDrawsProcessed", count(g_stats.aaProcessed));
    t.Add("counters.aaSkipped", count(g_stats.aaSkipped));
    t.Add("counters.deferredSeen", count(g_stats.deferredSeen));
    t.Add("counters.slotsAcquired", count(g_stats.slotsAcquired));
    t.Add("counters.consumerSwaps", count(g_stats.consumerSwaps));
    t.Add("counters.floodSwaps", count(g_stats.floodSwaps));
    t.Add("counters.tintApplications", count(g_stats.tintApplied));
    t.Add("counters.probeDrawsScanned", count(g_stats.probeScanned));
    t.Add("counters.probeSamplerDraws", count(g_stats.probeSamplerDraws));
    t.Add("counters.probeCopiesSeen", count(g_stats.probeCopies));
    t.Add("counters.probeCopiesNotFollowed", count(g_stats.probeCopiesRefused));
    t.Add("counters.probeDerivedAdded", count(g_stats.probeDerivedAdded));
    t.Add("counters.probeCopyLines", g_stats.probeCopyLines.load(std::memory_order_relaxed));
    t.Add("counters.probeSamplerLines", g_stats.probeSamplerLines.load(std::memory_order_relaxed));
    t.Add("counters.probeSwapLines", g_stats.probeSwapLines.load(std::memory_order_relaxed));

    const dlss::Counters dc = dlss::GetCounters();
    t.Add("dlss.activeSlots", dc.activeSlots);
    t.Add("dlss.evaluations", dc.evaluations);
    t.Add("dlss.fallbacks", dc.fallbacks);
    const char* note = dlss::StatusNote();
    t.Add("dlss.status", note != nullptr && note[0] != '\0' ? note : "no note");
    t.AddBool("dlss.composedImageAvailable", dlss::ComposedTexture() != nullptr);
    t.AddBool("aa.outputImageAvailable", enhance::OutputTexture() != nullptr);
    nr::ResultKind nrKind = nr::ResultKind::None;
    t.AddBool("nr.resultImageAvailable", nr::ResultTexture(now, false, &nrKind) != nullptr);
    t.Add("nr.resultKind", nrKind == nr::ResultKind::BringUp      ? "bring-up (NR on the native image, native size)"
                           : nrKind == nr::ResultKind::Production ? "production (NR on the super-resolution image, twice the render size, before the alpha pass)"
                                                                  : "none");
    {
        const nr::Status ns = nr::GetStatus();
        t.Add("nr.state", ns.availability == nr::Availability::Available ? "available" : ns.reason[0] != 0 ? ns.reason : "off");
        t.Add("nr.streams", ns.streams);
        t.Add("nr.liveStreams", ns.liveStreams);
        t.Add("nr.runs", ns.runs);
        t.Add("nr.fallbacks", ns.fallbacks);
        t.Add("nr.lastGpuMicroseconds", static_cast<uint64_t>(ns.lastGpuMs * 1000.0));
        t.Add("nr.videoMemoryMb", ns.videoMemoryMb);
        t.AddBool("nr.late", ns.late); // a bring-up stream uses the late variant now (PreviewNRLate applies to the bring-up only)
        t.Add("nr.uiDrawsGivenResult", g_stats.nrSwaps.load(std::memory_order_relaxed));
        t.Add("nr.production.runs", ns.productionRuns);
        t.Add("nr.production.fallbacks", ns.productionFallbacks);
        t.Add("nr.production.lastGpuMicroseconds", static_cast<uint64_t>(ns.productionGpuMs * 1000.0));
        t.Add("nr.production.imagesUsedByTheAlphaPass", g_stats.nrProductionRuns.load(std::memory_order_relaxed));
        t.Add("nr.production.uiDrawsGivenComposedImage", g_stats.nrProductionSwaps.load(std::memory_order_relaxed));
        t.Add("nr.preSr.sourceChangeResets", g_stats.srSourceResets.load(std::memory_order_relaxed));
    }

    // The table: pointers only, never dereferenced. The format is known for the entries a draw was seen sampling.
    const table::Snapshot& snap = table::Current(now);
    t.AddBool("table.ready", snap.ready);
    t.Add("table.validEntries", snap.ValidCount());
    for (size_t i = 0; i < table::kEntries; ++i)
    {
        const table::Entry& e = snap.entries[i];
        const std::string key = "table.entry." + std::to_string(i);
        if (!e.Valid())
        {
            t.Add(key, std::string("empty (") + table::EntryRole(i) + ")");
            continue;
        }
        std::string format = "unknown";
        if (i < s.held.size() && reinterpret_cast<uintptr_t>(s.held[i].resource.Get()) == e.resource)
        {
            ComPtr<ID3D11Texture2D> texture;
            D3D11_TEXTURE2D_DESC desc {};
            if (SUCCEEDED(s.held[i].resource.As(&texture)))
            {
                texture->GetDesc(&desc);
                format = FormatText(static_cast<uint32_t>(desc.Format));
            }
        }
        t.Add(key, TextureText(e.resource, e.width, e.height, format) + " (" + table::EntryRole(i) + ")");
    }
    for (size_t i = 0; i < s.held.size(); ++i)
    {
        const Held& h = s.held[i];
        if (h.resource == nullptr)
            continue;
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC desc {};
        std::string shape = "not a 2D texture";
        if (SUCCEEDED(h.resource.As(&texture)))
        {
            texture->GetDesc(&desc);
            shape = std::format("{}x{} format {}", desc.Width, desc.Height, FormatText(static_cast<uint32_t>(desc.Format)));
        }
        const bool current = reinterpret_cast<uintptr_t>(h.resource.Get()) == snap.entries[i].resource;
        t.Add("held.entry." + std::to_string(i),
              std::format("{} {}, last sampled {} ms ago, {} the table entry now", PointerText(reinterpret_cast<uintptr_t>(h.resource.Get())), shape,
                          now >= h.lastSampled ? now - h.lastSampled : 0, current ? "same as" : "NOT the same as"));
    }

    const std::vector<Derived> live = s.derived.Live(now);
    t.Add("derived.count", live.size());
    for (size_t i = 0; i < live.size(); ++i)
    {
        const Derived& d = live[i];
        t.Add("derived." + std::to_string(i),
              std::format("{} {}x{} format {}, from preview entry {}, hops {}, kind {}, first seen {} ms ago, last seen {} ms ago",
                          PointerText(d.ptr), d.info.width, d.info.height, FormatText(d.info.format), d.sourceEntry, d.hops,
                          d.kind == DerivedKind::Draw ? "render target of a draw" : "copy", now >= d.firstSeen ? now - d.firstSeen : 0,
                          now >= d.lastSeen ? now - d.lastSeen : 0));
    }

    AddDrawFacts(t, "lastSamplerDraw", s.lastDraw, now);
    for (size_t k = 0; k < s.lastSwap.size(); ++k)
    {
        const StoredSwap& sw = s.lastSwap[k];
        const std::string prefix = std::string("lastSwap.") + SwapKindName(static_cast<SwapKind>(k));
        t.AddBool(prefix + ".seen", sw.valid);
        if (!sw.valid)
            continue;
        t.Add(prefix + ".count", sw.count);
        t.Add(prefix + ".ageMs", now >= sw.tick ? now - sw.tick : 0);
        t.Add(prefix + ".slot", std::format("t{}", sw.record.slot));
        t.Add(prefix + ".srvBeforeSwap", PointerText(sw.record.before));
        t.Add(prefix + ".srvAfterSwap", PointerText(sw.record.afterSwap));
        t.Add(prefix + ".srvAfterDraw", PointerText(sw.record.afterDraw));
        t.Add(prefix + ".replacementView", PointerText(sw.record.replacement));
        t.AddBool(prefix + ".drawFactsAreFromTheSwappedDraw", sw.sameDraw);
        AddDrawFacts(t, prefix + ".draw", sw.draw, now);
    }

    const dump::Status status = dump::GetStatus();
    t.Add("output.folder", Utf8(dump::OutputRoot()));
    t.Add("output.dumpsWritten", status.dumps);
    t.Add("output.dumpsRefused", status.refusals);
    t.Add("output.filesWritten", status.files);
    t.Add("output.bytesWritten", status.bytes);
    t.Add("output.limits", std::format("{} files, {} per session", kMaxFiles, SizeText(kMaxBytes)));
    return t.Str();
}

// Starts the dump of the preview images (draw thread). Returns the result text.
std::string StartDump(ID3D11DeviceContext* context, uint64_t now)
{
    // The copy into the staging texture is skipped by the runtime while the game has a predicate set (SetPredication): the files would be
    // black. Wait for a later draw instead (ServiceDump retries at the next complete preview image).
    if (dump::PredicationActive(context))
        return "postponed, the game has a predicate set (SetPredication) on this draw; the dump is retried at the next complete preview image";
    State& s = S();
    std::vector<ComPtr<ID3D11Texture2D>> keep; // the held resources as textures, alive until Begin has made its copies
    std::vector<dump::Source> sources;

    const auto asTexture = [&keep](const Held& h) -> ID3D11Texture2D*
    {
        ComPtr<ID3D11Texture2D> texture;
        if (h.resource == nullptr || FAILED(h.resource.As(&texture)))
            return nullptr;
        keep.push_back(texture);
        return texture.Get();
    };

    {
        dump::Source source;
        source.name = "native";
        source.texture = asTexture(s.held[0]);
        source.missing = "no draw has been seen sampling preview output entry 0 since the probe was switched on";
        source.age = ImageAgeText(now, detail::g_lastActivity.load(std::memory_order_relaxed), s.held[0].lastSampled);
        sources.push_back(std::move(source));
    }
    size_t extras = 0;
    for (size_t e = 1; e < s.held.size() && extras < kMaxExtraImages; ++e)
    {
        const Held& h = s.held[e];
        if (h.resource == nullptr || now < h.lastSampled || now - h.lastSampled > kExtraImageMs)
            continue;
        dump::Source source;
        source.name = "native-" + std::to_string(e);
        source.texture = asTexture(h);
        source.age = ImageAgeText(now, detail::g_lastActivity.load(std::memory_order_relaxed), h.lastSampled);
        if (source.texture == nullptr)
            continue;
        sources.push_back(std::move(source));
        ++extras;
    }
    sources.push_back({ "aa", enhance::OutputTexture(), "the spatial AA has not produced an image" });
    {
        dump::Source dlssSource { "dlss", dlss::ComposedTexture(), "the DLSS path has no evaluated preview image" };
        if (dlss::ComposedFromNr())
            dlssSource.age = "kind: composed from the production NR output (preview NR ran on the super-resolution image)";
        sources.push_back(std::move(dlssSource));
    }
    // The NR result is shared with the D3D12 queue: the wait (late variant) is queued on this context now, before the copy that Begin
    // issues, and the reference keeps the image alive until then.
    nr::ResultKind nrKind = nr::ResultKind::None;
    const ComPtr<ID3D11Texture2D> nrResult = nr::ResultTexture(now, true, &nrKind);
    dump::Source nrSource { "nr", nrResult.Get(), "preview NR has no current result" };
    nrSource.age = NrImageNote(static_cast<int>(nrKind)); // bring-up or production: the picture does not say which
    sources.push_back(std::move(nrSource));

    std::string why;
    if (!dump::Begin(context, sources, BuildState(now), now, why))
        return "refused, " + why;
    return "ok, saving the preview images; the files are written when the GPU has finished the copies (a few frames)";
}

// One timestamped line (or more) to chara-probe.out.
void WriteOut(const std::vector<std::string>& lines)
{
    State& s = S();
    if (!s.channel)
        return;
    if (!s.channel->AppendOutput(lines) && !s.outputFailureLogged)
    {
        // A read-only folder, a full disk, or a viewer that locks the file: the user gets no answer, so the log says why, once.
        s.outputFailureLogged = true;
        LOG_WARN("CharaView probe: the answer file {} could not be written (read-only folder, full disk or a program holding the file); the "
                 "answers to commands only appear in the log",
                 Utf8(s.channel->OutputPath()));
    }
}

void PumpDump(ID3D11DeviceContext* context, uint64_t now)
{
    if (!dump::InFlight())
        return;
    if (const std::optional<DumpOutcome> outcome = dump::Pump(context, now))
    {
        LOG_INFO("CharaView probe: preview images: {}", outcome->message);
        WriteOut({ LogTimeNow() + " dump: " + outcome->message });
    }
}

// What the commands do (see CharaProbeCommands.h). Draw thread.
struct ProbeActions
{
    uint64_t now = 0;

    std::string SetCopies(bool on)
    {
        S().derived.SetCopiesEnabled(on);
        return std::string("ok, copy tracking is now ") + (on ? "on" : "off") +
               (on ? "" : "; new copies are not followed (textures already derived stay until they expire)");
    }

    std::string RequestDump()
    {
        bool expected = false;
        if (dump::InFlight() || !g_dumpPending.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            return "refused, a dump is already waiting or in progress";
        return "ok, dump requested; a line with the result follows when the files are written";
    }

    std::string RequestState()
    {
        const DumpOutcome outcome = dump::WriteStateOnly(BuildState(now));
        dump::SetMessage(outcome.message);
        return outcome.message;
    }

    std::string ResetAll()
    {
        Reset();
        return "ok, derived textures, logged cases and counters were forgotten";
    }
};

// Polls chara-probe.cmd (about every kCommandPollMs), runs its commands and answers in chara-probe.out. Draw thread.
void PollCommands(uint64_t now)
{
    State& s = S();
    if (!s.channel || (now >= s.lastPoll && now - s.lastPoll < kCommandPollMs))
        return;
    s.lastPoll = now;

    FILETIME ft {};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t fileNow = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const CommandChannel::Polled polled = s.channel->Poll(fileNow);
    if (polled.state != CommandChannel::Polled::State::Commands)
        return;

    std::vector<std::string> lines;
    const auto note = [&lines](const std::string& text) { lines.push_back(LogTimeNow() + " chara-probe.cmd: " + text); };
    if (polled.disposition == Disposition::Renamed)
        note("the file could not be deleted and was renamed to chara-probe.cmd.done");
    else if (polled.disposition == Disposition::Kept)
        note("the file could not be deleted or renamed; it will not run again until it changes");
    if (polled.parsed.truncatedBytes)
        note("the file is longer than " + std::to_string(kMaxCommandBytes) + " bytes; the rest was ignored");
    if (polled.parsed.truncatedLines)
        note("more than " + std::to_string(kMaxCommandLines) + " lines; the rest was ignored");
    if (polled.parsed.commands.empty())
        note("no commands in the file");

    ProbeActions actions;
    actions.now = now;
    for (const Command& command : polled.parsed.commands)
    {
        const std::string result = Execute(command, ext::Cfg().ffxiv.chara, actions);
        const std::string echo = command.text.empty() ? std::string("(empty)") : command.text;
        lines.push_back(LogTimeNow() + " " + echo + ": " + result);
        LOG_INFO("CharaView probe: command \"{}\": {}", echo, result);
    }
    WriteOut(lines);
}

// Executes a waiting "save preview images" request. It is armed when first seen and started at the first point where the preview image
// is complete (DumpService): `finisherPoint` is true right after an accepted finisher draw; a consumer swap reported since the request was
// armed also counts (every image of the frame exists then); 2 s without either, any draw will do. Draw thread, immediate context.
void ServiceDump(ID3D11DeviceContext* context, uint64_t now, bool finisherPoint)
{
    State& s = S();
    const bool pending = g_dumpPending.load(std::memory_order_relaxed);
    std::string result;
    const auto start = [&]
    {
        try
        {
            result = StartDump(context, now);
        }
        catch (...)
        {
            result = "failed, an error stopped the request";
        }
        return result.rfind("postponed", 0) == 0;
    };
    switch (s.dumpService.Service(pending, finisherPoint, now, start))
    {
    case DumpService::Result::Nothing: return;
    case DumpService::Result::Postponed:
        // The game's state forbids the copy for now (an active predicate): the request stays and the next complete preview image tries again.
        dump::SetMessage(result);
        return;
    case DumpService::Result::GaveUp:
        result = "refused, the game keeps a predicate set for the draws the dump could use; try again later";
        break;
    case DumpService::Result::Started: break;
    }
    g_dumpPending.store(false, std::memory_order_relaxed);
    dump::SetMessage(result);
    LOG_INFO("CharaView probe: save preview images: {}", result);
    if (result.rfind("ok", 0) != 0) // a started dump reports when it finishes; a refused one says so here
        WriteOut({ LogTimeNow() + " dump: " + result });
}

// Every kTickMs while the probe is on (called from Update on the draw thread, immediate context only).
void Tick(ID3D11DeviceContext* context, uint64_t now)
{
    State& s = S();
    if (now >= s.lastTick && now - s.lastTick < kTickMs)
        return;
    s.lastTick = now;
    try
    {
        PumpDump(context, now);
        PollCommands(now);
    }
    catch (...)
    {
        dump::SetMessage("failed, an error stopped the request");
    }
}

} // namespace

static void UpdateCore(uint64_t now, ID3D11DeviceContext* context)
{
    State& s = S();

    // The flood test: its own log lines and texture are handled by the flood unit; here it only decides whether the scan and the
    // copy tracking run for it.
    flood::Update(now);
    const bool wantFlood = flood::On();
    if (wantFlood != s.flood)
    {
        s.flood = wantFlood;
        if (wantFlood && !s.on)
        {
            ForgetAll();
            detail::g_lastActivity.store(0, std::memory_order_relaxed);
        }
        else if (!wantFlood && !s.on)
            ForgetAll();
        detail::g_live.store(s.on || s.flood, std::memory_order_relaxed);
    }

    const bool want = ProbeOn();
    if (want != s.on)
    {
        if (!want)
        {
            SwitchOff("PreviewProbe was switched off");
            return;
        }
        ForgetAll();
        detail::g_lastActivity.store(0, std::memory_order_relaxed);
        s.on = true;
        g_onFlag.store(true, std::memory_order_relaxed);
        s.lastGauge = 0;
        s.lastTick = 0;
        s.lastPoll = 0;
        s.channel.reset();
        s.outputFailureLogged = false;
        if (const std::filesystem::path folder = dump::OptiScalerDirectory(); !folder.empty())
            s.channel.emplace(folder);
        detail::g_live.store(true, std::memory_order_relaxed);
        g_swapSink.store(&kSink, std::memory_order_relaxed);
        const std::filesystem::path folder = dump::OutputRoot();
        LOG_INFO("CharaView probe: on (copy tracking {}, output folder {}); every distinct copy of the preview and every distinct draw "
                 "that samples it or a copy of it is logged once, with lines that start with \"CharaView probe:\"; commands are read from {} and answered in {} (at most {} copy and {} "
                 "sampler cases)",
                 s.derived.CopiesEnabled() ? "on" : "off", folder.empty() ? std::string("unknown") : Utf8(folder),
                 s.channel ? Utf8(s.channel->CommandPath()) : std::string("nowhere"), s.channel ? Utf8(s.channel->OutputPath()) : std::string("nowhere"), kCopyCaseCap,
                 kSamplerCaseCap);
        return;
    }
    if ((s.on || s.flood) && (now < s.lastGauge || now - s.lastGauge >= kGaugeMs))
    {
        s.lastGauge = now;
        s.derived.Prune(now); // lets go of the textures of entries that expired (they are held only while the entry lives)
        g_stats.probeDerivedLive.store(static_cast<uint32_t>(s.derived.Count(now)), std::memory_order_relaxed);
    }
    if (s.on && context != nullptr)
    {
        Tick(context, now);
        ServiceDump(context, now, false); // a swap seen since the request was armed, or the 2 s fallback, starts it at this draw
    }
}

void Update(uint64_t now, ID3D11DeviceContext* context)
{
    UpdateCore(now, context);
    SyncResolveHook();
}


void OnPreviewComplete(ID3D11DeviceContext* context, uint64_t now)
{
    if (context == nullptr || !g_onFlag.load(std::memory_order_relaxed))
        return;
    ServiceDump(context, now, true);
}
void SetResolveHookControl(const ResolveHookControl* control) { g_resolveControl.store(control, std::memory_order_release); }

bool ResolveTrackingIncomplete() { return g_resolveIncomplete.load(std::memory_order_relaxed); }

bool RequestDump()
{
    if (!g_onFlag.load(std::memory_order_relaxed) || dump::InFlight())
        return false;
    bool expected = false;
    return g_dumpPending.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

DumpStatus GetDumpStatus()
{
    DumpStatus out;
    out.probeOn = g_onFlag.load(std::memory_order_relaxed);
    const dump::Status status = dump::GetStatus();
    out.busy = g_dumpPending.load(std::memory_order_relaxed) || status.inFlight;
    out.dumps = status.dumps;
    out.refusals = status.refusals;
    out.files = status.files;
    out.bytes = status.bytes;
    out.lastFolder = status.lastFolder;
    out.lastMessage = status.lastMessage;
    return out;
}

bool HasFollowedCopies(uint64_t now)
{
    State& s = S();
    return (s.on || s.flood) && !s.derived.Empty(now);
}

ScanResult OnDraw(ID3D11DeviceContext* context, DrawApi api, const table::Snapshot& snapshot, uint64_t now)
{
    ScanResult result;
    State& s = S();
    if ((!s.on && !s.flood) || context == nullptr)
        return result;
    if (s.on)
        ++s.drawSerial; // a draw counter: it advances for every dispatched draw, scanned or not (see SameDraw)

    // One cheap check before any COM call (ScanCanFindAnything): the scan runs while a preview output exists in the table (an open preview
    // window, also when it stopped re-rendering), while copies of the preview are alive, and for the probe alone while the renderer is active.
    const uint64_t activity = detail::g_lastActivity.load(std::memory_order_relaxed);
    const bool recent = activity != 0 && now >= activity && now - activity <= kActivityMs;
    if (!ScanCanFindAnything(s.on, s.flood, recent, !s.derived.Empty(now), AnyPreviewOutput(snapshot)))
        return result;

    if (s.on)
        ++g_stats.probeScanned;
    ID3D11ShaderResourceView* views[kScanSlots] {};
    context->PSGetShaderResources(0, kScanSlots, views);

    size_t count = 0;
    for (UINT i = 0; i < kScanSlots; ++i)
    {
        if (views[i] == nullptr)
            continue;
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        views[i]->Release();
        const uintptr_t texture = reinterpret_cast<uintptr_t>(resource.Get());
        if (texture == 0)
            continue;

        ScanHit hit;
        hit.slot = i;
        hit.texture = texture;
        const int entry = table::IndexOfPointer(snapshot, texture);
        if (IsPreviewEntry(entry))
        {
            hit.entry = entry;
            hit.sourceEntry = entry;
        }
        else if (const Derived* found = FindDerived(s, resource.Get(), texture, now))
        {
            hit.derived = true;
            hit.sourceEntry = found->sourceEntry;
            hit.hops = found->hops;
            hit.kind = found->kind;
            if (recent)
                s.derived.Touch(texture, now); // sampled while the preview renders: still alive (never otherwise, or it would never expire)
        }
        else
            continue;
        hit.resource = std::move(resource);
        result.slotMask |= 1u << i;
        result.hits[count++] = std::move(hit);
    }
    result.count = count;
    if (count == 0)
        return result;

    if (s.on)
    {
        ++g_stats.probeSamplerDraws;
        // Keep the last resource sampled for each preview output alive for the image dump (a reference per entry 0..7).
        for (size_t n = 0; n < count; ++n)
        {
            const ScanHit& h = result.hits[n];
            if (h.derived || !IsPreviewEntry(h.entry))
                continue;
            Held& held = s.held[static_cast<size_t>(h.entry)];
            held.lastSampled = now;
            if (held.resource.Get() != h.resource.Get())
                held.resource = h.resource;
        }
    }
    // The draw samples a preview image: read where it renders. Only draws that matched get here.
    ComPtr<ID3D11RenderTargetView> rtv;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    ComPtr<ID3D11Resource> rtResource;
    D3D11_TEXTURE2D_DESC rtDesc {};
    bool haveTarget = false;
    if (rtv != nullptr)
    {
        rtv->GetResource(&rtResource);
        ComPtr<ID3D11Texture2D> rtTexture;
        if (rtResource != nullptr && SUCCEEDED(rtResource.As(&rtTexture)))
        {
            rtTexture->GetDesc(&rtDesc);
            haveTarget = true;
        }
    }
    const uintptr_t rtPointer = reinterpret_cast<uintptr_t>(rtResource.Get());

    const BackbufferInfo& bb = Backbuffer(now);
    const bool targetIsBackbuffer = haveTarget && bb.valid && bb.texture == rtPointer;

    // The pixel shader: the probe logs it, and "is there one" decides whether the render target may become derived (a draw without a
    // pixel shader reads nothing; what it has bound at t0..t7 is left over from an earlier draw).
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);

    if (s.on)
    {
        // The viewport and the log lines are the probe's own.
        const ShaderInfo shader = GetShaderInfo(ps.Get());
        const uint32_t roles = RoleOf(ps.Get());

        D3D11_VIEWPORT viewport {};
        UINT viewportCount = 1;
        context->RSGetViewports(&viewportCount, &viewport);
        if (viewportCount < 1)
            viewport = D3D11_VIEWPORT {};

        DrawFacts facts;
        facts.valid = true;
        facts.serial = s.drawSerial;
        facts.tick = now;
        facts.api = api;
        facts.crcKnown = shader.known;
        facts.crc = shader.crc;
        facts.shaderSize = shader.size;
        facts.roles = roles;
        facts.haveTarget = haveTarget;
        facts.target = rtPointer;
        facts.targetWidth = rtDesc.Width;
        facts.targetHeight = rtDesc.Height;
        facts.targetFormat = static_cast<uint32_t>(rtDesc.Format);
        facts.backbuffer = bb.valid ? (targetIsBackbuffer ? 1 : 0) : -1;
        facts.viewportWidth = static_cast<int>(viewport.Width);
        facts.viewportHeight = static_cast<int>(viewport.Height);
        facts.hitCount = count;
        for (size_t n = 0; n < count; ++n)
        {
            const ScanHit& h = result.hits[n];
            facts.hits[n] = { h.slot, h.texture, h.entry, h.derived, h.sourceEntry, h.hops, h.kind };
        }
        s.lastDraw = facts;

        for (size_t n = 0; n < count; ++n)
        {
            const ScanHit& hit = result.hits[n];
            const uint64_t tag = hit.derived ? 1000 + static_cast<uint64_t>(hit.sourceEntry) * 16 + hit.hops + (hit.kind == DerivedKind::Draw ? 8 : 0)
                                             : static_cast<uint64_t>(hit.entry);
            const uint64_t key = CaseKey(tag, hit.slot | (static_cast<uint64_t>(api) << 8),
                                         shader.known ? (static_cast<uint64_t>(shader.crc) | (static_cast<uint64_t>(shader.size) << 32))
                                                      : reinterpret_cast<uintptr_t>(ps.Get()),
                                         (static_cast<uint64_t>(rtDesc.Width) << 32) | rtDesc.Height,
                                         static_cast<uint64_t>(rtDesc.Format) | (static_cast<uint64_t>(targetIsBackbuffer) << 32));
            if (!s.samplerCases.Offer(key))
                continue;

            const std::string crc = shader.known ? std::format("{:08X}", shader.crc) : std::string("unknown (shader made before the hooks)");
            const std::string target = haveTarget ? std::format("{:#x} {}x{} format {}", rtPointer, rtDesc.Width, rtDesc.Height,
                                                                FormatText(static_cast<uint32_t>(rtDesc.Format)))
                                                  : std::string("none");
            std::string what;
            if (hit.derived)
                what = std::format("draw samples preview-derived texture {:#x} ({} of {}, hops {})", hit.texture,
                                   hit.kind == DerivedKind::Draw ? "draw" : "copy", hit.sourceEntry, hit.hops);
            else
                what = std::format("draw samples preview texture {:#x} (entry {})", hit.texture, hit.entry);
            ++g_stats.probeSamplerLines;
            LOG_INFO("CharaView probe: {} at t{}: shader CRC {} roles {:#x}, render target {} (backbuffer? {}), viewport {}x{}, via {}", what,
                     hit.slot, crc, roles, target, bb.valid ? YesNo(targetIsBackbuffer) : "unknown", static_cast<int>(viewport.Width),
                     static_cast<int>(viewport.Height), DrawApiName(api));
            if (s.samplerCases.Count() == kSamplerCaseCap)
                LOG_INFO("CharaView probe: the limit of {} sampler-draw cases is reached; further new cases are only counted (send the command `reset`, or switch the probe off and on, to start new logs)", kSamplerCaseCap);
        }
    }

    // The render target of a draw that sampled the preview (or a copy of it) holds the image too, unless it is the screen, one of the
    // preview renderer's own textures, a screen-size intermediate, or the draw reads nothing (no pixel shader: what is bound is left
    // over from an earlier draw). Only while the preview renders (see the copy path). The rules are DecidePromotion.
    if (recent && haveTarget && rtPointer != 0)
    {
        PromotionFacts pf;
        pf.rendererRecent = recent;
        pf.haveTarget = haveTarget;
        pf.targetIsBackbuffer = targetIsBackbuffer;
        pf.targetIsTableEntry = table::IndexOfPointer(snapshot, rtPointer) >= 0;
        pf.pixelShaderBound = ps != nullptr;
        pf.target = { rtDesc.Width, rtDesc.Height, static_cast<uint32_t>(rtDesc.Format) };
        pf.backbufferKnown = bb.valid;
        pf.backbuffer = { bb.width, bb.height, bb.format };
        if (!bb.valid)
        {
            D3D11_VIEWPORT viewport {};
            UINT viewportCount = 1;
            context->RSGetViewports(&viewportCount, &viewport);
            if (viewportCount >= 1)
            {
                pf.viewportWidth = static_cast<uint32_t>(viewport.Width);
                pf.viewportHeight = static_cast<uint32_t>(viewport.Height);
            }
        }
        const Promotion verdict = DecidePromotion(pf);
        if (verdict == Promotion::Promote)
        {
            // the closest source: a preview output first, then the derived texture with the fewest hops
            const ScanHit* best = &result.hits[0];
            for (size_t n = 1; n < count; ++n)
            {
                const ScanHit& h = result.hits[n];
                if ((!h.derived && best->derived) || (h.derived == best->derived && h.hops < best->hops))
                    best = &h;
            }
            const std::shared_ptr<void> hold = HoldOf(rtResource.Get(), false); // not the backbuffer (checked above)
            const DeriveResult derived = best->derived ? s.derived.FromDerived(best->texture, rtPointer, pf.target, false, DerivedKind::Draw, now, hold)
                                                       : s.derived.FromPreview(best->entry, best->texture, rtPointer, pf.target, false, DerivedKind::Draw, now, hold);
            if (derived == DeriveResult::Added || derived == DeriveResult::Replaced)
                ++g_stats.probeDerivedAdded;
        }
        else if (s.on && (verdict == Promotion::NoPixelShader || verdict == Promotion::ScreenSizeIntermediate || verdict == Promotion::NoReference))
        {
            // Said once per case: these are the draws a reader of the log would otherwise wonder about.
            const uint64_t key = CaseKey(900 + static_cast<uint64_t>(verdict), rtPointer, (static_cast<uint64_t>(rtDesc.Width) << 32) | rtDesc.Height);
            if (s.samplerCases.Offer(key))
            {
                ++g_stats.probeSamplerLines;
                if (verdict == Promotion::NoPixelShader)
                    LOG_INFO("CharaView probe: bound, not known to be read: a draw with no pixel shader has the preview or a copy of it bound; its render target "
                             "{:#x} is not followed", rtPointer);
                else if (verdict == Promotion::ScreenSizeIntermediate)
                    LOG_INFO("CharaView probe: screen-size intermediate: {:#x} {}x{} is at least half of the {} area and is not followed as a texture derived "
                             "from the preview (a copy of the preview into it would be)",
                             rtPointer, rtDesc.Width, rtDesc.Height, bb.valid ? "backbuffer's" : "viewport's");
                else
                    LOG_INFO("CharaView probe: neither the backbuffer nor the viewport is known: the render target {:#x} {}x{} is not followed", rtPointer,
                             rtDesc.Width, rtDesc.Height);
            }
        }
    }
    return result;
}

void OnCopy(ID3D11DeviceContext*, ID3D11Resource* dest, ID3D11Resource* source)
{
    if (!Live())
        return;
    HandleCopy(dest, source, CopyDetail {});
}

void OnCopyRegion(ID3D11DeviceContext*, ID3D11Resource* dest, UINT destSub, UINT x, UINT y, UINT z, ID3D11Resource* source,
                  UINT sourceSub, const D3D11_BOX* box)
{
    if (!Live())
        return;
    CopyDetail d;
    d.via = CopyVia::CopyRegion;
    d.destSub = destSub;
    d.x = x;
    d.y = y;
    d.z = z;
    d.sourceSub = sourceSub;
    d.box = box;
    HandleCopy(dest, source, d);
}

void OnResolve(ID3D11DeviceContext*, ID3D11Resource* dest, UINT destSub, ID3D11Resource* source, UINT sourceSub, DXGI_FORMAT format)
{
    if (!Live())
        return;
    CopyDetail d;
    d.via = CopyVia::Resolve;
    d.destSub = destSub;
    d.sourceSub = sourceSub;
    d.format = format;
    HandleCopy(dest, source, d);
}

void Reset()
{
    // Not released here: the preview textures kept for the dump, a dump in flight and the session's output budget.
    State& s = S();
    ForgetDerived();
    s.lastDraw = DrawFacts {};
    s.lastSwap = {};
    s.copyCases.Clear();
    s.samplerCases.Clear();
    g_stats.tintApplied = 0;
    g_stats.floodSwaps = 0;
    g_stats.probeScanned = 0;
    g_stats.probeSamplerDraws = 0;
    g_stats.probeCopies = 0;
    g_stats.probeCopiesRefused = 0;
    g_stats.probeDerivedAdded = 0;
    g_stats.probeCopyLines = 0;
    g_stats.probeSamplerLines = 0;
    g_stats.probeSwapLines = 0;
}

void Detach()
{
    State& s = S();
    g_swapSink.store(nullptr, std::memory_order_relaxed);
    s.flood = false; // before SwitchOff: nothing stays live
    if (s.on)
        SwitchOff("the hooks are being removed");
    else
        ForgetAll();
    detail::g_live.store(false, std::memory_order_relaxed);
    flood::Release();
    SyncResolveHook(); // both aids are off now: the ResolveSubresource detour goes (its own transaction)
}
} // namespace ext::ffxiv::chara::probe
