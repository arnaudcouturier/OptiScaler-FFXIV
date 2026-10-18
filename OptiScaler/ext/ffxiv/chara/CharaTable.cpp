#include "pch.h"

#include "ext/core/ProcessMemory.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/chara/CharaTable.h"

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <vector>

namespace ext::ffxiv::chara::table
{
namespace
{
constexpr size_t kScanChunk = 1u << 20;

// The feature is off for this session; this writes the one log line about it.
Resolution Inert(std::string reason)
{
    Resolution r;
    r.state = AccessorState::Inert;
    LOG_WARN("FFXIV character preview: switched off for this session ({})", reason);
    r.reason = std::move(reason);
    return r;
}

Resolution DoResolve()
{
    const GameBuild* build = MatchCurrentProcess();
    if (build == nullptr)
        return Inert("this game build is not the one the preview support was checked against");

    // The verification itself is ResolveFromImage (CharaTable.h, tested on a synthetic image); this reads the real process.
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto started = std::chrono::steady_clock::now();
    Resolution r = ResolveFromImage(
        [](uintptr_t address, void* out, size_t bytes) { return ext::core::ReadProcessBytes(address, out, bytes); }, base,
        build->charaView, kScanChunk);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (r.state != AccessorState::Ready)
        return Inert(std::move(r.reason));

    LOG_INFO("CharaView native lookup: verified accessor available true (accessor RVA {:#x}, manager pointer RVA {:#x}, "
             "scan {} ms over {} bytes)",
             r.accessorRva, r.managerPointerRva, ms, r.textBytes);
    return r;
}

Snapshot LoadSnapshot(const Resolution& r)
{
    Snapshot s;
    s.ready = true; // the accessor is verified; an empty table only means the renderer does not exist yet

    uint64_t manager = 0;
    if (!ext::core::ReadProcessValue(r.managerPointerAddress, manager) || manager == 0)
        return s;

    const Layout layout {};
    std::array<uint8_t, 0x400> copy {};
    if (layout.managerBytes > copy.size() || !ext::core::ReadProcessBytes(static_cast<uintptr_t>(manager), copy.data(), layout.managerBytes))
        return s;

    return ParseManager(copy.data(), layout.managerBytes, layout,
                        [](uintptr_t address, void* out, size_t bytes) { return ext::core::ReadProcessBytes(address, out, bytes); });
}
} // namespace

namespace
{
std::once_flag g_once;
Resolution g_resolution;
std::atomic<bool> g_resolved { false };
} // namespace

const Resolution& Resolve()
{
    std::call_once(g_once,
                   []
                   {
                       // An exception must not leave the once-flag unset (every draw would throw again): it becomes Inert.
                       bool threw = false;
                       g_resolution = ResolveSafely(DoResolve, &threw);
                       if (threw)
                       {
                           try
                           {
                               LOG_WARN("FFXIV character preview: switched off for this session ({})", g_resolution.reason);
                           }
                           catch (...)
                           {
                           }
                       }
                       g_resolved.store(true, std::memory_order_release);
                   });
    return g_resolution;
}

bool Ready() { return Resolve().state == AccessorState::Ready; }

const char* InertReason()
{
    // Never starts the (one-time) executable scan: the menu asks this while the feature is off.
    if (!g_resolved.load(std::memory_order_acquire))
        return "";
    return g_resolution.state == AccessorState::Inert ? g_resolution.reason.c_str() : "";
}

const Snapshot& Current(uint64_t nowMs)
{
    static const Snapshot notReady {};
    const Resolution& r = Resolve();
    if (r.state != AccessorState::Ready)
        return notReady;

    thread_local Cache cache;
    return cache.Get(nowMs, [&] { return LoadSnapshot(r); });
}

void LogTable(const char* when, const Snapshot& s)
{
    LOG_INFO("CharaView table ({}): {} of {} entries valid", when, s.ValidCount(), kEntries);
    for (size_t i = 0; i < kEntries; ++i)
    {
        const Entry& e = s.entries[i];
        if (e.Valid())
            LOG_INFO("CharaView table ({}): entry {} ({}) texture {:#x} {}x{}", when, i, EntryRole(i), e.resource, e.width,
                     e.height);
    }
}
} // namespace ext::ffxiv::chara::table
