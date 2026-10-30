#pragma once
// The shader catalogs of the FFXIV fork as one table, plus the partial-UI-catalog health rule.
//
// Shaders are identified by the CRC-32 of the bytecode passed to CreatePixelShader (nameplate: the vertex shader bytecode passed to
// CreateInputLayout), see ext/core/Crc32.h. A game patch that recompiles a shader changes its CRC and the feature silently goes
// off. This table is what the health accounting (CatalogLedger.h) counts against; it never replaces the owners' own matching
// (UiShaderCatalog.h, FfxivSceneHdr.h, FfxivLightingCapture.h, CharaTags.h, PlateLayoutTag.h): no CRC is
// changed here and no catalog membership is decided by anything but the CRC.
//
// Pure and dependency free apart from the two light owner headers it mirrors (UiShaderCatalog.h, CharaTags.h): UI and chara entries are
// BUILT from the owner's constants, the scene stages are checked against SceneStageOfCrc at
// compile time. The lighting and nameplate literals live in heavy headers and are mirrored here; the test
// tests/ext/ffxiv/catalog/catalog_wiring_pins.cpp checks every one of them against the owner's source text.
//
// refFingerprint: 0 = unknown. It is the structural fingerprint (ShaderFingerprint.h) of the shader as it was when the CRC was recorded,
// to be baked in after one real game run has logged the table (see the `fingerprint` log lines). It is only ever used to NAME
// near-miss candidates in the log; it takes no part in CRC identity.

