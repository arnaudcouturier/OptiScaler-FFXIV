// S10 D0: RegisterAddon against a stand-in ReShade DLL that exports the add-on API (fake_reshade.cpp).
// Usage: reshade_registration_fake.exe <ok|refuse> <path to FakeReShade64.dll>
// One mode per process: the header-only helpers cache the module they resolved in function-local statics.
#include "ext/reshade/ReShadeRegister.h"
#include "ext/reshade/third_party/reshade/reshade.hpp"

#include <cstdio>
#include <cstring>

using namespace ext::reshade;

namespace
{
void OnPresent(reshade::api::effect_runtime*) {}
using InfoFn = void (*)(uint32_t*, void**, int*, int*, int*, void**);

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    const bool refuse = std::strcmp(argv[1], "refuse") == 0;
    SetEnvironmentVariableA("FAKE_RESHADE_MODE", refuse ? "refuse" : "ok");

    HMODULE fake = LoadLibraryA(argv[2]);
    CHECK(fake != nullptr);
    auto info = reinterpret_cast<InfoFn>(GetProcAddress(fake, "Fake_Info"));
    CHECK(info != nullptr);
    HMODULE self = GetModuleHandleW(nullptr);

    // Explicit module.
    const RegisterResult r = RegisterAddon(self, fake);
    uint32_t api = 0;
    void* module = nullptr;
    int calls = 0, events = 0, lastEvent = 0;
    void* lastCallback = nullptr;
    info(&api, &module, &calls, &events, &lastEvent, &lastCallback);

    CHECK(calls == 1);
    CHECK(api == 20);
    CHECK(module == self);
    CHECK(r.reshade == fake);

    if (refuse)
    {
        CHECK(r.status == RegisterStatus::Refused);
        std::puts("PASS: a ReShade that refuses API 20 gives RegisterStatus::Refused");
        return 0;
    }

    CHECK(r.status == RegisterStatus::Ok);

    // The header-only event registration resolves through the module that was registered with.
    reshade::register_event<reshade::addon_event::reshade_present>(&OnPresent);
    info(&api, &module, &calls, &events, &lastEvent, &lastCallback);
    CHECK(events == 1);
    CHECK(lastEvent == static_cast<int>(reshade::addon_event::reshade_present));
    CHECK(lastCallback == reinterpret_cast<void*>(&OnPresent));

    // A second RegisterAddon call (idempotence is the caller's job) simply registers again with the same module.
    std::puts("PASS: RegisterAddon registers API 20 with the module's exports, then register_event reaches the same module");
    return 0;
}
