#include "pch.h"

#include "ext/ffxiv/chara/CharaIdentify.h"
#include "ext/ffxiv/chara/CharaStats.h"
#include "ext/ffxiv/chara/CharaTable.h"

#include <array>

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

void LogEndOfBurst(uint64_t now)
{
    State& s = g_state;
    LOG_INFO("CharaView: preview render activity ended after {} ms: prepass {}, GBuffer {}, finisher {} (accepted {}, rejected {}), "
             "consumer {}",
             now >= s.activityStart ? now - s.activityStart : 0, s.burst[static_cast<size_t>(DrawKind::Prepass)],
             s.burst[static_cast<size_t>(DrawKind::GBuffer)], s.burst[static_cast<size_t>(DrawKind::Finisher)],
             s.burstFinisherOk, s.burstFinisherRejected, s.burst[static_cast<size_t>(DrawKind::Consumer)]);
    s.inActivity = false;
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

void OnFinisher(bool accepted)
{
    State& s = g_state;
    if (accepted)
    {
        ++g_stats.finisherAccepted;
        ++s.burstFinisherOk;
    }
    else
    {
        ++g_stats.finisherRejected;
        ++s.burstFinisherRejected;
    }
}

void Maintain(uint64_t nowMs)
{
    State& s = g_state;
    if (s.inActivity && nowMs >= s.lastActivity && nowMs - s.lastActivity > kQuietMs)
        LogEndOfBurst(nowMs);
}
} // namespace ext::ffxiv::chara::identify