#include "../chara/CharaTags.h"
#include "../ui/UiShaderCatalog.h"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace ext::ffxiv::catalog
{
enum class Catalog : uint8_t
{
    Ui = 0,   // 7 pixel shaders, the HDR "UI only" stage (FfxivHdrPreview::Tag)
    Scene,    // 5 pixel shaders, scene/post stages 1..5 (FfxivSceneHdr::Stage)
    Lighting, // 9 pixel shaders of the native exposure scan (FfxivLightingCapture::shaders)
    Chara,    // 9 pixel shaders of the character preview: 7 geometry, finisher, equipment display (CharaTags.h)
    Nameplate, // 3 VERTEX shaders hashed in CreateInputLayout (PlateLayoutTag.h)
};
inline constexpr size_t kCatalogCount = 5;

struct Entry
{
    uint32_t crc;
    const char* name;
    uint32_t bytes = 0;          // exact bytecode size when the owner pins it (lighting), else 0 = any
    uint64_t refFingerprint = 0; // 0 = unknown; see the header comment
};

struct CatalogInfo
{
    const char* name;
    const Entry* entries;
    size_t count;
    unsigned healthyMin; // entries a healthy build has created by the time the session settled; 0 = no baseline established
    bool vertexShaders;  // true: CRCs of vertex shader bytecode (CreateInputLayout), not pixel shaders
};

// ---- the tables ---------------------------------------------------------------------------------------------------------------
namespace table
{
namespace ui = ext::ffxiv::ui;
namespace chara = ext::ffxiv::chara;

inline constexpr Entry kUi[] = {
    { ui::kUiCrcs[0], "ui0" }, { ui::kUiCrcs[1], "ui1" }, { ui::kUiCrcs[2], "ui2" }, { ui::kUiCrcs[3], "ui3" },
    { ui::kUiCrcs[4], "ui4" }, { ui::kUiCrcs[5], "ui5" }, { ui::kUiCrcs[6], "ui6" },
};
static_assert(sizeof(kUi) / sizeof(kUi[0]) == ui::kUiCrcs.size());

// Stage n of ui::SceneStageOfCrc. The CRC values are mirrored (the owner is a switch), the static_asserts below tie them to it.
inline constexpr Entry kScene[] = {
    { 0x85e777efu, "tonemap" }, { 0xf8f57f0au, "post-tonemap" }, { 0x27ebc404u, "lut3" }, { 0x1f264d17u, "lut4" }, { 0xf6e81a1bu, "gamma" },
};
static_assert(ui::SceneStageOfCrc(kScene[0].crc) == 1 && ui::SceneStageOfCrc(kScene[1].crc) == 2 && ui::SceneStageOfCrc(kScene[2].crc) == 3 &&
              ui::SceneStageOfCrc(kScene[3].crc) == 4 && ui::SceneStageOfCrc(kScene[4].crc) == 5);

// Mirror of FfxivLightingCapture::shaders (name, DXBC size, CRC); Identify() also compares the 16 byte DXBC checksum, which the
// health accounting does not need (a CRC-32 match with the same size is identity enough for a count).
inline constexpr Entry kLighting[] = {
    { 2246539247u, "ToneMapping", 1512 },       { 4142406171u, "ToneAdjust", 1292 },
    { 3549584390u, "MeasureLumInitial", 1504 }, { 3365552696u, "MeasureLumIterative", 996 },
    { 3429323786u, "MeasureLumFinal", 1120 },   { 3046182588u, "AdaptLum", 1376 },
    { 2198444491u, "BrightPassFilter", 1424 },  { 3541952783u, "BrightPassFilterUpdate", 1444 },
    { 2137381618u, "ToneMapLut", 1392 },
};
static_assert(kLighting[0].crc == kScene[0].crc && kLighting[1].crc == kScene[4].crc); // ToneMapping = stage 1, ToneAdjust = stage 5

inline constexpr Entry kChara[] = {
    { chara::kGeometryCrcs[0], "geometry0" }, { chara::kGeometryCrcs[1], "geometry1" }, { chara::kGeometryCrcs[2], "geometry2" },
    { chara::kGeometryCrcs[3], "geometry3" }, { chara::kGeometryCrcs[4], "geometry4" }, { chara::kGeometryCrcs[5], "geometry5" },
    { chara::kGeometryCrcs[6], "geometry6" }, { chara::kFinisherCrc, "finisher" },      { chara::kEquipDisplayCrc, "equip-display" },
};
static_assert(sizeof(kChara) / sizeof(kChara[0]) == chara::kGeometryCrcs.size() + 2);

// PlateLayoutTag::TagLayout: vertex shader CRCs of variant 0, 4 and 5/6.
inline constexpr Entry kNameplate[] = {
    { 2964264238u, "vs-layout0" }, { 1445359818u, "vs-layout4" }, { 3153679471u, "vs-layout5-6" },
};
} // namespace table

// UI shaders a healthy build has created by the first Present ("UI 6" in all 26 logged runs, both exe builds).
inline constexpr unsigned kUiCatalogHealthyMin = 6;

// healthyMin: ONLY the UI catalog has an established baseline. 0 = none (never flagged below baseline, never triggers the near-miss dump;
// the count and the missing entries are still logged). Not scene: stage 3 (0x27ebc404, lut3) has no match in the installed shader files nor
// in the exe, so a healthy build probably reaches 4 of 5. Not chara: the finisher is created when the lobby starts (+16..20 s), which can
// be after the 30 s settle point. Add a baseline only after a real log shows it.
inline constexpr CatalogInfo kInfo[kCatalogCount] = {
    { "UI", table::kUi, sizeof(table::kUi) / sizeof(table::kUi[0]), kUiCatalogHealthyMin, false },
    { "scene", table::kScene, sizeof(table::kScene) / sizeof(table::kScene[0]), 0, false },
    { "lighting", table::kLighting, sizeof(table::kLighting) / sizeof(table::kLighting[0]), 0, false },
    { "chara", table::kChara, sizeof(table::kChara) / sizeof(table::kChara[0]), 0, false },
    { "nameplate", table::kNameplate, sizeof(table::kNameplate) / sizeof(table::kNameplate[0]), 0, true },
};

constexpr const CatalogInfo& Info(Catalog c) { return kInfo[static_cast<size_t>(c)]; }

// ---- partial UI catalog guard -------------------------------------------------------------------------------------------------
// One bit per UI entry (bit i = table::kUi[i] was created).
inline constexpr uint32_t kUiCatalogMask = (1u << (sizeof(table::kUi) / sizeof(table::kUi[0]))) - 1;

// The exact set of UI shaders a healthy build creates: every bit in 0x5F. Three real runs created all of ui0, ui1, ui2, ui3, ui4 and ui6
// (ui3 arrives late, with the scene, but in every run); ui5 (crc 0xe95deec4) never appeared in the 2026-10-26 run, so it stays out of the set.
// The rule is "all of these were created" instead of "6 of 7 were created"; 0 would mean not pinned yet.
inline constexpr uint32_t kUiRequiredMask = 0x5F; // ui0, ui1, ui2, ui3, ui4, ui6; ui5 is scene-dependent
static_assert((kUiRequiredMask & ~kUiCatalogMask) == 0, "the pinned UI set names only catalogued entries");
static_assert(std::popcount(kUiRequiredMask) == kUiCatalogHealthyMin, "the pinned UI set keeps the healthy threshold at 6");

// Is the UI catalog complete enough for the HUD-less boundary source to be trusted? A partial set is the dangerous case: R1b can then
// fire on a later UI draw than the real first HUD draw and the "UI-free" capture would contain HUD pixels.
// Stateless on purpose: the caller evaluates it on every frame and never latches the answer, so a shader created late clears it.
constexpr bool UiCatalogHealthy(uint32_t uiMask, uint32_t required = kUiRequiredMask, unsigned minCount = kUiCatalogHealthyMin)
{
    uiMask &= kUiCatalogMask;
    if (required != 0)
        return (uiMask & required) == (required & kUiCatalogMask);
    return static_cast<unsigned>(std::popcount(uiMask)) >= minCount;
}
} // namespace ext::ffxiv::catalog
