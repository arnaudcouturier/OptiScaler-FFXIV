// [REL] ext/dx11bridge/NgxFeatureTable.h: the table that replaces `Dx11Contexts`, the closed/in-flight registry and the leaf
// lock they share. Pure CPU, std only.
//
// Lock order (documented in the header and in ADOPT-REL-LOG.md): the table lock is a LEAF. Nothing is called while it is held
// (no NGX, no drain, no FG mutex, no NR lock, no destructor of a feature); the registry has its own leaf mutex that is never
// held together with it. The tests check that with owner-tracking: the fake "NGX call", "drain" and destructors assert that the
// calling thread does not hold the table lock.
#include "ext/dx11bridge/NgxFeatureTable.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace ext::dx11bridge;
using namespace std::chrono_literals;

static std::atomic<int> g_failed { 0 };
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

// A stand-in for ContextData<IFeature_Dx11>: a unique_ptr to something with a destructor that does "driver work".
static std::atomic<int> g_destroyedUnderLock { 0 };
static std::atomic<int> g_destroyed { 0 };
struct Feature
{
    int id;
    explicit Feature(int i) : id(i) {}
    ~Feature()
    {
        ++g_destroyed;
        if (TableLockHeldByThisThread())
            ++g_destroyedUnderLock;
    }
};
struct Context
{
    std::unique_ptr<Feature> feature;
    void* createParams = nullptr;
    int changeBackendCounter = 0;
    unsigned long long lastEvalFrame = 0;
};

// Runs `body` on a thread; a deadlock shows as a test failure after 60 s instead of a hung test run.
static void WithWatchdog(const char* what, const std::function<void()>& body)
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    std::thread([body, done] {
        body();
        done->set_value();
    }).detach();
    if (fut.wait_for(60s) != std::future_status::ready)
    {
        std::printf("FAIL: %s did not finish in 60 s (deadlock?)\n", what);
        std::fflush(stdout);
        std::_Exit(2);
    }
}

// Counts heap allocations made by the thread that set the flag (the pin is on the per-frame path and must not allocate).
static thread_local bool t_countAllocs = false;
static thread_local int t_allocs = 0;
void* operator new(size_t n)
{
    if (t_countAllocs)
        ++t_allocs;
    if (void* p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static void TestPinIsAllocationFree()
{
    EvalRegistry reg;
    reg.TryEnter(31); // the first use of a handle may allocate its entry
    reg.Leave(31);
    t_allocs = 0;
    t_countAllocs = true;
    for (int i = 0; i < 1000; ++i)
    {
        EvaluatePin pin(reg, 31);
        CHECK(static_cast<bool>(pin));
    }
    t_countAllocs = false;
    CHECK(t_allocs == 0);
    CHECK(reg.InFlight(31) == 0);

    // more threads inside one handle than the registry has per-thread slots: every enter/leave still balances and the wait sees them
    EvalRegistry many;
    constexpr int kThreads = 14;
    std::atomic<int> inside { 0 };
    std::atomic<bool> go { false };
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&] {
            EvaluatePin pin(many, 77);
            ++inside;
            while (!go)
                std::this_thread::sleep_for(1ms);
        });
    while (inside < kThreads)
        std::this_thread::sleep_for(1ms);
    CHECK(many.InFlight(77) == kThreads);
    many.Close(77);
    CHECK(many.WaitIdle(77, 50ms) == Quiescence::EvaluateInFlight);
    go = true;
    for (auto& t : threads)
        t.join();
    CHECK(many.InFlight(77) == 0);
    CHECK(many.WaitIdle(77, 50ms) == Quiescence::Idle);
}

