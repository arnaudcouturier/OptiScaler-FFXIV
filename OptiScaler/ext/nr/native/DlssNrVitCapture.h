#pragma once

// Safe capture of the CUDA launches of the first few NR evaluations (ini [DlssNr] VitCapture=true).
//
// Purpose: to profile a new NR model file for ViT bottleneck reuse (see DlssNrVitReuse.h and
// docs/nr-vit-reuse-profile-runbook.md) one needs the launch order and the raw parameter bytes of every launch, and the CUDA
// module itself. Nothing in the code can derive those from the names and sizes the normal log shows.
//
// The capture only observes. It never changes a launch, its parameters or its timing, and it never touches the filter's
// drop decision: the launch is recorded before that decision is made, so it works with the override off (nothing is dropped
// then anyway), with the override on, and with VitEvery=1 (the evaluation is then only watched, see Filter::BeginObserve).
//
// This header is the pure part: record formatting, budgets and the in-memory buffer. It knows nothing about NvAPI, files or
// the clock, so tests/ext/nr/nr_vit_capture.cpp exercises it on the host. DlssNrNative.cpp owns the file writes, outside any
// driver call and never from a place an exception could unwind into NVIDIA's DLL.
//
// File format (version 1), one line per record, `TAG key=value key=value ...`. Values never contain spaces (a kernel name
// the runtime did not tell us is `?`). Numbers are decimal unless they start with 0x. Hex dumps are lowercase, two digits per
// byte, no separators. Lines starting with # are comments. Order of records:
//
//   CAPTURE format=1 evaluations_max=N launches_max=N param_bytes_max=N run=YYYYMMDD-HHMMSS
//   MODULE  handle=0x.. bytes=N sha256=<64 hex> magic=<first 4 bytes hex> dump=modules/<sha256>.bin|-  status=dumped|<reason>
//   FUNC    fn=0x.. module=0x.. kernel=<cc_vit_* name> role=none|start|inner|end
//   EVAL    n=<1..N> feature=<index of the NR pass feature, in order of first use> reset=0|1 every=<VitEvery as applied>
//           override=0|1 observe_only=0|1
//   LAUNCH  eval=n ord=<0-based launch index within the evaluation, counting every launch> fn=0x.. kernel=<name>|?
//           vit=0|1 (name starts with cc_vit_) role=none|start|inner|end (what DlssNrVitReuse::RoleOf says) abi=basic|ex
//           grid=XxYxZ block=XxYxZ shared=<dynamic shared bytes> psize=<paramSize the caller passed>
//           pmode=blob|ptrs|none (blob: pParams/paramSize; ptrs: only the EX kernelParams pointer array, size unknowable and
//           never dereferenced; none) pbytes=<bytes dumped> trunc=0|1 [kp=0x..] params=<hex of the first pbytes bytes>|-
//   END     eval=n launches=<launches seen> recorded=<LAUNCH lines written> skipped=<over the cap> success=0|1
//   DONE    evaluations=N launches=N
//   NOTE    text=<no spaces>
//
// A CUDA module blob goes to modules/<sha256>.bin beside the capture file; only modules whose bytes contain the text
// "cc_vit_" are dumped (a kernel name must be in the module for the driver to find it).

#include "ext/nr/native/DlssNrVitReuse.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DlssNrVitCapture
{
constexpr unsigned kFormat = 1;
constexpr unsigned kEvaluations = 3;               // NR evaluations recorded per session
constexpr unsigned kMaxLaunchesPerEvaluation = 512; // an evaluation launched 158 kernels in the live log
constexpr size_t kMaxParamBytes = 512;             // per launch; the ViT kernels take 24..80
constexpr size_t kMaxPendingBytes = 4u << 20;      // text waiting for a flush
constexpr size_t kMaxFunctionNames = 4096;         // function handle -> name map
constexpr size_t kMaxModuleBytes = 16u << 20;      // largest blob dumped (the ViT module is 1.4 MB)
constexpr unsigned kMaxModuleDumps = 6;            // blobs dumped per session
constexpr size_t kKeepRuns = 5;                    // capture folders kept in total, newest first
constexpr std::string_view kVitPrefix = "cc_vit_";

inline std::string Hex(const void* data, size_t n)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    const auto* p = static_cast<const unsigned char*>(data);

    for (size_t i = 0; i < n; ++i)
    {
        out += digits[p[i] >> 4];
        out += digits[p[i] & 15];
    }

    return out;
}

