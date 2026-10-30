#pragma once
// Stand-in for ext/core/ExtConfig.h in the tick-lease tests: the lease reads its key through ext::latency::KeyOn(), which the test defines, so the
// ini binder, simpleini and the process-wide config are not needed here. Only the include has to resolve.
namespace ext
{
struct ExtConfigStub
{
};
inline ExtConfigStub& Cfg()
{
    static ExtConfigStub config;
    return config;
}
} // namespace ext