static void TestRegistry()
{
    EvalRegistry reg;
    // an open handle admits evaluates, counts them, and leaves
    CHECK(reg.TryEnter(7));
    CHECK(reg.TryEnter(7));
    CHECK(reg.InFlight(7) == 2);
    reg.Leave(7);
    reg.Leave(7);
    CHECK(reg.InFlight(7) == 0);

    // Close refuses new evaluates, others are unaffected
    reg.Close(7);
    CHECK(reg.IsClosed(7));
    CHECK(!reg.TryEnter(7));
    CHECK(reg.TryEnter(8));
    reg.Leave(8);
    // quiet handle: Idle at once
    CHECK(reg.WaitIdle(7, 50ms) == Quiescence::Idle);
    // Reopen admits again (a failed release keeps the feature alive)
    reg.Reopen(7);
    CHECK(!reg.IsClosed(7));
    CHECK(reg.TryEnter(7));
    reg.Leave(7);

    // an evaluate in flight: the wait is bounded and says so
    CHECK(reg.TryEnter(9));
    reg.Close(9);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(reg.WaitIdle(9, 80ms) == Quiescence::SelfInside); // this thread IS the evaluator
    const auto selfMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(selfMs < 60); // self-inside is answered without waiting for the timeout
    reg.Leave(9);

    // another thread inside: the wait times out; once it leaves, the wait succeeds
    reg.Reopen(9);
    std::promise<void> entered, leave;
    auto leaveFut = leave.get_future();
    std::thread evaluator([&] {
        const bool ok = reg.TryEnter(9);
        CHECK(ok);
        entered.set_value();
        leaveFut.wait();
        reg.Leave(9);
    });
    entered.get_future().wait();
    reg.Close(9);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(reg.WaitIdle(9, 80ms) == Quiescence::EvaluateInFlight);
    CHECK(std::chrono::steady_clock::now() - t1 >= 70ms);
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);
        leave.set_value();
    });
    CHECK(reg.WaitIdle(9, 5s) == Quiescence::Idle);
    evaluator.join();
    releaser.join();

    // Forget drops the closed state; a late Leave of a forgotten handle is harmless
    reg.Close(11);
    reg.Forget(11);
    CHECK(!reg.IsClosed(11));
    reg.Leave(11);
    CHECK(reg.InFlight(11) == 0);
    CHECK(reg.TryEnter(11));
    reg.Leave(11);

    // the RAII pin
    {
        EvaluatePin pin(reg, 12);
        CHECK(static_cast<bool>(pin));
        CHECK(reg.InFlight(12) == 1);
        reg.Close(12);
        EvaluatePin refused(reg, 12);
        CHECK(!static_cast<bool>(refused));
    }
    CHECK(reg.InFlight(12) == 0);
}

static void TestTable()
{
    EvalRegistry reg;
    ContextTable<Context> table(reg);

    // operator[] inserts a default entry, references stay valid while other handles come and go
    Context& first = table[1000000];
    first.feature = std::make_unique<Feature>(1);
    Feature* const firstFeature = first.feature.get();
    for (unsigned id = 1000001; id < 1010000; ++id)
        table[id].feature = std::make_unique<Feature>(static_cast<int>(id));
    CHECK(&table[1000000] == &first);
    CHECK(first.feature.get() == firstFeature);
    for (unsigned id = 1000001; id < 1010000; id += 2)
        table.Extract(id); // erase a lot of others
    CHECK(&table[1000000] == &first);
    CHECK(table.Size() == 5000); // 10000 inserted, the 5000 odd ids extracted

    // Extract hands the whole entry out, removes it, forgets the handle, and destroys nothing under the lock
    reg.Close(1000002);
    g_destroyed = 0;
    g_destroyedUnderLock = 0;
    {
        Context detached = table.Extract(1000002);
        CHECK(detached.feature != nullptr && detached.feature->id == 1000002);
        CHECK(!reg.IsClosed(1000002)); // forgotten
        CHECK(g_destroyed == 0);
    }
    CHECK(g_destroyed == 1);
    CHECK(g_destroyedUnderLock == 0); // the destructor ran after the lock was released
    // Extract of a missing handle is an empty entry, not an insert
    {
        const auto before = table.Size();
        Context none = table.Extract(5);
        CHECK(none.feature == nullptr);
        CHECK(table.Size() == before);
    }

    // the locked view holds the lock for the loop and sees every entry once
    {
        size_t seen = 0;
        bool heldInside = false;
        for (const auto& [id, context] : table.Locked())
        {
            (void) id;
            (void) context;
            heldInside = TableLockHeldByThisThread();
            ++seen;
        }
        CHECK(heldInside);
        CHECK(seen == table.Size());
        CHECK(!TableLockHeldByThisThread());
    }
}

