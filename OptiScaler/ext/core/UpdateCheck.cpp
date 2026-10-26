#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/core/UpdateCheck.h"

namespace ext::updatecheck
{
void ApplyDefault() { DefaultOff(Config::Instance()->CheckForUpdate); }

namespace
{
// Both run inside Config::Reload / Config::SaveIni. The first one can run inside the very first Config::Instance() call (it hands
// back the object being built), so they allocate nothing, take no lock and only touch the one option.
void OnConfigLoaded() { ApplyDefault(); }

void OnConfigSaved(CSimpleIniA& ini) { KeepOn(Config::Instance()->CheckForUpdate, ini); }

// Registers the two hooks when the DLL's static initialisers run (before DllMain, the way the mip bias glue registers its load
// hook); the config is read later, from Config::Instance(). Nothing here reads or creates the config.
[[maybe_unused]] const bool kHooksRegistered =
    (ExtConfig::updateCheckOnLoaded = &OnConfigLoaded, ExtConfig::updateCheckOnSaved = &OnConfigSaved, true);
} // namespace
} // namespace ext::updatecheck
