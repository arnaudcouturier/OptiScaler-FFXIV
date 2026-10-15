#include "pch.h"

#include "ext/fg/FgConfig.h"

#include "ext/core/ExtConfig.h"

namespace ext
{
void FgConfig::Bind(config::Binder& b)
{
    b.Add("DLSSG", "AdaMfgUnlock", adaMfgUnlock);
}
} // namespace ext
