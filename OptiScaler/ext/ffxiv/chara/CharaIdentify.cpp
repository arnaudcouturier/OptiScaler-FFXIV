#include "pch.h"

#include "ext/ffxiv/chara/CharaIdentify.h"
#include "ext/ffxiv/chara/CharaDlssApi.h"
#include "ext/ffxiv/chara/CharaDlssRules.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTable.h"
#include "ext/ffxiv/chara/CharaTags.h"

#include <array>
#include <format>
#include <string>
#include <vector>

namespace ext::ffxiv::chara::identify
{
namespace
{
constexpr uint64_t kQuietMs = 1500;     // no preview draw for this long ends an activity burst
constexpr size_t kMaxSignatures = 48;

struct State
{
    bool loggedFirstTable = false;
    bool loggedActiveTable = false;
    bool inActivity = false;
    uint64_t activityStart = 0;
    uint64_t lastActivity = 0;
    std::array<uint64_t, 5> burst {};
    uint64_t burstFinisherOk = 0;
    uint64_t burstFinisherRejected = 0;
    std::array<uint64_t, kFinisherVerdicts> burstVerdict {};
    uint64_t burstOffTable = 0;
    uint64_t burstShapeOnly = 0;
    uint64_t burstPasses = 0;
    uint32_t shapeOnlyLines = 0;
    dlss::BurstLimiter lines;                  // pass started / finisher accepted lines: once per burst, at most 24 each
    uint64_t startEvaluations = 0;       // DLSS counters when the burst started
    uint64_t startPassesLive = 0;
    uint64_t startSlots = 0;
    uint64_t startSwaps = 0;
    std::array<uint64_t, 12> startFinish {};
    std::vector<uint64_t> shapeOnlyKeys;
    uint64_t burstDepthOnly = 0;
    std::array<bool, kFinisherVerdicts> verdictLogged {};
    uint32_t offTableLines = 0;
    std::vector<uint64_t> offTableKeys;
    std::vector<uint64_t> depthOnlySignatures;
    std::vector<uint64_t> shaderKeys;
    uint32_t shaderLines = 0;
    uint32_t bursts = 0;
    std::array<uint64_t, kMaxSignatures> signatures {};
    size_t signatureCount = 0;
};

State g_state;

uint64_t Signature(const DrawView& v)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a over the fields that identify the kind of draw
    const auto mix = [&h](uint64_t value)
    {
        h ^= value;
        h *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(v.kind));
    mix(static_cast<uint64_t>(v.api));
    mix(static_cast<uint64_t>(static_cast<int64_t>(v.facts.rtv0Entry) + 2));
    mix(static_cast<uint64_t>(static_cast<int64_t>(v.facts.dsvEntry) + 2));
    mix(static_cast<uint64_t>(static_cast<int64_t>(v.facts.srvEntry) + 2));
    mix(v.roles);
    mix(v.facts.targetWidth);
    mix(v.facts.targetHeight);
    return h | 1; // never 0 (0 marks an empty slot)
}

constexpr size_t kMaxDepthOnlyLines = 12;
constexpr uint32_t kMaxShaderLines = 64;
constexpr uint32_t kMaxOffTableLines = 4;

// "no render target 2, depth or stencil test on 1, ..." for the non-zero counts (accepted excluded), or "none".
std::string ReasonCounts(const std::array<uint64_t, kFinisherVerdicts>& counts)
{
    std::string text;
    for (size_t i = 1; i < kFinisherVerdicts; ++i)
    {
        if (counts[i] == 0)
            continue;
        if (!text.empty())
            text += ", ";
        text += std::string(FinisherVerdictName(static_cast<FinisherVerdict>(i))) + " " + std::to_string(counts[i]);
    }
    return text.empty() ? std::string("none") : text;
}

// What the DLSS path did with the accepted finishers so far (cumulative over the session).
std::string DownstreamCounts()
{
    std::string text = "PreviewDLSS off " + std::to_string(g_stats.finisherNotWanted.load()) + ", path unavailable " +
                       std::to_string(g_stats.finisherNoDlss.load());
    for (size_t i = 0; i < g_stats.finishVerdict.size(); ++i)
    {
        const uint64_t n = g_stats.finishVerdict[i].load();
        if (n != 0)
            text += std::string(", ") + dlss::FinishVerdictName(static_cast<dlss::FinishVerdict>(i)) + " " + std::to_string(n);
    }
    return text;
}

void LogEndOfBurst(uint64_t now)
{
    State& s = g_state;
    LOG_INFO("CharaView: preview render activity ended after {} ms: passes started {}, prepass {}, GBuffer {}, finisher {} (accepted {}, rejected {}), "
             "consumer {}",
             now >= s.activityStart ? now - s.activityStart : 0, s.burstPasses, s.burst[static_cast<size_t>(DrawKind::Prepass)],
             s.burst[static_cast<size_t>(DrawKind::GBuffer)], s.burst[static_cast<size_t>(DrawKind::Finisher)],
             s.burstFinisherOk, s.burstFinisherRejected, s.burst[static_cast<size_t>(DrawKind::Consumer)]);
    LOG_INFO("CharaView: that burst: finisher rejections by reason: {}; finisher-shader draws outside the preview outputs {}; draws with only the finisher shape (not counted) {}; "
             "depth-only draws into a table depth texture {}; shaders tagged so far: geometry {}, finisher {}, equipment display {}, "
             "UI {}; accepted finishers so far: {}",
             ReasonCounts(s.burstVerdict), s.burstOffTable, s.burstShapeOnly, s.burstDepthOnly, g_stats.tagged[0].load(), g_stats.tagged[1].load(),
             g_stats.tagged[2].load(), g_stats.tagged[3].load(), DownstreamCounts());
    {
        // What the DLSS path did in this burst (differences of the session counters).
        std::string finishes;
        for (size_t i = 0; i < s.startFinish.size(); ++i)
        {
            const uint64_t n = g_stats.finishVerdict[i].load() - s.startFinish[i];
            if (n != 0)
                finishes += std::string(finishes.empty() ? "" : ", ") + dlss::FinishVerdictName(static_cast<dlss::FinishVerdict>(i)) + " " + std::to_string(n);
        }
        const uint64_t evaluations = dlss::GetCounters().evaluations - s.startEvaluations;
        const uint64_t live = g_stats.passesLive.load() - s.startPassesLive;
        const uint64_t slots = g_stats.slotsAcquired.load() - s.startSlots;
        const uint64_t swaps = g_stats.consumerSwaps.load() - s.startSwaps;
        if (!finishes.empty() || evaluations != 0 || live != 0 || slots != 0 || swaps != 0)
            LOG_INFO("CharaView: DLSS path in that burst: passes jittered and replayed {}, preview slots acquired {}, finish outcomes: {}, "
                     "evaluations {}, UI draws given the processed image {}",
                     live, slots, finishes.empty() ? std::string("none") : finishes, evaluations, swaps);
    }
    s.inActivity = false;
}

uint64_t Mix(uint64_t h, uint64_t value)
{
    h ^= value;
    h *= 1099511628211ull;
    return h;
}

bool Remember(std::vector<uint64_t>& seen, uint64_t key)
{
    for (const uint64_t k : seen)
    {
        if (k == key)
            return false;
    }
    seen.push_back(key);
    return true;
}

std::string ShaderText(ID3D11PixelShader* shader)
{
    if (shader == nullptr)
        return "no pixel shader";
    const ShaderInfo info = GetShaderInfo(shader);
    if (!info.known)
        return "pixel shader CRC not recorded (created before the hooks)";
    return std::format("pixel shader CRC {:08X}, {} bytes", info.crc, info.size);
}
} // namespace