// Many threads create, evaluate and release disjoint and shared handles; no deadlock, nothing destroyed under the lock,
// no evaluate runs on a feature after its release finished, and every drain/NGX stand-in runs without the lock.
static void TestStress()
{
    EvalRegistry reg;
    ContextTable<Context> table(reg);
    std::mutex nrLock; // stands for the NR state lock: taken inside "NGX calls", never while holding the table lock
    std::atomic<int> lockHeldDuringCall { 0 };
    std::atomic<int> evaluatedAfterRelease { 0 };
    std::atomic<int> orderViolations { 0 };
    g_destroyedUnderLock = 0;

    auto ngxCall = [&] {
        if (TableLockHeldByThisThread())
            ++lockHeldDuringCall;
        std::lock_guard<std::mutex> nr(nrLock); // "NR runs inside the NGX call"
        std::this_thread::yield();
    };

    constexpr int kThreads = 8, kRounds = 400;
    WithWatchdog("table stress", [&] {
        std::vector<std::thread> threads;
        std::atomic<unsigned> nextId { 1000000 };
        std::atomic<bool> stop { false };
        std::vector<std::atomic<bool>> alive(kThreads * kRounds + 16);
        // creators / releasers: each owns its handles
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back([&, t] {
                for (int r = 0; r < kRounds; ++r)
                {
                    const unsigned id = nextId.fetch_add(1);
                    // create: table op, then the "driver call"
                    table[id].feature = std::make_unique<Feature>(static_cast<int>(id));
                    alive[id - 1000000].store(true);
                    ngxCall();
                    // release: close, wait for evaluators, drain (a driver call), detach, destroy
                    reg.Close(id);
                    const auto q = reg.WaitIdle(id, 5s);
                    if (q != Quiescence::Idle)
                        ++orderViolations;
                    ngxCall(); // the drain
                    alive[id - 1000000] = false;
                    {
                        Context detached = table.Extract(id);
                        (void) detached;
                    }
                }
            });
        }
        // evaluators: pick any handle; evaluating a released one must be refused or find an empty entry
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back([&, t] {
                unsigned seed = 12345u + static_cast<unsigned>(t);
                while (!stop.load())
                {
                    seed = seed * 1664525u + 1013904223u;
                    const unsigned top = nextId.load();
                    if (top <= 1000000)
                        continue;
                    const unsigned id = 1000000 + (seed >> 8) % (top - 1000000);
                    EvaluatePin pin(reg, id);
                    if (!pin)
                        continue;
                    // alive is published after the feature exists and cleared only after the release's wait for evaluators,
                    // so a pinned evaluator that sees it set may use the feature until it leaves
                    if (alive[id - 1000000].load())
                    {
                        Context& c = table[id];
                        if (c.feature == nullptr)
                            ++evaluatedAfterRelease;
                        ngxCall();
                        if (!alive[id - 1000000].load() || c.feature == nullptr)
                            ++evaluatedAfterRelease; // the release finished while this evaluate was running
                    }
                    for (const auto& [i, ctx] : table.Locked())
                    {
                        (void) i;
                        (void) ctx.lastEvalFrame;
                        break; // one look is enough: the point is the lock, not the data
                    }
                }
            });
        }
        for (int t = 0; t < kThreads; ++t)
            threads[static_cast<size_t>(t)].join();
        stop = true;
        for (size_t t = kThreads; t < threads.size(); ++t)
            threads[t].join();
    });

    CHECK(lockHeldDuringCall == 0);
    CHECK(g_destroyedUnderLock == 0);
    CHECK(evaluatedAfterRelease == 0);
    CHECK(orderViolations == 0);
}

// A thread that holds the (fake) NR lock may touch the table (NR -> table is the allowed direction); nothing takes the NR lock
// while holding the table lock. Run both directions concurrently: no deadlock.
static void TestLockOrder()
{
    EvalRegistry reg;
    ContextTable<Context> table(reg);
    std::recursive_mutex nr;
    WithWatchdog("lock order", [&] {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
        {
            threads.emplace_back([&, t] { // render side: NR lock, then a table operation
                for (int i = 0; i < 2000; ++i)
                {
                    std::lock_guard<std::recursive_mutex> g(nr);
                    table[static_cast<unsigned>(1000 + t)].changeBackendCounter = i;
                    for (const auto& [id, c] : table.Locked())
                    {
                        (void) id;
                        (void) c;
                    }
                }
            });
            threads.emplace_back([&, t] { // update side: table operations, NR lock only after they returned
                for (int i = 0; i < 2000; ++i)
                {
                    Context c = table.Extract(static_cast<unsigned>(1000 + t));
                    (void) c;
                    CHECK(!TableLockHeldByThisThread());
                    std::lock_guard<std::recursive_mutex> g(nr);
                }
            });
        }
        for (auto& th : threads)
            th.join();
    });
}

int main()
{
    TestRegistry();
    TestPinIsAllocationFree();
    TestTable();
    TestStress();
    TestLockOrder();
    if (g_failed == 0)
        std::printf("ngx_feature_table: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
