#pragma once
// Shared fakes of the jitter tests: a fake process (regions of bytes), the game's camera jitter object laid out the way JitterBuild
// describes it, and the object of a game that applies jitter scaled by s.
#include "ext/ffxiv/jitter/FfxivJitterProbe.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

namespace jitter_fakes
{
using namespace ext::ffxiv;
using namespace ext::ffxiv::jitter;

constexpr uintptr_t kBase = 0x7FF600000000ull;
constexpr uint64_t kGlobal = 0x1000000000ull, kCamera = 0x2000000000ull, kObject = 0x3000000000ull;

// A fake process: regions of bytes keyed by start address. A read succeeds only when it lies inside one region.
struct FakeMemory
{
    std::map<uint64_t, std::vector<unsigned char>> regions;
    int reads = 0;
    std::function<void(FakeMemory&)> beforeRead; // lets a test change memory between reads

    void Put(uint64_t address, const void* data, size_t size)
    {
        auto& r = regions[address];
        r.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
    }
    void PutU64(uint64_t address, uint64_t v) { Put(address, &v, sizeof(v)); }
    bool Read(uint64_t address, void* out, size_t size)
    {
        ++reads;
        if (beforeRead)
            beforeRead(*this);
        for (auto& [start, bytes] : regions)
            if (address >= start && address + size <= start + bytes.size())
            {
                std::memcpy(out, bytes.data() + (address - start), size);
                return true;
            }
        return false;
    }
    auto Reader()
    {
        return [this](uintptr_t a, void* out, size_t n) { return Read(a, out, n); };
    }
};

void PutObject(FakeMemory& m, const RawObject& o)
{
    unsigned char bytes[20];
    std::memcpy(bytes + 0, &o.jx, 4);
    std::memcpy(bytes + 4, &o.jy, 4);
    std::memcpy(bytes + 8, &o.ndcX, 4);
    std::memcpy(bytes + 12, &o.ndcY, 4);
    std::memcpy(bytes + 16, &o.counter, 4);
    m.Put(kObject, bytes, sizeof(bytes));
}

// The object of a game that applies jitter scaled by s: ndcX = s * jx / W, ndcY = -s * jy / H.
RawObject ObjectFor(float jx, float jy, float s, float w, float h, int counter = 100)
{
    return { jx, jy, s * jx / w, -s * jy / h, counter };
}

FakeMemory Image(const RawObject& o)
{
    const JitterBuild& b = kCurrentBuild.jitter;
    FakeMemory m;
    m.PutU64(kBase + b.cameraGlobalRva, kGlobal);
    m.PutU64(kGlobal + b.cameraOffset, kCamera);
    m.PutU64(kCamera + b.jitterPointerOffset, kObject);
    PutObject(m, o);
    return m;
}

Guard Read(FakeMemory& m, RawObject& out, const GameBuild* build = &kCurrentBuild)
{
    return ReadChain(m.Reader(), build, kBase, out);
}
} // namespace jitter_fakes

