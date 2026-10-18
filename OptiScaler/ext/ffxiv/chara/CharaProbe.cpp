#include "pch.h"

#include "ext/ffxiv/chara/CharaProbe.h"

#include "ext/ffxiv/chara/CharaIdentify.h"
#include "ext/ffxiv/chara/CharaProbeHooks.h"
#include "ext/ffxiv/chara/CharaProbeRules.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTags.h"
#include "ext/ffxiv/ui/FfxivUiBoundary.h"
#include "ext/ffxiv/ui/UiDiagFormat.h"

#include <array>
#include <filesystem>
#include <format>
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
constexpr uint32_t kScreenClassWidth = 3000; // "3840x2160-class" render target: render-target capable and at least this large
constexpr uint32_t kScreenClassHeight = 1700;
constexpr UINT kScanSlots = 8;               // pixel-shader resources compared per draw

struct BackbufferInfo
{
    bool valid = false;
    uintptr_t texture = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
};

struct State
{
    bool on = false;
    DerivedSet derived;
    CaseLog<kCopyCaseCap> copyCases;
    CaseLog<kSamplerCaseCap> samplerCases;
    BackbufferInfo backbuffer;
    bool backbufferKnown = false; // backbuffer holds an answer (valid or not) taken at backbufferStamp
    uint64_t backbufferStamp = 0;
    uint64_t backbufferFrame = 0; // g_stats.frames when the answer was taken: a new frame asks again
    uint64_t lastGauge = 0;
    std::array<bool, 3> swapSeen {};
    std::array<uint32_t, 3> swapBurst {};
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

std::filesystem::path OutputDirectory()
{
    wchar_t path[32768] {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OutputDirectory), &module) ||
        !GetModuleFileNameW(module, path, 32768))
        return {};
    return std::filesystem::path(path).parent_path() / "OptiScaler" / "PreviewDebug";
}

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

// ---- Consumer swap trace (sink of SwapTrace) -----------------------------------------------------------------------------------

// A trace is taken once per burst of preview activity and kind of swap.
bool ArmSwap(SwapKind kind)
{
    State& s = S();
    if (!s.on || g_stats.probeSwapLines.load(std::memory_order_relaxed) >= kMaxSwapLines)
        return false;
    const size_t k = static_cast<size_t>(kind);
    const uint32_t burst = identify::BurstNumber();
    if (k >= s.swapSeen.size() || (s.swapSeen[k] && s.swapBurst[k] == burst))
        return false;
    s.swapSeen[k] = true;
    s.swapBurst[k] = burst;
    return true;
}

void ReportSwap(const SwapRecord& r)
{
    ++g_stats.probeSwapLines;
    LOG_INFO("CharaView probe: consumer swap [{}]: t{} before {:#x} after-swap {:#x} after-draw {:#x} (replacement {:#x})",
             SwapKindName(r.kind), r.slot, r.before, r.afterSwap, r.afterDraw, r.replacement);
}

const SwapSink kSink { &ArmSwap, &ReportSwap };

// ---- Switching ------------------------------------------------------------------------------------------------------------

void ForgetAll()
{
    State& s = S();
    s.derived.Clear();
    s.backbuffer = BackbufferInfo {};
    s.backbufferKnown = false;
    s.swapSeen = {};
    s.swapBurst = {};
    g_stats.probeDerivedLive.store(0, std::memory_order_relaxed);
}

void SwitchOff(const char* why)
{
    State& s = S();
    g_swapSink.store(nullptr, std::memory_order_relaxed);
    detail::g_live.store(false, std::memory_order_relaxed);
    s.on = false;
    LOG_INFO("CharaView probe: off ({}); copy cases logged {}, sampler cases logged {}, consumer swap traces {}, derived textures seen {}",
             why, g_stats.probeCopyLines.load(), g_stats.probeSamplerLines.load(), g_stats.probeSwapLines.load(),
             g_stats.probeDerivedAdded.load());
    ForgetAll();
}

// ---- Copies ---------------------------------------------------------------------------------------------------------------