inline std::string Handle(uint64_t value)
{
    char buf[24];
    snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(value));
    return buf;
}

inline bool IsVitKernel(std::string_view name) { return name.substr(0, kVitPrefix.size()) == kVitPrefix; }

// True when the module bytes mention a cc_vit_ kernel.
inline bool ContainsVitKernel(const void* data, size_t n)
{
    if (!data || n < kVitPrefix.size())
        return false;

    return std::string_view(static_cast<const char*>(data), n).find(kVitPrefix) != std::string_view::npos;
}

inline const char* RoleName(DlssNrVitReuse::Role role)
{
    switch (role)
    {
    case DlssNrVitReuse::Role::Start: return "start";
    case DlssNrVitReuse::Role::Inner: return "inner";
    case DlssNrVitReuse::Role::End: return "end";
    default: return "none";
    }
}

// Why a module blob is not written out, or an empty string when it should be.
inline std::string_view WhyNotDumped(size_t bytes, unsigned dumpsSoFar, bool alreadyDumped)
{
    if (alreadyDumped)
        return "duplicate";

    if (bytes == 0 || bytes > kMaxModuleBytes)
        return "too_large";

    if (dumpsSoFar >= kMaxModuleDumps)
        return "dump_limit";

    return {};
}

// ---- run folders ---------------------------------------------------------------------------------------------------

// YYYYMMDD-HHMMSS, local time of the first write of the session. Sorts chronologically as a string.
inline std::string FormatRunId(const std::tm& t)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             t.tm_sec);
    return buf;
}

inline bool IsRunId(std::string_view s)
{
    if (s.size() != 15 || s[8] != '-')
        return false;

    for (size_t i = 0; i < s.size(); ++i)
        if (i != 8 && (s[i] < '0' || s[i] > '9'))
            return false;

    return true;
}

// Of the folder names found next to a new run (the new run included), the oldest ones beyond `keep`. Names that are not
// run ids are never listed: the capture only ever deletes what it created.
inline std::vector<std::string> RunsToPrune(std::vector<std::string> names, size_t keep = kKeepRuns)
{
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !IsRunId(n); }), names.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    if (names.size() <= keep)
        return {};

    return std::vector<std::string>(names.begin(), names.end() - static_cast<std::ptrdiff_t>(keep));
}

// ---- records -------------------------------------------------------------------------------------------------------

inline std::string FormatHeader(const std::string& runId)
{
    std::string s;
    s += "# OptiScaler-FFXIV NR ViT launch capture, format " + std::to_string(kFormat) + "\n";
    s += "# One record per line: TAG key=value ... (values never contain spaces; hex is lowercase). See DlssNrVitCapture.h.\n";
    s += "# Observation only: this capture did not change any launch, parameter or timing.\n";
    s += "CAPTURE format=" + std::to_string(kFormat) + " evaluations_max=" + std::to_string(kEvaluations) +
         " launches_max=" + std::to_string(kMaxLaunchesPerEvaluation) + " param_bytes_max=" + std::to_string(kMaxParamBytes) +
         " run=" + (runId.empty() ? std::string("-") : runId) + "\n";
    return s;
}

struct ModuleNote
{
    uint64_t handle = 0;
    unsigned bytes = 0;
    std::string sha256;
    std::string magic;  // first four bytes, hex
    std::string dump;   // path relative to the capture folder, empty when not written
    std::string status; // "dumped" or the reason
};