void Observe(const DrawView& v)
{
    State& s = g_state;

    ++g_stats.classified;
    ++g_stats.kind[static_cast<size_t>(v.kind)];

    if (!s.loggedFirstTable && v.table != nullptr && v.table->ready)
    {
        s.loggedFirstTable = true;
        table::LogTable("feature on, before any preview activity", *v.table);
    }

    if (v.kind == DrawKind::Ignore)
        return;

    if (!s.inActivity)
    {
        s.inActivity = true;
        s.activityStart = v.now;
        s.burst = {};
        s.burstFinisherOk = s.burstFinisherRejected = 0;
        s.burstVerdict = {};
        s.burstOffTable = s.burstDepthOnly = 0;
        s.burstShapeOnly = s.burstPasses = 0;
        s.startEvaluations = dlss::GetCounters().evaluations;
        s.startPassesLive = g_stats.passesLive.load();
        s.startSlots = g_stats.slotsAcquired.load();
        s.startSwaps = g_stats.consumerSwaps.load();
        for (size_t i = 0; i < s.startFinish.size(); ++i)
            s.startFinish[i] = g_stats.finishVerdict[i].load();
        ++s.bursts;
        LOG_INFO("CharaView: preview render activity started (burst {})", s.bursts);
        if (!s.loggedActiveTable && v.table != nullptr)
        {
            s.loggedActiveTable = true;
            table::LogTable("first preview activity", *v.table);
        }
    }
    s.lastActivity = v.now;
    ++s.burst[static_cast<size_t>(v.kind)];
    if (v.passStarted)
    {
        ++s.burstPasses;
        if (s.lines.ShouldLog(0, s.bursts))
            LOG_INFO("CharaView: pass started (pass {}, {}x{}, first draw is a {} into table entry {}, via {})", v.pass != nullptr ? v.pass->counter : 0u,
                     v.facts.targetWidth, v.facts.targetHeight, DrawKindName(v.kind), v.facts.rtv0Entry, DrawApiName(v.api));
    }

    // One line per new signature.
    const uint64_t sig = Signature(v);
    for (size_t i = 0; i < s.signatureCount; ++i)
    {
        if (s.signatures[i] == sig)
            return;
    }
    if (s.signatureCount >= kMaxSignatures)
        return;
    s.signatures[s.signatureCount++] = sig;

    const int e = v.facts.rtv0Entry;
    const int d = v.facts.dsvEntry;
    const int t = v.facts.srvEntry;
    LOG_INFO("CharaView draw signature {}: {} via {}, target {}x{} entry {}{}, depth entry {}, samples preview entry {}, "
             "shader roles {:#x}",
             s.signatureCount, DrawKindName(v.kind), DrawApiName(v.api), v.facts.targetWidth, v.facts.targetHeight,
             e >= 0 ? std::to_string(e) : std::string("none"),
             e >= 0 ? std::string(" (") + table::EntryRole(static_cast<size_t>(e)) + ")" : std::string(),
             d >= 0 ? std::to_string(d) : std::string("none"), t >= 0 ? std::to_string(t) : std::string("none"), v.roles);
}

