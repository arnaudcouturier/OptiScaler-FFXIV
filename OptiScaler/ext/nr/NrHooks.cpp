#include "pch.h"

#include "ext/nr/NrHooks.h"

namespace ext::nr
{
namespace
{
SceneHdrHooks g_sceneHdr {};
}

void SetSceneHdrHooks(const SceneHdrHooks& hooks) { g_sceneHdr = hooks; }

const SceneHdrHooks& SceneHdr() { return g_sceneHdr; }
} // namespace ext::nr
