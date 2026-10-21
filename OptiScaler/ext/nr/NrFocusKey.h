#pragma once
// [DlssNr] PauseOnFocusLoss: another name for DisableOnFocusLoss, used by another build, so that a copied ini keeps
// working. The alias is migrated once, right after the ini was read (NrConfig::PostLoad), and then cleared.
//
// Why at load and not when the key is used: DisableOnFocusLoss is declared with the default false, so an explicit false is
// not saved (the line becomes `auto`). An alias resolved at use time would turn the pause back on at the next launch after the
// user unticked the menu box, because the alias line would still say true. Clearing it here makes the next Save write
// `auto` for the alias (its value is gone) and the real value for the key.
//
// Pure and generic over any optional-like (has_value / value / operator= / reset), so it is unit-tested without the game
// (tests/ext/nr/nr_focus_key.cpp).

namespace ext::nr
{
// `key` wins whenever it is set, an explicit false included. Otherwise the alias value (if any) moves into it. The alias
// is cleared in every case.
template <class Key, class Alias> void MigrateFocusAlias(Key& key, Alias& alias)
{
    if (!key.has_value() && alias.has_value())
        key = alias.value();

    alias.reset();
}
} // namespace ext::nr
