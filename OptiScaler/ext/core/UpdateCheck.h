#pragma once
// The upstream update check, off unless the player turns it on.
//
// Upstream starts a version check against its own GitHub project at launch (VersionCheck::Start, dllmain.cpp), gated by
// `Config::CheckForUpdate.value_or_default()`, a bool whose default is true. This fork is a different product, so that check
// would tell a player about the wrong releases and would be a network call nobody asked for. The fork's rule:
//
//   [Hotfix] CheckForUpdate   not in the ini, or auto   -> off
//                             true                      -> on
//                             false                     -> off
//
// Two steps, both registered by UpdateCheck.cpp on the pointers of ExtConfig:
//   DefaultOff   end of every config load. A key that is unset gets the volatile value false: it is what value_or_default()
//                answers, and it is never saved (CustomOptional keeps the ini's own answer aside for value_for_config()).
//   KeepOn       end of every config save, after upstream wrote its keys. Upstream writes a value equal to the compiled default as
//                `auto`, and the compiled default is true: an explicit true (a hand-edited ini, or the menu checkbox) would be
//                saved as `auto`, which this fork reads as off. KeepOn writes the `true` back. It touches nothing else, so an
//                unset key is still saved as `auto` and an explicit false as `false`.
//
// Third step, ApplyDefault(), called by ext::core::Banner() (dllmain.cpp: after the first Config::Instance(), before CheckWorkingMode
// starts the check). A launch without a readable OptiScaler.ini never reaches ext::config::Load (Config::Reload returns before it), so
// the load hook does not run and the key would stay unset, which upstream reads as its default, on. ApplyDefault() is DefaultOff on the
// process-wide option; it changes nothing after a normal load.
//
// Templates over the option and the ini so that the unit test runs them on the real CustomOptional and a real CSimpleIniA
// without the process-wide Config.
namespace ext::updatecheck
{
void ApplyDefault(); // DefaultOff(Config::Instance()->CheckForUpdate); idempotent, allocation free, takes no lock

// `checkForUpdate` is Config::CheckForUpdate (a CustomOptional<bool> with default true).
template <class Option> void DefaultOff(Option& checkForUpdate)
{
    // Unset after the ini was read means the player did not choose. An explicit true or false has a value and is left alone, and so is
    // a second call (the first one made it a value).
    if (!checkForUpdate.has_value())
        checkForUpdate.set_volatile_value(false);
}

template <class Option, class Ini> void KeepOn(Option& checkForUpdate, Ini& ini)
{
    // has_value() && true is always the player's choice: the only value this fork sets itself is the volatile false.
    if (checkForUpdate.has_value() && checkForUpdate.value())
        ini.SetValue("Hotfix", "CheckForUpdate", "true");
}
} // namespace ext::updatecheck