inline std::string FormatModule(const ModuleNote& m)
{
    return "MODULE handle=" + Handle(m.handle) + " bytes=" + std::to_string(m.bytes) + " sha256=" + m.sha256 +
           " magic=" + (m.magic.empty() ? std::string("-") : m.magic) + " dump=" + (m.dump.empty() ? std::string("-") : m.dump) +
           " status=" + m.status + "\n";
}

inline std::string FormatFunc(uint64_t fn, uint64_t module, std::string_view name)
{
    const std::string n(name);
    return "FUNC fn=" + Handle(fn) + " module=" + Handle(module) + " kernel=" + n + " role=" +
           RoleName(DlssNrVitReuse::RoleOf(n.c_str())) + "\n";
}

inline std::string FormatEval(unsigned n, unsigned feature, bool reset, unsigned every, bool override, bool observeOnly)
{
    return "EVAL n=" + std::to_string(n) + " feature=" + std::to_string(feature) + " reset=" + (reset ? "1" : "0") +
           " every=" + std::to_string(every) + " override=" + (override ? "1" : "0") +
           " observe_only=" + (observeOnly ? "1" : "0") + "\n";
}

struct LaunchRecord
{
    unsigned evaluation = 0;
    unsigned ordinal = 0;
    uint64_t function = 0;
    std::string kernel; // empty = the runtime never told us
    uint32_t grid[3] = {};
    uint32_t block[3] = {};
    uint32_t shared = 0;
    uint32_t paramSize = 0;
    const void* params = nullptr;
    uint64_t kernelParams = 0; // EX pointer-array form
    bool ex = false;
};

inline std::string FormatLaunch(const LaunchRecord& r)
{
    const bool blob = r.params != nullptr && r.paramSize != 0;
    const size_t bytes = blob ? std::min<size_t>(r.paramSize, kMaxParamBytes) : 0;
    const std::string name = r.kernel.empty() ? std::string("?") : r.kernel;
    const char* mode = blob ? "blob" : (r.kernelParams != 0 ? "ptrs" : "none");

    std::string s = "LAUNCH eval=" + std::to_string(r.evaluation) + " ord=" + std::to_string(r.ordinal) +
                    " fn=" + Handle(r.function) + " kernel=" + name + " vit=" + (IsVitKernel(name) ? "1" : "0") +
                    " role=" + RoleName(DlssNrVitReuse::RoleOf(name.c_str())) + " abi=" + (r.ex ? "ex" : "basic") +
                    " grid=" + std::to_string(r.grid[0]) + "x" + std::to_string(r.grid[1]) + "x" + std::to_string(r.grid[2]) +
                    " block=" + std::to_string(r.block[0]) + "x" + std::to_string(r.block[1]) + "x" +
                    std::to_string(r.block[2]) + " shared=" + std::to_string(r.shared) + " psize=" + std::to_string(r.paramSize) +
                    " pmode=" + mode + " pbytes=" + std::to_string(bytes) + " trunc=" + (blob && r.paramSize > bytes ? "1" : "0");

    if (!blob && r.kernelParams != 0)
        s += " kp=" + Handle(r.kernelParams);

    s += " params=" + (blob ? Hex(r.params, bytes) : std::string("-")) + "\n";
    return s;
}

inline std::string FormatEnd(unsigned eval, unsigned seen, unsigned recorded, unsigned skipped, bool success)
{
    return "END eval=" + std::to_string(eval) + " launches=" + std::to_string(seen) + " recorded=" + std::to_string(recorded) +
           " skipped=" + std::to_string(skipped) + " success=" + (success ? "1" : "0") + "\n";
}

// ---- the recorder --------------------------------------------------------------------------------------------------

// Everything except SetWanted/Wanted is called under the native-runtime mutex (the same one that serializes the filter).
// Mutating members never throw: an allocation failure stops the capture and the game carries on untouched.
class Recorder
{
  public:
    // The ini setting; read from any thread (config load, every NR evaluation, the module-creation hooks).
    void SetWanted(bool on) noexcept { wanted_.store(on, std::memory_order_relaxed); }
    bool Wanted() const noexcept { return wanted_.load(std::memory_order_relaxed); }

