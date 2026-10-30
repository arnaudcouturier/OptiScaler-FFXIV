// M3: no allocation per event. The queue used to be a std::deque of 192-byte
// records (one heap block per element on MSVC), so every Emit on the present
// thread allocated. The fix is a fixed ring of records preallocated at start:
// steady-state taps only write a slot. This test overrides global operator
// new in its own binary, arms the recorder without a worker, and emits 1000
// events: any heap allocation during the burst fails here.
#include "fgrec_json.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <vector>

static std::atomic<long long> g_news { 0 };

void* operator new(size_t n)
{
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new[](size_t n)
{
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

#include "ext/fgrec/FgRecorder.cpp"

namespace fg = ext::fgrec;
namespace ft = fgrec_test;

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static std::string ReadSource(const std::string& repo, const char* rel)
{
    const std::string path = repo + rel;
    std::string text;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
    {
        std::printf("FAIL: cannot read %s\n", path.c_str());
        ++g_failed;
        return text;
    }
    char buf[65536] = {};
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0)
        text.append(buf, n);
    fclose(f);
    return text;
}

static void RingPin(const std::string& repo)
{
    // A fixed ring, not a node/block container: no per-event heap block.
    const std::string core = ReadSource(repo, "/OptiScaler/ext/fgrec/FgRecorder.cpp");
    CHECK(core.find("std::deque<Record>") == std::string::npos);
}

static void NoHeapPerEvent()
{
    fg::State& s = fg::Global();
    s.enabled.store(true, std::memory_order_relaxed); // armed, no worker: nothing drains
    for (int i = 0; i < 10; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i), i, 0, 0, "warm");
    g_news.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i)
        fg::Emit(fg::kSleep, static_cast<uint64_t>(i + 10), i + 10, 0, 0, "burst");
    const long long news = g_news.load(std::memory_order_relaxed);
    std::printf("heap allocations over 1000 emits: %lld\n", news);
    CHECK(news == 0); // steady-state taps write a ring slot, nothing more
    s.enabled.store(false, std::memory_order_relaxed);
    s.queue.clear();
    s.next = 0;
    s.dropped = 0;
}

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    RingPin(repo);
    NoHeapPerEvent();
    if (g_failed != 0)
    {
        std::printf("fgrec_alloc: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("fgrec_alloc: all passed\n");
    return 0;
}
