#pragma once
// Engine pass overrides: the shader index (ext/ffxiv/engine/PassIndex.cpp).
//
// The game creates its pixel shaders while it loads, so the index is filled whatever the key says: a shader whose bytecode names one of
// EnginePassRules.h's names is parsed once, its slot table is stored, and the shader object is stamped with the table's index. A draw then
// costs one integer load (the pixel-shader seam tracks the bound shader) and a copy of the small layout.
//
// Nothing here touches a device: the parse reads the DXBC reflection the engine's bytecode carries, so a host test compiles a shader and
// checks the answer, and the draw path only ever sees plain structs.

#include "ext/ffxiv/engine/EnginePassRules.h"

#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::engine
{
// The slots of every name of kNames a shader declares, plus its declared output element count. Not `ok` when the bytecode names none of
// them, which is "not a draw this module can patch". Pure.
PassLayout PassLayoutOfShader(const void* code, size_t size);

// The element count of the bytecode's output signature, or 0 when it has none this reader understands. Pure; a truncated or foreign blob
// answers 0 instead of faulting, because the blob comes from the game.
uint32_t OutputCountOfShader(const void* code, size_t size);

// Whether the bytecode contains any of kNames as a byte string, which is the cheap test that drops the shaders that cannot touch a knob
// before the reflection parse. Pure, and deliberately a plain scan: the reflection is only read for the few shaders that name a buffer.
bool MentionsAnyName(const void* code, size_t size) noexcept;
} // namespace ext::ffxiv::engine
