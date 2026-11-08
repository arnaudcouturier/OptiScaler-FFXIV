#pragma once
// Test double of ext/ffxiv/chara/CharaDispatch.h: the shadow mapping route reads only the character preview's replay marker.
namespace ext::ffxiv::chara
{
inline thread_local bool t_replay = false;
} // namespace ext::ffxiv::chara
