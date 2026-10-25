// The registration state of the ReShade add-on as the rest of the fork reads it (ReShadeRegistrationState.cpp, which ReShadeAddon.cpp
// builds Status() and OnReshadeLoaded on): what NoteRegistration (the late path's way in) records, and what Registered() answers.
#include "pch.h"

#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeRegistrationState.h"

#include <cstdio>
#include <thread>
#include <vector>

using namespace ext::reshade;

namespace
{
int g_fail = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);                                           \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

RegisterStatus Reason() { return static_cast<RegisterStatus>(Registration().reason.load()); }

void Initial()
{
    // nothing has run: not attempted, not registered, and the reason a start-up answers
    CHECK(!Registration().attempted.load());
    CHECK(!Registered());
    CHECK(Reason() == RegisterStatus::NoModule);
}

void NoteEveryStatus()
{
    Registration().attempted = true; // the load-time attempt ran (and found nothing): the late path notes what it finds
    for (const RegisterStatus status : { RegisterStatus::Refused, RegisterStatus::ExportsMissing, RegisterStatus::Exception,
                                         RegisterStatus::NoSelf, RegisterStatus::NoModule })
    {
        NoteRegistration(status);
        CHECK(!Registered());
        CHECK(Reason() == status);
        CHECK(Registration().attempted.load()); // the load-time attempt's flag is not the late path's to change
    }
    NoteRegistration(RegisterStatus::Ok);
    CHECK(Registered());
    CHECK(Reason() == RegisterStatus::Ok);
    CHECK(Registration().attempted.load());
    // a later note replaces it (the late path does not note after success, but the state is whatever was noted last)
    NoteRegistration(RegisterStatus::Refused);
    CHECK(!Registered() && Reason() == RegisterStatus::Refused);
    NoteRegistration(RegisterStatus::Ok);
    CHECK(Registered());
}

void Threads()
{
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back(
            [t]
            {
                for (int i = 0; i < 2000; ++i)
                {
                    NoteRegistration(((i + t) & 1) != 0 ? RegisterStatus::Ok : RegisterStatus::NoModule);
                    (void)Registered();
                }
            });
    for (std::thread& t : threads)
        t.join();
    NoteRegistration(RegisterStatus::Ok);
    CHECK(Registered());
}
} // namespace

int main()
{
    Initial();
    NoteEveryStatus();
    Threads();
    if (g_fail != 0)
    {
        std::printf("reshade_registration_state: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("reshade_registration_state: all checks passed");
    return 0;
}
