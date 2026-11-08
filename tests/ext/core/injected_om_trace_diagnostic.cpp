#include "ext/core/InjectedOmTraceDiagnostic.h"

#include <cassert>
#include <stdexcept>
#include <thread>

int main()
{
    using namespace ext::core::om_trace;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x1234));
    enabled.store(false);
    const auto before = passes.load();
    { PassScope disabled(context); Record(Api::Rtv, Decision::Enter, context, 1); }
    assert(passes.load() == before);
    assert(current.load() == nullptr);

    enabled.store(true);
    { PassScope invalid(nullptr); assert(invalid.Get() == nullptr); }
    assert(passes.load() == before);
    try
    {
        PassScope pass(context);
        assert(pass.Get() == &captures[0]);
        Record(Api::Rtv, Decision::Enter, context, 1);
        { PassScope nested(context); assert(nested.Get() == nullptr); Record(Api::Rtv, Decision::Keep, context, kKeep); }
        std::thread foreign([&] { Record(Api::RtvUav, Decision::ContextMismatch, context, 0); });
        foreign.join();
        for (uint32_t i = 0; i < 70; ++i) Record(Api::Rtv, Decision::AcceptedSame, context, i);
        throw std::runtime_error("scope-exit fixture");
    }
    catch (const std::runtime_error&) {}

    const Capture& c = captures[0];
    assert(c.committed.load());
    assert(!c.active.load());
    assert(c.completed.load() == kRowBudget);
    assert(c.reserved.load() == 73);
    assert(c.overflow.load() == 9);
    assert(current.load() == nullptr);
    bool sawForeign = false;
    const auto owner = GetCurrentThreadId();
    for (uint32_t i = 0; i < kRowBudget; ++i)
    {
        assert(c.rows[i].ready.load());
        if (c.rows[i].thread != owner) sawForeign = true;
    }
    assert(sawForeign);
    {
        PassScope restoreBudget(context);
        for (uint32_t i = 0; i < 10; ++i) Record(Api::Rtv, Decision::Enter, context, 0);
        restoreBudget.PhaseNow(Phase::SwapOut);
        Record(Api::RtvUav, Decision::Enter, context, 1);
        Record(Api::RtvUav, Decision::ContextMismatch, context, 1);
    }
    const Capture& restoration = captures[1];
    assert(restoration.rows[10].stackCount != 0);
    assert(restoration.rows[11].stackCount != 0);
    return 0;
}