    // Wanted, not failed, and evaluations left to record.
    bool Open() const noexcept { return Wanted() && !failed_ && evaluation_ < kEvaluations; }
    bool Capturing() const noexcept { return active_; }
    bool Done() const noexcept { return done_; }
    bool Failed() const noexcept { return failed_; }
    unsigned Evaluations() const noexcept { return evaluation_; }
    unsigned ModuleDumps() const noexcept { return dumps_; }

    // ---- before the first evaluation: modules and functions are created lazily by the runtime ----

    void NoteFunction(uint64_t fn, uint64_t module, std::string_view name) noexcept
    {
        if (!Open())
            return;

        try
        {
            if (names_.size() < kMaxFunctionNames || names_.count(fn))
                names_[fn] = std::string(name);

            if (IsVitKernel(name))
                Append(FormatFunc(fn, module, name));
        }
        catch (...)
        {
            Fail();
        }
    }

    void ForgetFunction(uint64_t fn) noexcept
    {
        try
        {
            names_.erase(fn);
        }
        catch (...)
        {
        }
    }

    bool ModuleNoted(uint64_t handle) const noexcept { return noted_.count(handle) != 0; }

    // A module dump decision's outcome. `dumpedSha` marks a blob that was actually written.
    void NoteModule(const ModuleNote& m) noexcept
    {
        try
        {
            noted_.insert(m.handle);

            if (m.status == "dumped")
            {
                ++dumps_;
                dumpedSha_.insert(m.sha256);
            }

            if (Open())
                Append(FormatModule(m));
        }
        catch (...)
        {
            Fail();
        }
    }

    bool ShaDumped(const std::string& sha) const noexcept { return dumpedSha_.count(sha) != 0; }

    // Keeps a copy of the newest module blob, so that a module whose bytes do not mention its kernel names can still be
    // written out when its first cc_vit_ function shows up (the runtime creates a lazy module and its functions together).
    void RememberModule(uint64_t handle, const void* data, size_t n) noexcept
    {
        try
        {
            if (!Open() || !data || n == 0 || n > kMaxModuleBytes)
            {
                slotHandle_ = 0;
                slot_.clear();
                return;
            }

            const auto* p = static_cast<const unsigned char*>(data);
            slot_.assign(p, p + n);
            slotHandle_ = handle;
        }
        catch (...)
        {
            slot_.clear();
            slotHandle_ = 0;
        }
    }

    const std::vector<unsigned char>* RememberedModule(uint64_t handle) const noexcept
    {
        return slotHandle_ != 0 && slotHandle_ == handle ? &slot_ : nullptr;
    }

    // ---- one NR evaluation ----

    // True when this evaluation is being recorded. `every` is the VitEvery value as applied to this pass (1 = watched only).
    bool BeginEvaluation(const void* feature, bool reset, unsigned every, bool overrideOn) noexcept
    {
        try
        {
            if (active_)
                EndEvaluation(false); // the previous one never ended

            if (!Open())
                return false;

            active_ = true;
            ++evaluation_;
            ordinal_ = 0;
            recorded_ = 0;
            skipped_ = 0;
            Append(FormatEval(evaluation_, FeatureIndex(feature), reset, every, overrideOn, every < 2));
            return active_;
        }
        catch (...)
        {
            Fail();
            return false;
        }
    }

    // Records one launch exactly as the runtime issued it. Works on the basic and the EX launch structs.
    template<class Kernel> void Observe(const Kernel& k) noexcept
    {
        if (!active_)
            return;

        try
        {
            LaunchRecord r;
            r.function = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(k.hFunction));
            r.grid[0] = k.gridDim.x; r.grid[1] = k.gridDim.y; r.grid[2] = k.gridDim.z;
            r.block[0] = k.blockDim.x; r.block[1] = k.blockDim.y; r.block[2] = k.blockDim.z;
            r.shared = k.dynSharedMemBytes;
            r.params = k.pParams;
            r.paramSize = k.paramSize;

            if constexpr (requires { k.kernelParams; })
            {
                r.ex = true;
                r.kernelParams = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(k.kernelParams));
            }

