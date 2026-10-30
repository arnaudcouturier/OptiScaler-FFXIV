#pragma once
// The status.json snapshot: what the writer collects (StatusCollect.cpp) and how it is rendered (ToJson in
// StatusJson.cpp, so a throwaway check can compile the renderer alone). Everything here is plain data built from the
// status and readout accessors the menu already uses; no feature file carries status-file code. Strings that reach
// the file are valid UTF-8, capped (kDetailMax) and never hold a path: runtimes name a folder label from a closed
// set only ("game folder", "OptiScaler folder", "streamline folder", "NGX driver folder", "other folder").

#include <cstdint>
#include <string>
#include <vector>

namespace ext::status
{
// Closed vocabulary of a feature's state.
inline constexpr const char* kEngaged = "engaged"; // on and delivering
inline constexpr const char* kPending = "pending"; // on but not delivering yet (or armed and falling back)
inline constexpr const char* kOff = "off";         // switched off
inline constexpr const char* kFailed = "failed";    // on and known-broken
inline constexpr const char* kUnknown = "unknown";  // on, but the state could not be read

struct Feature
{
    const char* name = "";
    const char* state = kUnknown;
    const char* reason = "unread"; // closed per-feature vocabulary, documented in docs/notes/status-json.md
    std::string detail;            // short free text (counts, a status line), never a path
};

struct Runtime
{
    const char* name = ""; // "dlss", "dlssg", "nr", "streamline"
    bool found = false;    // a copy exists where the loader looks
    bool loaded = false;   // that copy (or one of them) is mapped into the process
    std::string version;   // file or snippet version, "" when unknown
    std::string folder;    // folder label only, "" when unknown
};

struct PerfStats
{
    uint64_t frames = 0;
    double p50Ms = 0.0, p95Ms = 0.0, p99Ms = 0.0, meanMs = 0.0, maxMs = 0.0;
};

struct PerfSeries
{
    PerfStats recent; // the last kRecentFrames intervals, exact percentiles
    PerfStats session; // whole session: exact count/mean/min/max, reservoir-sampled percentiles
};

struct Errors
{
    uint64_t count = 0;   // writer-own failures (atomic replaces, lock takes) since start
    std::string code;     // closed vocabulary ("", "write", "lock"), "" when count is 0
    std::string detail;   // operation + numeric error only, never a path
};

struct Snapshot
{
    uint64_t seq = 0;
    std::string writtenUtc; // of this write
    std::string sessionId;  // random hex, new every process
    unsigned long pid = 0;
    std::string startUtc;
    uint64_t uptimeSec = 0;
    std::string forkDescribe;
    std::string forkUpstream;
    std::string exeName; // "ffxiv_dx11.exe", never a path
    bool gameMatched = false;
    uint32_t peStamp = 0; // TimeDateStamp of the running exe's headers
    uint32_t peImageSize = 0;
    std::string buildVer; // content of ffxivgame.ver, "" when unreadable
    std::string gpuName;
    uint64_t gpuVramMB = 0;
    std::vector<Runtime> runtimes; // dlss, dlssg, nr, streamline, in that order
    std::vector<Feature> features; // gameModule, nativeQuality, companion, nr, frameGeneration, hudless, hdr
    std::string fgInput;           // closed vocabulary ("NoFG", "Upscaler", "DLSSG", ...)
    std::string fgOutput;          // closed vocabulary ("NoFG", "FSRFG", "DLSSG", ...)
    bool nrEnabled = false;
    bool nrActive = false;
    PerfSeries base;   // bridge-present cadence
    PerfSeries output; // visible-present cadence
    bool haveOutput = false;
    Errors errors;
};

// Gathers the snapshot. `seq` etc. come from the writer (StatusFile.cpp); everything else is read here from the
// accessors the menu uses. Never throws; an unreadable source keeps its field's default. `refreshIo` allows file
// and version reads (runtimes, ffxivgame.ver): false reuses the caches without touching the disk, for ticks where
// no write is due; the first call fills them.
Snapshot Collect(uint64_t seq, const std::string& sessionId, unsigned long pid, const std::string& startUtc,
                 const PerfSeries& base, const PerfSeries& output, bool haveOutput, const Errors& errors,
                 bool refreshIo);

// The change-detection key: only the state fields (features, runtimes, frame generation, errors, versions), so
// percentiles and timestamps alone never cause a write.
std::string StateKey(const Snapshot& snap);

// Renders the snapshot as UTF-8 JSON, at most kFileMax bytes and always valid: over the cap, the free-text
// details are dropped and "truncated":true is set (a minimal fixed-fields document if even that does not fit).
std::string ToJson(const Snapshot& snap);

// UTF-8 helpers shared by collection and rendering: shrink to a character boundary, and replace invalid sequences
// (raw file bytes) with U+FFFD.
void TruncateUtf8(std::string& s, size_t maxBytes);
// Only version-like text may enter a version field: non-empty, at most 64 characters, only [0-9A-Za-z._+-] with
// at least one digit (file/snippet versions). Anything else (the unlock reports "metadata pending" while discovery
// is still provisional) stays unknown, never a version.
bool IsVersionText(const std::string& s);
std::string CleanUtf8(const std::string& s);

// Deletes status.tmp-<pid>.json files whose pid is gone (a crashed writer's leftovers). Our own pid is kept.
// Best effort, called once at startup on the worker thread.
void SweepStaleTemps(const std::wstring& dir, unsigned long ownPid) noexcept;

inline constexpr size_t kFileMax = 16 * 1024;
} // namespace ext::status
