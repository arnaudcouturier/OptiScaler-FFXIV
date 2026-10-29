// The bounded multi-producer ring of the developer probe (ext/hudless/PlateProbeRing.h): a full ring drops and counts, never blocks;
// several producers racing one consumer lose nothing that was accepted, duplicate nothing and keep each producer's own order.
#include "ext/hudless/PlateProbeRing.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ext::hudless::plate;

namespace
{
int g_failures = 0;
#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

Event Make(uint32_t producer, uint64_t n)
{
    Event e;
    e.thread = producer;
    e.seq = n;
    e.qpc = n;
    return e;
}

void FullAndEmpty()
{
    EventRing<8> ring;
    Event out[16];
    EXPECT(ring.Drain(out, 16) == 0, "empty ring drains nothing");
    for (uint64_t i = 0; i < 8; ++i)
        EXPECT(ring.Push(Make(1, i)), "slot %llu", (unsigned long long) i);
    EXPECT(!ring.Push(Make(1, 99)) && ring.Drops() == 1, "the ninth is dropped and counted");
    EXPECT(!ring.Push(Make(1, 100)) && ring.Drops() == 2, "again");
    EXPECT(ring.Drain(out, 3) == 3 && out[0].seq == 0 && out[2].seq == 2, "oldest first, limited by max");
    EXPECT(ring.Push(Make(1, 8)) && ring.Push(Make(1, 9)) && ring.Push(Make(1, 10)) && !ring.Push(Make(1, 11)), "room for exactly the drained ones");
    const size_t n = ring.Drain(out, 16);
    EXPECT(n == 8 && out[0].seq == 3 && out[4].seq == 7 && out[5].seq == 8 && out[7].seq == 10, "order kept across the wrap (n=%zu)", n);
    EXPECT(ring.Drops() == 3, "drops %llu", (unsigned long long) ring.Drops());
    EXPECT(EventRing<8>::Capacity() == 8, "capacity");
}

void Race()
{
    constexpr uint32_t kProducers = 4;
    constexpr uint64_t kEach = 50000;
    EventRing<1024> ring;
    std::atomic<bool> go { false };
    std::atomic<uint64_t> accepted { 0 };
    std::vector<std::thread> producers;
    for (uint32_t p = 1; p <= kProducers; ++p)
        producers.emplace_back(
            [&, p]
            {
                while (!go.load())
                    std::this_thread::yield();
                for (uint64_t n = 1; n <= kEach; ++n)
                    if (ring.Push(Make(p, n)))
                        accepted.fetch_add(1);
            });

    std::vector<uint64_t> last(kProducers + 1, 0);
    uint64_t consumed = 0;
    bool ordered = true, duplicate = false;
    auto drain = [&]
    {
        Event buffer[256];
        size_t n;
        while ((n = ring.Drain(buffer, 256)) > 0)
            for (size_t i = 0; i < n; ++i)
            {
                const Event& e = buffer[i];
                if (e.seq <= last[e.thread])
                {
                    if (e.seq == last[e.thread])
                        duplicate = true;
                    else
                        ordered = false;
                }
                last[e.thread] = e.seq;
                ++consumed;
            }
    };
    go.store(true);
    bool running = true;
    while (running)
    {
        drain();
        running = false;
        for (auto& t : producers)
            running = running || t.joinable();
        if (accepted.load() + ring.Drops() == kProducers * kEach)
            running = false;
    }
    for (auto& t : producers)
        t.join();
    drain();
    EXPECT(accepted.load() + ring.Drops() == kProducers * kEach, "every push was either accepted or counted as a drop");
    EXPECT(consumed == accepted.load(), "consumed %llu accepted %llu", (unsigned long long) consumed, (unsigned long long) accepted.load());
    EXPECT(ordered && !duplicate, "per-producer order kept, nothing twice");
}
} // namespace

int main()
{
    FullAndEmpty();
    Race();
    if (g_failures)
    {
        std::printf("FAIL: %d expectation(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: probe ring (full ring drops and counts, four producers against one consumer lose and duplicate nothing)");
    return 0;
}
