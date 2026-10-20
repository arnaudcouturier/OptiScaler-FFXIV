// Host check of DlssNrVitReuse.h: which launches of one NR evaluation are dropped when the ViT bottleneck is reused. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_vit_reuse_smoke.cpp
#include "ext/nr/native/DlssNrVitReuse.h"

#include <cstdio>
#include <thread>
#include <vector>

using namespace DlssNrVitReuse;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

using Seq = std::vector<Role>;
// what one evaluation launches: other work, the ViT range, other work
static const Seq kEval = { Role::None, Role::Start, Role::Inner, Role::Inner, Role::Inner, Role::End, Role::None };

struct Result
{
    std::vector<bool> dropped;
    bool wellFormed = true;
};

static Result Eval(Filter& f, const void* feature, bool reset, unsigned every, const Seq& seq = kEval)
{
    Result r;
    f.Begin(feature, reset, every);
    for (Role role : seq)
        r.dropped.push_back(f.Drop(role));
    r.wellFormed = f.End();
    return r;
}

static bool Dropped(const Result& r) // the ViT range was dropped, everything else kept
{
    return r.dropped == std::vector<bool> { false, true, true, true, true, true, false };
}

static bool Kept(const Result& r)
{
    for (bool d : r.dropped)
        if (d)
            return false;
    return true;
}

