#pragma once
// The fake engine environment shared by the jitter engine tests (jitter_glue.cpp, jitter_engine.cpp): a stateful parameter block (a Set
// changes what later Gets return), fake process memory, a clock and a log, plus the game's side of one evaluate.
#include "fake_game.h"
#include "ext/ffxiv/jitter/FfxivJitterCore.h"

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jitter_fakes
{
inline bool Near(float a, double b, double tol = 1e-5) { return std::fabs(a - b) <= tol; }
inline bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

struct Op
{
    bool set = false; // false = Get
    Param param = Param::JitterX;
    float value = 0.0f; // the value written (Set) or returned (float Get)
};

// The fake environment: a stateful parameter block (a Set changes what later Gets return), fake process memory, a clock and a log.
struct FakeEnv
{
    FakeMemory mem;
    const GameBuild* build = &kCurrentBuild;
    std::map<Param, float> floats;
    std::map<Param, std::uint32_t> uints;
    std::optional<int> perfQuality = 2;
    ForcedQualityAnswer answer;
    std::uint64_t now = 0;
    std::optional<bool> frameGeneration = false;
    bool dropWrites = false;   // a block that does not take the write (the stored value stays)
    bool eraseOnWrite = false; // a block that loses the entry when it is written (a later read finds nothing)

    std::vector<Op> ops;
    std::vector<std::string> logs;
    std::string frameGenerationInput = "Upscaler"; // State::activeFgInput as text ("" = could not be told)
    int locks = 0, buildLookups = 0, clockReads = 0, qualityQueries = 0, fgQueries = 0, setsEver = 0;
    int fgInputQueries = 0;

    struct Held
    {
        explicit Held(FakeEnv& e) { ++e.locks; }
    };
    Held Lock() { return Held(*this); }
    const GameBuild* Build()
    {
        ++buildLookups;
        return build;
    }
    std::uintptr_t ModuleBase() { return kBase; }
    bool ReadMemory(std::uintptr_t address, void* out, std::size_t bytes) { return mem.Read(address, out, bytes); }

    std::optional<float> GetFloat(Param p)
    {
        const auto it = floats.find(p);
        ops.push_back({ false, p, it == floats.end() ? 0.0f : it->second });
        if (it == floats.end())
            return std::nullopt;
        return it->second;
    }
    std::uint32_t GetUInt(Param p)
    {
        ops.push_back({ false, p, 0.0f });
        const auto it = uints.find(p);
        return it == uints.end() ? 0u : it->second;
    }
    std::optional<int> GetInt(Param p)
    {
        ops.push_back({ false, p, 0.0f });
        return p == Param::PerfQuality ? perfQuality : std::nullopt; // only the quality parameter is an int
    }
    void SetFloat(Param p, float v)
    {
        ops.push_back({ true, p, v });
        ++setsEver;
        if (eraseOnWrite)
            floats.erase(p);
        else if (!dropWrites)
            floats[p] = v;
    }
    ForcedQualityAnswer ForcedQuality()
    {
        ++qualityQueries;
        return answer;
    }
    std::uint64_t NowMs()
    {
        ++clockReads;
        return now;
    }
    void Log(const std::string& line) { logs.push_back(line); }
    std::optional<bool> FrameGenerationActive()
    {
        ++fgQueries;
        return frameGeneration;
    }
    std::string FrameGenerationInput()
    {
        ++fgInputQueries;
        return frameGenerationInput;
    }

    std::size_t Gets() const
    {
        std::size_t n = 0;
        for (const Op& o : ops)
            n += o.set ? 0 : 1;
        return n;
    }
    std::vector<Op> Sets() const
    {
        std::vector<Op> out;
        for (const Op& o : ops)
            if (o.set)
                out.push_back(o);
        return out;
    }
};

// A game at 1920x1080 whose camera object is wired like the game's: memory chain plus a parameter block as the bridge hands it over.
inline FakeEnv MakeEnv()
{
    FakeEnv e;
    e.mem = Image(ObjectFor(0.3f, -0.2f, 1.0f, 1920, 1080, 100));
    e.uints[Param::SubrectWidth] = 1920;
    e.uints[Param::SubrectHeight] = 1080;
    e.uints[Param::Width] = 1920;
    e.uints[Param::Height] = 1080;
    e.uints[Param::OutWidth] = 3840;
    e.uints[Param::OutHeight] = 2160;
    return e;
}

// The game's side of one evaluate, as in the game: its camera object in memory (a game that applies jitter scaled by `s`) and the
// UNSCALED jitter it sets in the parameter block. `ngxJx/ngxJy` override what the block carries (another frame, say).
struct Frame
{
    float jx = 0.3f, jy = -0.2f, s = 1.0f;
    int counter = 100;
    std::uint64_t timeMs = 0;
    std::optional<float> ngxJx, ngxJy;
    bool ngxMissing = false; // the block has no Jitter.Offset at all
    bool ngxMissingX = false, ngxMissingY = false; // only one of the two Jitter.Offset entries is absent
    std::optional<std::pair<float, float>> ndcFrom; // the object's NDC values are those of THIS jitter (another frame's) instead of jx/jy
    bool flipNdcY = false;  // the object's ndcY has the opposite sign (a convention the correction does not understand)
};

struct Outcome
{
    std::vector<Op> ops;
    std::vector<std::string> logs;
    std::vector<Op> sets;
};

inline Outcome Evaluate(FakeEnv& e, Engine& engine, const Frame& f, bool probe, bool correct)
{
    RawObject object = ObjectFor(f.jx, f.jy, f.s, 1920, 1080, f.counter);
    if (f.ndcFrom)
    {
        const RawObject other = ObjectFor(f.ndcFrom->first, f.ndcFrom->second, f.s, 1920, 1080, f.counter);
        object.ndcX = other.ndcX;
        object.ndcY = other.ndcY;
    }
    if (f.flipNdcY)
        object.ndcY = -object.ndcY;
    PutObject(e.mem, object);
    e.floats[Param::JitterX] = f.ngxJx.value_or(f.jx);
    e.floats[Param::JitterY] = f.ngxJy.value_or(f.jy);
    if (f.ngxMissing)
    {
        e.floats.erase(Param::JitterX);
        e.floats.erase(Param::JitterY);
    }
    if (f.ngxMissingX)
        e.floats.erase(Param::JitterX);
    if (f.ngxMissingY)
        e.floats.erase(Param::JitterY);
    e.now = f.timeMs;
    e.ops.clear();
    const std::size_t logsBefore = e.logs.size();
    engine.Step(e, probe, correct);
    Outcome o;
    o.ops = e.ops;
    o.logs.assign(e.logs.begin() + static_cast<std::ptrdiff_t>(logsBefore), e.logs.end());
    o.sets = e.Sets();
    return o;
}

// Runs a script of frames with the correction on and says, per frame, whether the values were replaced.
inline std::vector<bool> Replaced(FakeEnv& e, Engine& engine, const std::vector<Frame>& frames)
{
    std::vector<bool> out;
    for (const Frame& f : frames)
        out.push_back(!Evaluate(e, engine, f, false, true).sets.empty());
    return out;
}

inline Frame At(int counter, float s, std::uint64_t timeMs, float jx = 0.3f, float jy = -0.2f)
{
    Frame f;
    f.counter = counter;
    f.s = s;
    f.timeMs = timeMs;
    f.jx = jx;
    f.jy = jy;
    return f;
}

inline int CountLines(const std::vector<std::string>& logs, const char* part)
{
    int n = 0;
    for (const std::string& line : logs)
        n += Has(line, part) ? 1 : 0;
    return n;
}
} // namespace jitter_fakes