void NoteQuietDraw(const table::Snapshot& snapshot)
{
    ++g_stats.classified;
    ++g_stats.kind[static_cast<size_t>(DrawKind::Ignore)];
    if (!g_state.loggedFirstTable && snapshot.ready)
    {
        g_state.loggedFirstTable = true;
        table::LogTable("feature on, before any preview activity", snapshot);
    }
}

void OnFinisher(const DrawView& view)
{
    State& s = g_state;
    const size_t reason = static_cast<size_t>(view.finisherVerdict);
    ++g_stats.finisherVerdict[reason];
    ++s.burstVerdict[reason];
    if (view.finisherVerdict == FinisherVerdict::Accepted)
    {
        ++g_stats.finisherAccepted;
        ++s.burstFinisherOk;
        if (s.lines.ShouldLog(1, s.bursts))
            LOG_INFO("CharaView: finisher accepted (pass {} {}x{}, table entry {}, via {}, {})", view.pass != nullptr ? view.pass->counter : 0u,
                     view.facts.targetWidth, view.facts.targetHeight, view.facts.rtv0Entry, DrawApiName(view.api), ShaderText(view.pixelShader));
        return;
    }

    ++g_stats.finisherRejected;
    ++s.burstFinisherRejected;
    if (s.verdictLogged[reason])
        return;
    s.verdictLogged[reason] = true;

    const FinisherCheck& c = view.finisherCheck;
    LOG_INFO("CharaView: finisher rejected (first time for this reason): {}. Target {}x{} texture format {} view format {}, render "
             "target {}, depth or stencil test {}, pass {} ({}x{}), viewport origin {:.2f}/{:.2f} size {:.1f}x{:.1f}, table entry {}, "
             "via {}, {}",
             FinisherVerdictName(view.finisherVerdict), c.targetWidth, c.targetHeight, c.targetFormat, view.facts.rtvFormat,
             c.rtv0Present ? "yes" : "no", c.depthOrStencilTest ? "yes" : "no", c.passActive ? "active" : "not active", c.passWidth,
             c.passHeight, c.viewportX, c.viewportY, c.viewportWidth, c.viewportHeight, view.facts.rtv0Entry, DrawApiName(view.api),
             ShaderText(view.pixelShader));
}

void OnOffTableFinisher(const DrawView& view)
{
    State& s = g_state;
    ++g_stats.finisherOffTable;
    ++s.burstOffTable;
    if (s.offTableLines >= kMaxOffTableLines)
        return;
    const uint64_t key = Mix(Mix(Mix(1, view.facts.targetWidth), view.facts.targetHeight), view.facts.rtvFormat);
    if (!Remember(s.offTableKeys, key))
        return;
    ++s.offTableLines;
    LOG_INFO("CharaView: the finisher shader drew outside the preview outputs (not counted as a preview finisher): target {}x{} view "
             "format {}, table entry {}, via {}, {}",
             view.facts.targetWidth, view.facts.targetHeight, view.facts.rtvFormat, view.facts.rtv0Entry, DrawApiName(view.api),
             ShaderText(view.pixelShader));
}

