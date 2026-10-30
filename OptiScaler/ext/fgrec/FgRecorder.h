#pragma once
// Frame-generation event recorder (`OptiScaler/ext/fgrec/`): a bounded queue and a writer thread producing one JSONL file per session,
// beside the DLL in `fg-recordings\`. The recording core is ported from SirenBrink/FFXIVRender's fg_diagnostics
// (GPL-3.0-or-later); the records are smaller here (fixed small fields, no big arrays), rotation keeps 9 files
// per session (144 MiB plus the queue batch that crossed each file's limit, about 40 minutes at 60 base fps and 2x frame
// generation, so a 25-minute session keeps its beginning; only the base file opens with `session.begin`; rotations
// open with a `continuation` line), and only the newest 5 sessions are kept. No record holds a path, a user name or a
// machine name: a module is named by file name only.
//
// Cost: with the key off a tap costs one relaxed atomic load of the cached key plus one thread-local counter
// increment, and every 16th tap per thread adds one config re-read; there is no thread, no allocation and no
// file. Callers only enqueue; disk I/O is on the writer, which drains about every 100 ms and asks the device
// to flush at most about once a second. Queue overflow is an explicit `records_dropped` record, never silent
// loss. A `session.end` record marks a clean stop; a recording without it is unfinished.
// Shutdown signals the worker and never waits (the worker may need the loader lock this detach holds); when
// the writer is already gone it writes the queued records and `session.end` synchronously instead, without
// allocating (that path runs inside DllMain).

#include <cstdint>

namespace ext::fgrec
{
// Provider ids, stable in the file (the analyzer maps them back to names).
inline constexpr unsigned kProvNone = 0;
inline constexpr unsigned kProvDlssG = 1;
inline constexpr unsigned kProvXeFg = 2;
inline constexpr unsigned kProvFsr = 3;

// Event kinds, a closed vocabulary (the analyzer only understands these).
inline constexpr const char* kSessionBegin = "session.begin";
inline constexpr const char* kSessionEnd = "session.end";
inline constexpr const char* kContinuation = "continuation";
inline constexpr const char* kDropped = "records_dropped";
inline constexpr const char* kFgPresentBegin = "fg.present.begin"; // a = sync interval, b = flags
inline constexpr const char* kFgPresentEnd = "fg.present.end";     // a = HRESULT, b = duration in QPC ticks
inline constexpr const char* kOutPresentBegin = "out.present.begin"; // a = sync interval, b = flags
inline constexpr const char* kOutPresentEnd = "out.present.end";     // a = HRESULT, b = duration in QPC ticks
inline constexpr const char* kHiddenPresent = "hidden.present";      // a = HRESULT, b = duration in QPC ticks
inline constexpr const char* kSleep = "sleep";                       // a = duration in QPC ticks, msg = what slept
inline constexpr const char* kAutoMfg = "automfg";                   // a = warn (0/1), msg = the log line
inline constexpr const char* kNrRebuild = "nr.rebuild";              // an NR model rebuild hitched its frame
inline constexpr const char* kProviderOn = "provider.activated";     // msg = provider label
inline constexpr const char* kProviderOff = "provider.deactivating"; // msg = provider label
inline constexpr const char* kPresetSwitch = "dlssg.preset.switch";  // msg = "A -> B"
inline constexpr const char* kModule = "provider.module";            // msg = module file name only
inline constexpr const char* kXeFactor = "xefg.factor";              // a = from, b = to
inline constexpr const char* kMark = "mark";                        // a = the user's mark number, see Mark()

// One user mark: the recorder counts the presses of the mark key ([FramePacing] FgRecorderMarkKey) and writes a `mark` record, so
// a session can be read at the moments the user pointed at ("now"). Returns the number the record carries, for the caller's one
// log line; with the recorder's key off nothing is written and the count still goes up (the number is the user's, not the file's).
unsigned Mark() noexcept;

// What session.begin carries. All strings are short file content (version, names, key states): never a path.
// moduleDir is local only: the folder is <moduleDir>\fg-recordings\, the directory itself is never emitted.
struct SessionInfo
{
    const char* version = "";
    const char* exe = ""; // exe file name only
    const char* build = ""; // game build label, "" when unknown
    const char* summary = ""; // settings summary, "" when none
    const wchar_t* moduleDir = nullptr;
};

// Starts the worker when keyOn is true and it is not running (a new session, after the old-session sweep);
// when keyOn is false it only arms the worker to stop after it drains (the worker writes session.end and
// exits by itself, so stopping never waits). May be called on any thread, including the present thread.
void EnsureStarted(bool keyOn, const SessionInfo& info) noexcept;

// Clears the sticky start failure when the key arms, so the first tap of a new key session retries once even
// when the previous failure landed after the key had gone off (that key-off clear had already run). Called by
// the key poll once per key-on, never on a tap: the tap-side check stays one relaxed load.
void RearmKey() noexcept;

// Generation context, read from the live process by the seams: the active provider id and a generation
// counter bumped on every provider activation (records after a switch carry the new generation).
void NoteProvider(unsigned provider) noexcept;
void NoteGeneration() noexcept;

// One event. With the key off this is a single relaxed atomic load. Times, thread and provider/generation
// context are filled in here; a/b/c carry small integers (HRESULT, flags, durations in QPC ticks, factors).
// msg is truncated to 159 characters and escaped so every line stays strict JSON.
void Emit(const char* kind, uint64_t frame, int64_t a, int64_t b, int64_t c, const char* msg) noexcept;

// True while the worker runs (the menu status line). Never blocks.
bool Running() noexcept;
// Worker liveness hint for the tap fast path: one relaxed load, never a lock.
bool Alive() noexcept;
// True while the sticky start failure is latched (a key-off, or the next key-on through RearmKey, clears it).
// One relaxed load, never a lock.
bool Failed() noexcept;
// Sticky writer error, 0 = none (1 = the folder or file could not be opened, 2 = a write failed,
// 3 = the module could not be pinned so nothing started, 4 = the worker thread failed to start,
// 5 = the recordings path is too long, 6 = the writer faulted, 7 = the module folder is unknown).
// Never blocks; once set it stays until the key goes off and on again.
unsigned Error() noexcept;

// DLL detach (both FreeLibrary and process exit): signal the worker, delete nothing, wait for nothing.
// A live worker drains, writes session.end and exits on its own; when it already exited, the queued
// records and session.end are written synchronously through the open handle instead (bounded, no waiting).
// The module is pinned while the worker runs, so a late worker still runs mapped code.
void Shutdown(bool freeLibrary) noexcept;
} // namespace ext::fgrec
