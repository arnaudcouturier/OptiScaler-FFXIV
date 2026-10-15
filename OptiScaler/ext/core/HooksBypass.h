#pragma once
// Hook bypass scope (docs/SEAMS.md section 3.9). Fork code that issues its own native draws or device calls on the
// current thread wraps them in a BypassScope; hooks that must not see those calls test HooksBypassed() first.

namespace ext::core
{
inline thread_local bool g_hooksBypassed = false;

inline bool HooksBypassed() { return g_hooksBypassed; }

struct BypassScope
{
    BypassScope() : _previous(g_hooksBypassed) { g_hooksBypassed = true; }
    ~BypassScope() { g_hooksBypassed = _previous; }

    BypassScope(const BypassScope&) = delete;
    BypassScope& operator=(const BypassScope&) = delete;

  private:
    bool _previous;
};
} // namespace ext::core
