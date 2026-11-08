#pragma once
// Engine light controls, writer 1 (eng-cb): the device side of the constant-buffer patch family (the rules are in CbFamilyRules.h).
//
// The module classifies the engine's own draws (the moon's light draw and the water pass's copy of it, the deferred composite, the water surface),
// builds the entry tables for the engine pass overrides' ONE CbPatch primitive, and records the F9 rows. It does not own the patch, the swap or the
// draw: EnginePass.cpp runs them (the same CbPatch, the same guards), so the family adds no second mechanism.
//
// Arming: the module publishes its ask through `Asks()`, which EnginePass.cpp folds into the seam's feature lists (ShadowsAmbientAsks), so the draw
// seam and the pixel-shader tracker run when only this family wants a draw, whatever [FFXIV] EnginePassOverrides says. Its failures are its own
// (Session in CbFamilyRules.h): they never feed the engine module's streaks, whose limit switches the shipped knobs off.

#include "ext/ffxiv/enginelight/CbFamilyRules.h"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <string>

namespace ext::ffxiv::enginelight::cb
{
// One draw's plan: the buffer to patch and the entries to patch it with.
struct DrawPlan
{
    PatchKind kind = PatchKind::Moon;
    uint32_t slot = 0;  // the constant-buffer slot the draw reads the buffer at
    uint32_t bytes = 0; // the buffer's bound size
    eng::ScaleEntry entries[eng::kMaxScaleEntries] {};
    uint32_t count = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> game; // the buffer the draw really bound (the patch copies exactly this one)
    Rgb factor;                                // the per-channel factor, for the dump row
    float weight = 0.0f;
    bool mapReaders = false; // the readers inside the swap must see the game's buffer (the light draws)
};

// Whether the family wants the seam entered: a published state that is off the engine's values, now or at the frame boundary, and not stood down.
bool Asks();

// Whether this draw's anchors are ones the family may take (a light draw or a reader of the planar reflection map, which the water surface is) and
// the family asks. One relaxed load for every other draw of the frame.
bool WantsDraw(eng::Anchors anchors);

// Plans the patch of a light draw or the water surface. False = not this family's draw / identity / refused: the draw runs unchanged and nothing is
// counted (the point-light volumes and the shadow-mask readers are in this case on every frame). True = `plan` is filled.
bool Plan(ID3D11DeviceContext* context, eng::Anchors anchors, DrawPlan& plan);

// The composite's entries for this frame (latched state): appended by EnginePass.cpp after the shipped knob's and the shadows module's.
uint32_t AmbientEntries(eng::ScaleEntry* out, uint32_t capacity);

// While the engine light master is on, the composite's ambient is the family's alone: the shadows module's night ambient lift is not appended.
bool OwnsAmbient();

// The family's own accounting. A patched draw is a run; a draw the family asked for and could not patch is a failure, and 64 in a row stand the
// family down for the session (one log line).
void NoteRun();
void NoteFailure(const char* why);

// The shader-creation hook: the stamp of a shader that declares the light draw's or the water surface's names.
void NoteShader(ID3D11PixelShader* shader, const void* code, size_t size);

// The F9 rows: one row per patched buffer on a capture frame (staging copies of the game's buffer and of the patch's destination). No-op off capture.
void RecordPatch(ID3D11DeviceContext* context, PatchKind kind, uint32_t slot, ID3D11Buffer* game, ID3D11Buffer* patched, uint32_t bytes,
                 const Rgb& factor, float weight);

// The same for the composite's ambient table, whose factor and weight are the latched state's.
void RecordComposite(ID3D11DeviceContext* context, uint32_t slot, ID3D11Buffer* game, ID3D11Buffer* patched, uint32_t bytes);

// The counters the card and the tests read.
struct Status
{
    bool asks = false;
    bool standDown = false;
    uint32_t moonRuns = 0, waterMoonRuns = 0, waterAmbientRuns = 0, compositeRuns = 0;
    uint32_t failures = 0;
    uint32_t stamped = 0;
};
Status CurrentStatus();

// The one line the composite writes the first time the family's entries ride its dispatch (the latched state's factors).
std::string CompositeRunLine(uint32_t boundBytes);

void Release();
} // namespace ext::ffxiv::enginelight::cb
