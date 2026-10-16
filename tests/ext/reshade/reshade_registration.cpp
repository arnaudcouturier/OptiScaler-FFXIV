// S10 D0: the registration helper degrades cleanly. This process has no ReShade, so every case must return a status
// WITHOUT reaching the header-only ReShade helpers (their function-local statics cache the first module and call
// cached function pointers unchecked). The positive paths against a stand-in ReShade DLL are reshade_registration_fake.
#include "ext/reshade/ReShadeRegister.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace ext::reshade;

int main()
{
    HMODULE self = GetModuleHandleW(nullptr);
    assert(self != nullptr);

    // No own module: nothing is touched.
    assert(RegisterAddon(nullptr, nullptr).status == RegisterStatus::NoSelf);
    assert(RegisterAddon(nullptr, GetModuleHandleW(L"kernel32.dll")).status == RegisterStatus::NoSelf);

    // Null ReShade module and none in the process (the test exe and the system DLLs do not export the add-on API).
    {
        const RegisterResult r = RegisterAddon(self, nullptr);
        assert(r.status == RegisterStatus::NoModule);
        assert(r.reshade == nullptr);
    }

    // A module without the add-on exports (any ordinary DLL): reported with the first missing export, never called.
    {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        assert(kernel32 != nullptr);
        const RegisterResult r = RegisterAddon(self, kernel32);
        assert(r.status == RegisterStatus::ExportsMissing);
        assert(r.reshade == kernel32);
        assert(r.missingExport != nullptr && std::strcmp(r.missingExport, "ReShadeRegisterAddon") == 0);
    }

    // Repeating a failed registration is harmless and gives the same answer (nothing was cached by the failures).
    assert(RegisterAddon(self, nullptr).status == RegisterStatus::NoModule);
    assert(RegisterAddon(self, GetModuleHandleW(L"ntdll.dll")).status == RegisterStatus::ExportsMissing);

    // Every status has a non-empty reason text for the one log line.
    const RegisterStatus all[] = { RegisterStatus::Ok, RegisterStatus::NoSelf, RegisterStatus::NoModule,
                                   RegisterStatus::ExportsMissing, RegisterStatus::Refused, RegisterStatus::Exception };
    for (RegisterStatus s : all)
        assert(Describe(s) != nullptr && Describe(s)[0] != '\0');

    std::puts("PASS: registration reports NoSelf / NoModule / ExportsMissing without touching ReShade state");
    return 0;
}