enum class CopyVia : uint8_t
{
    CopyResource,
    CopyRegion,
    Resolve,
};

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

void HandleCopy(ID3D11Resource* dest, ID3D11Resource* source, const CopyDetail& detail)
{
    State& s = S();
    if (!s.on || dest == nullptr || source == nullptr || !table::Ready())
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
    if (follow)
    {
        const DeriveResult result = fromPreview ? s.derived.FromPreview(entry, src, dst, info, destIsPreviewEntry, DerivedKind::Copy, now)
                                                : s.derived.FromDerived(src, dst, info, destIsPreviewEntry, DerivedKind::Copy, now);
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

    const int sourceEntry = fromPreview ? entry : from.sourceEntry;
    const uint32_t hops = fromPreview ? 1 : from.hops + 1;
    const uint64_t sourceTag = fromPreview ? static_cast<uint64_t>(entry) : 1000 + static_cast<uint64_t>(from.sourceEntry) * 16 + from.hops;
    const uint64_t key = CaseKey(sourceTag, dst, (static_cast<uint64_t>(td.Width) << 32) | td.Height,
                                 static_cast<uint64_t>(td.Format) | (static_cast<uint64_t>(detail.via) << 32));
    if (!s.copyCases.Offer(key))
        return;

    // The line is about to be written: this is the only place that asks the swapchain for the backbuffer on the copy path.
    const BackbufferInfo& bb = Backbuffer(now);
    const bool isBackbuffer = bb.valid && bb.texture == dst;
    const bool renderTarget = (td.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
    const bool shaderResource = (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    const bool screenClass = renderTarget && td.Width >= kScreenClassWidth && td.Height >= kScreenClassHeight;

    std::string what = fromPreview ? std::format("copy of preview entry {}", entry)
                                   : std::format("copy of derived texture {:#x} (entry {}, hops {})", src, from.sourceEntry, from.hops);
    std::string flag;
    if (isBackbuffer)
        flag = " <-- destination is the swapchain backbuffer";
    else if (screenClass)
        flag = " <-- destination is a screen-size render target";
    ++g_stats.probeCopyLines;
    LOG_INFO("CharaView probe: {} -> texture {:#x} {}x{} format {} (backbuffer? {}, render target? {}, shader resource? {}, usage {}, "
             "screen-size render target? {}); preview entry {}, hops {}; via {}{}",
             what, dst, td.Width, td.Height, FormatText(static_cast<uint32_t>(td.Format)), YesNo(isBackbuffer), YesNo(renderTarget),
             YesNo(shaderResource), UsageText(td.Usage), YesNo(screenClass), sourceEntry, hops, DetailText(detail), flag);
    if (s.copyCases.Count() == kCopyCaseCap)
        LOG_INFO("CharaView probe: the limit of {} copy cases is reached; further new cases are only counted", kCopyCaseCap);
}

// ---- Sampler draws --------------------------------------------------------------------------------------------------------------

struct Hit
{
    UINT slot = 0;
    uintptr_t texture = 0;
    int entry = -1;        // preview table entry when the texture is a preview output, else -1
    bool derived = false;  // the texture is in the derived set
    int sourceEntry = -1;  // the preview entry its image came from
    uint32_t hops = 0;     // derived only
    DerivedKind kind = DerivedKind::Copy;
};
} // namespace

void Update(uint64_t now)
{
    State& s = S();
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
        s.lastGauge = 0;
        detail::g_live.store(true, std::memory_order_relaxed);
        g_swapSink.store(&kSink, std::memory_order_relaxed);
        const std::filesystem::path folder = OutputDirectory();
        LOG_INFO("CharaView probe: on (copy tracking {}, output folder {}); every distinct copy of the preview and every distinct draw "
                 "that samples it or a copy of it is logged once, with lines that start with \"CharaView probe:\" (at most {} copy and {} "
                 "sampler cases)",
                 s.derived.CopiesEnabled() ? "on" : "off", folder.empty() ? std::string("unknown") : folder.string(), kCopyCaseCap,
                 kSamplerCaseCap);
        return;
    }
    if (s.on && (now < s.lastGauge || now - s.lastGauge >= kGaugeMs))
    {
        s.lastGauge = now;
        g_stats.probeDerivedLive.store(static_cast<uint32_t>(s.derived.Count(now)), std::memory_order_relaxed);
    }
}

void OnDraw(ID3D11DeviceContext* context, DrawApi api, const table::Snapshot& snapshot, uint64_t now)
{
    State& s = S();
    if (!s.on || context == nullptr)
        return;

    // One cheap check before any COM call: scan only while the preview renderer is active or copies of it are alive.
    const uint64_t activity = detail::g_lastActivity.load(std::memory_order_relaxed);
    const bool recent = activity != 0 && now >= activity && now - activity <= kActivityMs;
    if (!recent && s.derived.Empty(now))
        return;

    ++g_stats.probeScanned;
    ID3D11ShaderResourceView* views[kScanSlots] {};
    context->PSGetShaderResources(0, kScanSlots, views);

    std::array<Hit, kScanSlots> hits {};
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

        Hit hit;
        hit.slot = i;
        hit.texture = texture;
        const int entry = table::IndexOfPointer(snapshot, texture);
        if (IsPreviewEntry(entry))
        {
            hit.entry = entry;
            hit.sourceEntry = entry;
        }
        else if (const Derived* found = s.derived.Find(texture, now))
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
        hits[count++] = hit;
    }
    if (count == 0)
        return;

    ++g_stats.probeSamplerDraws;

    // The draw samples a preview image: read what it is and where it renders. Only draws that matched get here.
    ComPtr<ID3D11PixelShader> ps;
    context->PSGetShader(&ps, nullptr, nullptr);
    const ShaderInfo shader = GetShaderInfo(ps.Get());
    const uint32_t roles = RoleOf(ps.Get());

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

    D3D11_VIEWPORT viewport {};
    UINT viewportCount = 1;
    context->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount < 1)
        viewport = D3D11_VIEWPORT {};

    const BackbufferInfo& bb = Backbuffer(now);
    const bool targetIsBackbuffer = haveTarget && bb.valid && bb.texture == rtPointer;

    for (size_t n = 0; n < count; ++n)
    {
        const Hit& hit = hits[n];
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
            LOG_INFO("CharaView probe: the limit of {} sampler-draw cases is reached; further new cases are only counted", kSamplerCaseCap);
    }

    // The render target of a draw that sampled the preview (or a copy of it) holds the image too, unless it is the screen.
    // Only while the preview renders (see the copy path), and never a screen-size target when the backbuffer is not known.
    const bool screenSize = rtDesc.Width >= kScreenClassWidth && rtDesc.Height >= kScreenClassHeight;
    if (recent && haveTarget && rtPointer != 0 && !targetIsBackbuffer && (bb.valid || !screenSize) &&
        table::IndexOfPointer(snapshot, rtPointer) < 0)
    {
        // the closest source: a preview output first, then the derived texture with the fewest hops
        const Hit* best = &hits[0];
        for (size_t n = 1; n < count; ++n)
        {
            const Hit& h = hits[n];
            if ((!h.derived && best->derived) || (h.derived == best->derived && h.hops < best->hops))
                best = &h;
        }
        const TextureInfo info { rtDesc.Width, rtDesc.Height, static_cast<uint32_t>(rtDesc.Format) };
        const DeriveResult result = best->derived ? s.derived.FromDerived(best->texture, rtPointer, info, false, DerivedKind::Draw, now)
                                                  : s.derived.FromPreview(best->entry, best->texture, rtPointer, info, false, DerivedKind::Draw, now);
        if (result == DeriveResult::Added || result == DeriveResult::Replaced)
            ++g_stats.probeDerivedAdded;
    }
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
    State& s = S();
    ForgetAll();
    s.copyCases.Clear();
    s.samplerCases.Clear();
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
    if (s.on)
        SwitchOff("the hooks are being removed");
    else
        ForgetAll();
}
} // namespace ext::ffxiv::chara::probe