int main()
{
    int a = 0, b = 0;

    // kernel names of both variants (fp8 chained and the plain fp16 ones) map to the range roles
    CHECK(RoleOf("cc_vit_1d_repack_2d_to_1d_fp8") == Role::Start);
    CHECK(RoleOf("cc_vit_1d_repack_2d_to_1d") == Role::Start);
    CHECK(RoleOf("cc_vit_1d_repack_1d_to_2d_fp8") == Role::End);
    CHECK(RoleOf("cc_vit_1d_repack_1d_to_2d") == Role::End);
    CHECK(RoleOf("cc_vit_1d_ffn_expand_publish_fp8") == Role::Inner);
    CHECK(RoleOf("cc_vit_1d_projection_wait_fp8") == Role::Inner);
    CHECK(RoleOf("cc_cb_clear") == Role::None);
    CHECK(RoleOf("cc_vit_ffn_expand") == Role::None); // the 2D ViT kernels are not the bottleneck range
    CHECK(RoleOf(nullptr) == Role::None);

    { // outside an evaluation nothing is touched, whatever the setting
        Filter f;
        CHECK(!f.Evaluating());
        CHECK(!f.Drop(Role::Start) && !f.Drop(Role::Inner) && !f.Drop(Role::End));
    }

    { // N = 1 never drops but keeps the cache warm
        Filter f;
        for (int i = 0; i < 4; ++i)
            CHECK(Kept(Eval(f, &a, false, 1)));
        CHECK(f.Computed() == 4 && f.Reused() == 0);
        CHECK(Dropped(Eval(f, &a, false, 2))); // raising N takes effect on the next evaluation, no warm-up
    }

    { // N = 2: full, reuse, full, reuse
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(f.Computed() == 2 && f.Reused() == 2 && !f.Disabled());
    }

    { // N = 3: full, reuse, reuse, full
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
    }

    { // lowering N mid-cycle computes at once
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 3)));
        CHECK(Dropped(Eval(f, &a, false, 3)));
        CHECK(Kept(Eval(f, &a, false, 1)));
        CHECK(Kept(Eval(f, &a, false, 1)));
    }

    { // a reset always computes in full, and restarts the cycle
        Filter f;
        Eval(f, &a, false, 2);
        CHECK(Kept(Eval(f, &a, true, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
    }

    { // each feature (each pass) has its own cache and cycle
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &b, false, 2))); // b has never been computed
        CHECK(Dropped(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &b, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // destroying the modules invalidates every cache
        Filter f;
        Eval(f, &a, false, 2);
        f.Clear();
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Dropped(Eval(f, &a, false, 2)));
    }

    { // a range that is never closed: the feature turns itself off
        Filter f;
        Eval(f, &a, false, 2);
        auto r = Eval(f, &a, false, 2, { Role::None, Role::Start, Role::Inner });
        CHECK(!r.wellFormed && f.Disabled());
        CHECK(Kept(Eval(f, &a, false, 2)));
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // an unclosed full range is not a valid cache
        Filter f;
        f.Begin(&a, false, 2);
        f.Drop(Role::Start);
        CHECK(!f.End());
        f.Clear();
    }

    { // a foreign launch inside a skipped range is kept, the rest of the range is still dropped, then the feature is off
        Filter f;
        Eval(f, &a, false, 2);
        auto r = Eval(f, &a, false, 2, { Role::Start, Role::Inner, Role::None, Role::Inner, Role::End });
        CHECK((r.dropped == std::vector<bool> { true, true, false, true, true }));
        CHECK(!r.wellFormed && f.Disabled());
        CHECK(Kept(Eval(f, &a, false, 2)));
    }

    { // End without Start
        Filter f;
        auto r = Eval(f, &a, false, 2, { Role::None, Role::End });
        CHECK((r.dropped == std::vector<bool> { false, false }) && !r.wellFormed && f.Disabled());
    }

    { // a second Start inside a range
        Filter f;
        auto r = Eval(f, &a, false, 2, { Role::Start, Role::Start, Role::End });
        CHECK(!r.wellFormed && f.Disabled());
    }

    { // a watched evaluation (capture mode with VitEvery=1) never drops, and leaves the cache, the cycle and the counters alone
        Filter f;
        CHECK(Kept(Eval(f, &a, false, 2))); // warm: the next full evaluation would reuse
        f.BeginObserve(&a);
        CHECK(f.Evaluating() && f.ObserveOnly());
        for (int pass = 0; pass < 3; ++pass)
            for (Role role : kEval)
                CHECK(!f.Drop(role));
        CHECK(f.End());
        CHECK(!f.Evaluating() && !f.ObserveOnly());
        CHECK(f.Computed() == 1 && f.Reused() == 0);
        CHECK(Dropped(Eval(f, &a, false, 2))); // still warm, still on the same cycle
        f.BeginObserve(&a); // an odd order while watching is not an anomaly: the feature stays enabled
        f.Drop(Role::End);
        f.Drop(Role::Start);
        f.Drop(Role::Start);
        CHECK(f.End() && !f.Disabled());
    }

    { // the evaluation context belongs to its thread: a launch from another thread is not part of it
        Filter f;
        Eval(f, &a, false, 2);
        f.Begin(&a, false, 2);
        bool other = true;
        std::thread([&] { other = f.Evaluating() || f.Drop(Role::Start); }).join();
        CHECK(!other);
        f.End();
    }

    { // A scene transition may retire one module while another NR module stays live.
        RoleRegistry<int> roles;
        roles.Add(11, 1, Role::Start);
        roles.Add(12, 1, Role::End);
        roles.Add(21, 2, Role::Start);
        roles.Add(22, 2, Role::End);
        roles.RemoveModule(99); // unrelated DLSS module
        CHECK(roles.Lookup(11) == Role::Start);
        roles.RemoveModule(1);
        CHECK(roles.Lookup(11) == Role::None && roles.Lookup(12) == Role::None);
        Filter f;
        for (int frame = 0; frame < 2; ++frame) {
            f.Begin(&a, false, 2);
            CHECK(f.Drop(roles.Lookup(21)) == (frame == 1));
            CHECK(f.Drop(roles.Lookup(22)) == (frame == 1));
            CHECK(f.End());
        }
        CHECK(f.Computed() == 1 && f.Reused() == 1);
        roles.RemoveFunction(21);
        CHECK(roles.Lookup(21) == Role::None && roles.Lookup(22) == Role::End);
    }

    { // the NR runtime was switched (Reset): cached results and the "unexpected launch order" verdict of the old module do not carry over
        Filter f;
        Eval(f, &a, false, 2);
        CHECK(Dropped(Eval(f, &a, false, 2))); // warm
        f.Reset();
        CHECK(Kept(Eval(f, &a, false, 2)));    // the cache is gone: a full computation again
        CHECK(Dropped(Eval(f, &a, false, 2))); // and reuse works for the new module
        auto bad = Eval(f, &a, false, 2, { Role::None, Role::End });
        CHECK(!bad.wellFormed && f.Disabled());
        CHECK(Kept(Eval(f, &a, false, 2)));    // a disabled filter keeps everything...
        f.Reset();
        CHECK(!f.Disabled());                  // ...until the next runtime is judged on its own
        Eval(f, &a, false, 2);
        CHECK(Dropped(Eval(f, &a, false, 2)));
    }

    printf(fails ? "nr_vit_reuse_smoke: %d FAILED\n" : "nr_vit_reuse_smoke: all passed\n", fails);
    return fails != 0;
}
