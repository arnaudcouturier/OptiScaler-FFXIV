#pragma once
// The simulation-tick probe: the raw events it produces, where they go, and the taps the rest of the fork calls. Nothing here is a hook of its own;
// the events come from the lease of the game's tick (ext/latency/TickLease.cpp) plus three existing fork seams.
//
// The default sink is the log: TickProbeCore.h counts the events of one 10 s window and TickProbe.cpp writes its one line. A recording sink can be
// plugged in through SetEventSink instead (nothing on this branch does); it then owns the numbering of the ticks and presents itself, because there
// is one sink at a time. A sink runs on the game thread and on the present thread and must be quick and allocation-free.
//
// Every event carries the game's own frame counter (Framework.Framecounter, ext/ffxiv/resolve/AnchorsLatency.h): the tick observer reads it before
// and after the original tick, the bridge present tap reads it when the present begins and when it ends. Ticks and presents are numbered from that
// one source, and a present pairs with the tick whose frame number it carries (TickProbeCore.h). When the counter did not resolve, `frameCounter` is
// false and the window reports age and W as n/a instead of guessing.
//
// Every tap returns at once while Armed() is false, which is the state of a build with the key off, of a game build whose anchors did not resolve, of
// any executable that is not the game, and of the window between the lease being installed and the next bridge present (arming takes the lease). One
// relaxed key load, and nothing else, is what a tap costs with the key off.
//
// Times are int64 MICROSECONDS from QueryPerformanceCounter (the same clock the hints of the GPU timers use), taken inside the tap, so a tap costs
// one QPC read when it is armed and nothing when it is not.

#include <cstdint>

namespace ext::latency
{
struct IProbeSink
{
    virtual ~IProbeSink() = default;
    // A tick began on the game thread. `frameBefore` is Framecounter before the original ran; the tick's frame number is `frameBefore + 1`.
    virtual void OnTickBegin(std::int64_t qpcUs, std::uint32_t tid, std::uint32_t frameBefore, bool frameCounter) = 0;
    // The original tick returned. `frameAfter` is Framecounter after it ran and must be the tick's frame number (frameBefore + 1).
    virtual void OnTickEnd(std::int64_t qpcUs, std::uint32_t frameAfter) = 0;
    // The DX11 bridge's present: the whole native present of the game's frame. `frameAtBegin` is the frame number the present carries.
    virtual void OnPresentBegin(std::int64_t qpcUs, std::uint32_t tid, std::uint32_t frameAtBegin, bool frameCounter) = 0;
    virtual void OnPresentEnd(std::int64_t qpcUs, std::uint32_t frameAtEnd) = 0;
    // The Reflex sleep of the frame-generation present path, on the present thread.
    virtual void OnSleepBegin(std::int64_t qpcUs) = 0;
    virtual void OnSleepEnd(std::int64_t qpcUs) = 0;
    // One line about the lease itself: installed, refused with its reason, restored, or why the probe is unavailable.
    virtual void OnNote(const char* text) = 0;
};

// nullptr restores the log sink. The pointer is the caller's and must stay valid while it is set.
void SetEventSink(IProbeSink* sink);

// True while the probe records: the key is on, the game build's tick anchors are enabled, and the lease is installed. Every tap below checks it
// first.
bool Armed();

// One relaxed load of the mirrored [FramePacing] TickProbe key: the gate every per-present tap uses before it reads anything else. The mirror is
// refreshed by RefreshKeys() at each config load and at the first device hook.
bool KeyOn();

// Re-reads the live key from the config. Called at the end of every config load (ext::AddKeysLoadedHook) and once from the first device hook, so the
// per-frame paths never read the ini.
void RefreshKeys();

// The taps. OnBridgePresentBegin / End are ext::present::NativeBegin / NativeEnd (the whole DX11 bridge present);
// OnSleepScopeBegin / End are the Reflex-sleep PacingScope of the frame-generation present path; OnTickBegin / End are called by the lease
// observer around the game's own tick, with the counter value it read on each side.
void OnBridgePresentBegin();
void OnBridgePresentEnd();
void OnSleepScopeBegin();
void OnSleepScopeEnd();
void OnTickBegin(std::uint32_t frameBefore, bool frameCounter);
void OnTickEnd(std::uint32_t frameAfter);

// One line about the lease (installed, refused:<reason>, restored, unavailable:<reason>). Logged once per distinct text by the log sink.
void Note(const char* text);
} // namespace ext::latency