            Record(r);
        }
        catch (...)
        {
            Fail();
        }
    }

    void Record(LaunchRecord r)
    {
        if (!active_)
            return;

        r.evaluation = evaluation_;
        r.ordinal = ordinal_++;

        if (recorded_ >= kMaxLaunchesPerEvaluation)
        {
            ++skipped_;
            return;
        }

        if (r.kernel.empty())
            if (auto it = names_.find(r.function); it != names_.end())
                r.kernel = it->second;

        Append(FormatLaunch(r));
        ++recorded_;
    }

    // True when the evaluation was recorded, so the caller should flush TakePending().
    bool EndEvaluation(bool success) noexcept
    {
        if (!active_)
            return false;

        try
        {
            active_ = false;
            Append(FormatEnd(evaluation_, ordinal_, recorded_, skipped_, success));
            totalLaunches_ += ordinal_;

            if (evaluation_ >= kEvaluations && !done_)
            {
                done_ = true;
                Append("DONE evaluations=" + std::to_string(evaluation_) + " launches=" + std::to_string(totalLaunches_) + "\n");
                names_.clear();
                slot_.clear();
                slot_.shrink_to_fit();
                slotHandle_ = 0;
            }

            return true;
        }
        catch (...)
        {
            Fail();
            return true;
        }
    }

    // Text to append to the capture file. Cleared by the call.
    std::string TakePending() noexcept
    {
        std::string out;
        out.swap(pending_);
        return out;
    }

    bool HasPending() const noexcept { return !pending_.empty(); }
    bool NeedsHeader() const noexcept { return !headerTaken_; }
    void HeaderTaken() noexcept { headerTaken_ = true; }

    // Back to a fresh recorder (tests only; the product keeps one recorder for the whole session).
    void ResetForTest() noexcept
    {
        wanted_.store(false);
        failed_ = active_ = done_ = headerTaken_ = false;
        evaluation_ = ordinal_ = recorded_ = skipped_ = dumps_ = 0;
        totalLaunches_ = 0;
        pending_.clear();
        names_.clear();
        noted_.clear();
        dumpedSha_.clear();
        features_.clear();
        slot_.clear();
        slotHandle_ = 0;
    }

    // The capture stops; the game does not notice. A NOTE explains why when it can be written.
    void Fail(std::string_view why = "recorder_error") noexcept
    {
        failed_ = true;
        active_ = false;

        try
        {
            pending_ += "NOTE text=capture_stopped_" + std::string(why) + "\n";
        }
        catch (...)
        {
        }
    }

  private:
    void Append(const std::string& text)
    {
        if (pending_.size() + text.size() > kMaxPendingBytes)
        {
            Fail("buffer_cap");
            return;
        }

        pending_ += text;
    }

    unsigned FeatureIndex(const void* feature)
    {
        for (size_t i = 0; i < features_.size(); ++i)
            if (features_[i] == feature)
                return static_cast<unsigned>(i);

        features_.push_back(feature);
        return static_cast<unsigned>(features_.size() - 1);
    }

    std::atomic<bool> wanted_ { false };
    bool failed_ = false, active_ = false, done_ = false, headerTaken_ = false;
    unsigned evaluation_ = 0, ordinal_ = 0, recorded_ = 0, skipped_ = 0, dumps_ = 0;
    unsigned long long totalLaunches_ = 0;
    std::string pending_;
    std::unordered_map<uint64_t, std::string> names_;
    std::unordered_set<uint64_t> noted_;
    std::unordered_set<std::string> dumpedSha_;
    std::vector<const void*> features_;
    std::vector<unsigned char> slot_;
    uint64_t slotHandle_ = 0;
};

inline Recorder& Instance()
{
    static Recorder recorder;
    return recorder;
}
} // namespace DlssNrVitCapture
