#include "ext/core/InjectedStateWindowDiagnostic.h"
#include <memory>
#include <string_view>
#include <thread>

int main()
{
    using namespace ext::core::state_window;
    auto* a = reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x1000));
    auto* b = reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x2000));
    enabled.store(false);
    { NativeDrawScope off(a, "off"); }
    if (Read().count != 0) return 1;

    enabled.store(true);
    const auto first = Begin(a, 101);
    if (!first) return 2;
    if (Begin(b, 303) != 0) return 8; // a nested owner cannot replace the active window
    { NativeDrawScope same(a, "same", 101); }
    { NativeDrawScope other(b, "other", 202); }
    if (Read().count != 0) return 3;
    {
        NativeDrawScope foreign(a, "DrawIndexed", 202);
        if (Read().count != 0) return 4;
    }
    auto hit = Read();
    if (hit.count != 1 || hit.owner == 0 || hit.caller == 0 || hit.kind != std::string_view("DrawIndexed")) return 5;

    auto outliving = std::make_unique<NativeDrawScope>(a, "late", 202);
    End(first);
    const auto second = Begin(a, 101);
    if (!second || second == first) return 6;
    outliving.reset(); // stale token must not count after its window has ended
    if (Read().count != 1) return 7;
    End(second);
    const auto realWindow = Begin(a);
    if (!realWindow) return 9;
    { NativeDrawScope sameThread(a, "same real thread"); }
    std::thread worker([&] { NativeDrawScope foreign(a, "real foreign thread"); });
    worker.join();
    const auto realHit = Read();
    End(realWindow);
    if (realHit.count != 2 || realHit.owner != GetCurrentThreadId() || realHit.owner == realHit.caller) return 10;
    enabled.store(false);
    return 0;
}
