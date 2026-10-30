#pragma once
// Test stand-in for the hook bypass scope: the tests never install hooks, so
// entering and leaving the scope is a no-op.

namespace ext::core
{
struct BypassScope
{
    BypassScope() = default;
};
} // namespace ext::core
