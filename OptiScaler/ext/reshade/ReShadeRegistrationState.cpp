#include "pch.h"

#include "ext/reshade/ReShadeAddon.h"
#include "ext/reshade/ReShadeRegistrationState.h"

namespace ext::reshade
{
RegistrationState& Registration()
{
    static RegistrationState* state = new RegistrationState;
    return *state;
}

bool Registered() { return Registration().registered.load(std::memory_order_acquire); }

void NoteRegistration(RegisterStatus status)
{
    auto& g = Registration();
    g.reason.store(static_cast<uint8_t>(status), std::memory_order_release);
    g.registered.store(status == RegisterStatus::Ok, std::memory_order_release);
}
} // namespace ext::reshade