void OnShapeOnlyFinisher(const DrawView& view)
{
    State& s = g_state;
    ++g_stats.finisherShapeOnly;
    ++s.burstShapeOnly;
    if (s.shapeOnlyLines >= kMaxOffTableLines)
        return;
    const ShaderInfo info = view.pixelShader != nullptr ? GetShaderInfo(view.pixelShader) : ShaderInfo {};
    const uint64_t key = info.known ? Mix(Mix(5, info.crc), info.size) : Mix(6, reinterpret_cast<uintptr_t>(view.pixelShader));
    if (!Remember(s.shapeOnlyKeys, key))
        return;
    ++s.shapeOnlyLines;
    LOG_INFO("CharaView: a draw has the shape of the finisher (output at the pass size, samples the G-buffer) but its shader is not the "
             "tagged finisher; it is not treated as a finisher and does not end the pass: table entry {}, depth entry {}, via {}, {}",
             view.facts.rtv0Entry, view.facts.dsvEntry, DrawApiName(view.api), ShaderText(view.pixelShader));
}

void ObserveDepthOnly(const DrawView& view)
{
    State& s = g_state;
    const DrawFacts& f = view.facts;
    ++g_stats.depthOnlyTable;
    ++s.burstDepthOnly;
    if (s.depthOnlySignatures.size() >= kMaxDepthOnlyLines)
        return;

    const uint32_t cbBucket = f.vsCb0Bytes == 0 ? 0u : f.vsCb0Bytes < kCameraBlockBytes ? 1u : 2u;
    uint64_t key = Mix(2, static_cast<uint64_t>(static_cast<int64_t>(f.dsvEntry) + 2));
    key = Mix(key, cbBucket);
    key = Mix(key, f.viewportMatchesTarget ? 1u : 0u);
    key = Mix(key, f.samples);
    key = Mix(key, static_cast<uint64_t>(view.api));
    if (!Remember(s.depthOnlySignatures, key))
        return;

    LOG_INFO("CharaView depth-only draw {}: via {}, depth entry {} ({}), {}x{}, {} samples, vertex-shader cb0 {} bytes (the prepass "
             "rule needs at least {}), viewport matches the target {}, prepass rule {}",
             s.depthOnlySignatures.size(), DrawApiName(view.api), f.dsvEntry,
             f.dsvEntry >= 0 ? table::EntryRole(static_cast<size_t>(f.dsvEntry)) : "none", f.targetWidth, f.targetHeight, f.samples,
             f.vsCb0Bytes, kCameraBlockBytes, f.viewportMatchesTarget ? "yes" : "no",
             IsPrepass(f) && GeometryAccepted(f) ? "satisfied" : "not satisfied");
}

void ObserveShader(const DrawView& view)
{
    State& s = g_state;
    if (s.shaderLines >= kMaxShaderLines || view.pixelShader == nullptr)
        return;

    const ShaderInfo info = GetShaderInfo(view.pixelShader);
    const uint64_t key = info.known ? Mix(Mix(3, info.crc), info.size) : Mix(4, reinterpret_cast<uintptr_t>(view.pixelShader));
    const bool renders = view.facts.rtv0Entry >= 0;
    if (!Remember(s.shaderKeys, Mix(key, renders ? 1u : 2u)))
        return;

    ++s.shaderLines;
    const int entry = renders ? view.facts.rtv0Entry : view.facts.srvEntry;
    LOG_INFO("CharaView shader {}/{}: {} table entry {} ({}), {}, classified as {}, roles {:#x}, via {}", s.shaderLines, kMaxShaderLines,
             renders ? "renders into" : "samples", entry, table::EntryRole(static_cast<size_t>(entry)), ShaderText(view.pixelShader),
             DrawKindName(view.kind), view.roles, DrawApiName(view.api));
}

uint32_t BurstNumber() { return g_state.bursts; }


void Maintain(uint64_t nowMs)
{
    State& s = g_state;
    if (s.inActivity && nowMs >= s.lastActivity && nowMs - s.lastActivity > kQuietMs)
        LogEndOfBurst(nowMs);
}
} // namespace ext::ffxiv::chara::identify
