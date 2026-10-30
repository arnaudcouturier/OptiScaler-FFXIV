// The catalog table (five shader catalogs, names, reference fingerprints) and the partial-UI-catalog health rule.
//
// What this pins:
//   * the sizes the brief names (UI 7, scene stages 5, lighting 9, chara 9, nameplate 3) and that every entry has a name;
//   * the CRC values themselves, written out here independently of the production tables (a changed catalog constant must fail here);
//   * the mirrors against their owners: UI and chara are built FROM the owner's constants, the scene stages are checked against
//     SceneStageOfCrc (the lighting and nameplate literals are checked in catalog_wiring_pins);
//   * UiCatalogHealthy: a healthy build creates the pinned six UI shaders (ui0, ui1, ui2, ui3, ui4, ui6; ui5 is scene-dependent), so the rule is the set, not a count
//     healthy exactly when every required bit is set, evaluated per call (no latch), and an explicit mask or count can replace the pinned set;
//   * refFingerprint is optional (0 = unknown) and is not part of CRC identity.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "check.h"

#include "ext/ffxiv/catalog/CatalogTable.h"
#include "ext/ffxiv/chara/CharaTags.h"
#include "ext/ffxiv/ui/UiShaderCatalog.h"

#include <bit>
#include <set>
#include <string>

using namespace ext::ffxiv::catalog;

namespace
{
const Entry* Find(Catalog c, uint32_t crc)
{
    const CatalogInfo& info = Info(c);
    for (size_t i = 0; i < info.count; ++i)
        if (info.entries[i].crc == crc)
            return &info.entries[i];
    return nullptr;
}

void Sizes()
{
    CHECK_EQ(Info(Catalog::Ui).count, 7u);
    CHECK_EQ(Info(Catalog::Scene).count, 5u);
    CHECK_EQ(Info(Catalog::Lighting).count, 9u);
    CHECK_EQ(Info(Catalog::Chara).count, 9u);
    CHECK_EQ(Info(Catalog::Nameplate).count, 3u);
    CHECK_EQ(kCatalogCount, 5u);

    for (size_t c = 0; c < kCatalogCount; ++c)
    {
        const CatalogInfo& info = Info(static_cast<Catalog>(c));
        CHECK(info.name != nullptr && info.name[0] != '\0');
        CHECK(info.count >= 1 && info.count <= 32); // a catalog mask is one 32-bit word
        std::set<uint32_t> crcs;
        std::set<std::string> names;
        for (size_t i = 0; i < info.count; ++i)
        {
            const Entry& e = info.entries[i];
            CHECK(e.name != nullptr && e.name[0] != '\0');
            CHECK(crcs.insert(e.crc).second);
            CHECK(names.insert(e.name ? e.name : "").second);
        }
    }
    // Only the nameplate catalog is made of vertex shaders (CreateInputLayout bytecode), the rest are pixel shaders.
    for (size_t c = 0; c < kCatalogCount; ++c)
        CHECK_EQ(Info(static_cast<Catalog>(c)).vertexShaders ? 1 : 0, c == static_cast<size_t>(Catalog::Nameplate) ? 1 : 0);
}

// The values, written out a second time (P0-B tables). Changing a catalog constant has to fail here.
void Values()
{
    const uint32_t ui[7] { 3865947726u, 2966694105u, 3759127293u, 1898885941u, 4076256744u, 3915247300u, 1823062889u };
    for (size_t i = 0; i < 7; ++i)
        CHECK_EQ(Info(Catalog::Ui).entries[i].crc, ui[i]);

    const uint32_t scene[5] { 0x85e777efu, 0xf8f57f0au, 0x27ebc404u, 0x1f264d17u, 0xf6e81a1bu };
    for (size_t i = 0; i < 5; ++i)
    {
        CHECK_EQ(Info(Catalog::Scene).entries[i].crc, scene[i]);
        CHECK_EQ(ext::ffxiv::ui::SceneStageOfCrc(scene[i]), i + 1); // owner agrees with the stage number
    }

    struct L
    {
        const char* name;
        uint32_t bytes, crc;
    };
    const L lighting[9] {
        { "ToneMapping", 1512, 2246539247u },       { "ToneAdjust", 1292, 4142406171u },
        { "MeasureLumInitial", 1504, 3549584390u }, { "MeasureLumIterative", 996, 3365552696u },
        { "MeasureLumFinal", 1120, 3429323786u },   { "AdaptLum", 1376, 3046182588u },
        { "BrightPassFilter", 1424, 2198444491u },  { "BrightPassFilterUpdate", 1444, 3541952783u },
        { "ToneMapLut", 1392, 2137381618u },
    };
    for (size_t i = 0; i < 9; ++i)
    {
        const Entry& e = Info(Catalog::Lighting).entries[i];
        CHECK_EQ(e.crc, lighting[i].crc);
        CHECK_EQ(e.bytes, lighting[i].bytes);
        CHECK(std::string(e.name) == lighting[i].name);
    }

    const uint32_t chara[9] { 0x68029DFCu, 0x05626DF1u, 0x478C1594u, 0x67AD01CAu, 0xA657BFE5u,
                              0xCC2C6A71u, 0xD2AEC0AAu, 0xFDB2F4FCu, 0x7B40E31Au };
    for (size_t i = 0; i < 9; ++i)
        CHECK_EQ(Info(Catalog::Chara).entries[i].crc, chara[i]);


    const uint32_t nameplate[3] { 2964264238u, 1445359818u, 3153679471u };
    for (size_t i = 0; i < 3; ++i)
        CHECK_EQ(Info(Catalog::Nameplate).entries[i].crc, nameplate[i]);
}

// Mirrors against their owners (compile time where the owner is light enough to include).
void Owners()
{
    for (size_t i = 0; i < 7; ++i)
        CHECK_EQ(Info(Catalog::Ui).entries[i].crc, ext::ffxiv::ui::kUiCrcs[i]);
    for (size_t i = 0; i < 7; ++i)
        CHECK_EQ(ext::ffxiv::chara::RoleOfCrc(Info(Catalog::Chara).entries[i].crc), uint32_t(ext::ffxiv::chara::RoleGeometry));
    CHECK_EQ(Info(Catalog::Chara).entries[7].crc, ext::ffxiv::chara::kFinisherCrc);
    CHECK_EQ(Info(Catalog::Chara).entries[8].crc, ext::ffxiv::chara::kEquipDisplayCrc);
}

// A CRC can belong to several catalogs: ToneMapping/ToneAdjust are lighting entries AND scene stages 1/5.
void Overlaps()
{
    CHECK(Find(Catalog::Scene, 0x85e777efu) && Find(Catalog::Lighting, 0x85e777efu));
    CHECK(Find(Catalog::Scene, 0xf6e81a1bu) && Find(Catalog::Lighting, 0xf6e81a1bu));
    CHECK(Find(Catalog::Ui, 3865947726u)); // a former replacement-layer shader stays a UI shader
    CHECK(!Find(Catalog::Ui, 0x85e777efu));
    CHECK(!Find(Catalog::Scene, 3865947726u));
    CHECK(!Find(Catalog::Nameplate, 3865947726u)); // nameplate CRCs are vertex-shader CRCs, a separate namespace
}

// healthy baselines: ONLY the UI catalog has one that an observation establishes (6 UI shader objects at the first draw in all 26
// logged runs). Every other catalog is 0 = no established baseline: never flagged as "below the healthy baseline", never the reason for the
// near-miss dump; its count and missing entries are still logged. In particular:
//   * scene: stage 3 (0x27ebc404, lut3) has no match in the installed shader files nor in the exe
//     and was not in the draws of a D1 frame (S10-E2-LOG), so a healthy build probably reaches 4 of 5, not 5;
//   * chara: the finisher shader is created when the lobby starts (+16..20 s in 26 runs), which can be after the 30 s settle point, so 9 of 9
//     is timing dependent, not a baseline;
//   * lighting, nameplate: never measured against a healthy log.
// A baseline is added here only after a real log shows it.
void Baselines()
{
    CHECK_EQ(Info(Catalog::Ui).healthyMin, kUiCatalogHealthyMin);
    CHECK_EQ(Info(Catalog::Scene).healthyMin, 0u);
    CHECK_EQ(Info(Catalog::Chara).healthyMin, 0u);
    CHECK_EQ(Info(Catalog::Lighting).healthyMin, 0u);
    CHECK_EQ(Info(Catalog::Nameplate).healthyMin, 0u);
    // UI is the only catalog with a baseline, and it is never above the catalog size.
    for (size_t c = 0; c < kCatalogCount; ++c)
    {
        const CatalogInfo& info = Info(static_cast<Catalog>(c));
        CHECK_EQ(info.healthyMin != 0 ? 1 : 0, c == static_cast<size_t>(Catalog::Ui) ? 1 : 0);
        CHECK(info.healthyMin <= info.count);
    }
}

void Health()
{
    CHECK_EQ(kUiCatalogHealthyMin, 6u); // the pinned set keeps the healthy threshold at 6 (26 runs + the 2026-10-26 run)
    CHECK_EQ(kUiCatalogMask, 0x7Fu);
    CHECK_EQ(kUiRequiredMask, 0x5Fu); // the pinned six (ui5 is scene-dependent); 0 would mean not pinned yet

    // Every 7-bit mask: healthy exactly when the pinned six are all present, whatever else is set. This pins the rule from both sides.
    for (uint32_t mask = 0; mask < 128; ++mask)
        CHECK_EQ(UiCatalogHealthy(mask) ? 1 : 0, (mask & 0x5Fu) == 0x5Fu ? 1 : 0);

    // Spelled out: only the 6-of-7 missing ui5 is healthy; missing any pinned entry is not, 5 is not, 7 is.
    for (uint32_t missing = 0; missing < 7; ++missing)
    {
        CHECK_EQ(UiCatalogHealthy(kUiCatalogMask & ~(1u << missing)) ? 1 : 0, missing == 5 ? 1 : 0);
        for (uint32_t second = 0; second < 7; ++second)
            if (second != missing)
                CHECK(!UiCatalogHealthy(kUiCatalogMask & ~(1u << missing) & ~(1u << second)));
    }
    CHECK(UiCatalogHealthy(kUiCatalogMask));
    CHECK(!UiCatalogHealthy(0));

    // Bits outside the catalog are not UI shaders.
    CHECK(!UiCatalogHealthy(0xFFFFFF80u));
    CHECK(!UiCatalogHealthy(0x1Fu | 0xFFFFFF80u)); // 5 real bits + noise
    CHECK(!UiCatalogHealthy(0x3Fu | 0xFFFFFF80u)); // 6 real bits + noise, ui6 missing: not the pinned set
    CHECK(UiCatalogHealthy(0x5Fu | 0xFFFFFF80u));  // the pinned six + noise

    // Not latched: a late creation clears it (the function has no state; the same mask gives the same answer).
    uint32_t mask = 0x1Fu;
    CHECK(!UiCatalogHealthy(mask));
    CHECK(!UiCatalogHealthy(mask));
    mask |= 1u << 5; // ui5 without ui6: 6 bits, still not the pinned set
    CHECK(!UiCatalogHealthy(mask));
    mask |= 1u << 6; // ui6 completes the pinned six
    CHECK(UiCatalogHealthy(mask));
    CHECK(UiCatalogHealthy(mask));

    // An explicit required mask replaces the pinned set (here a pretend one, ui0..ui5); the count then no longer matters.
    // (the production pinned set itself is pinned above: kUiRequiredMask == 0x5F)
    const uint32_t required = 0x3Fu; // pretend the real run showed ui0..ui5
    CHECK(UiCatalogHealthy(0x3Fu, required));
    CHECK(UiCatalogHealthy(0x7Fu, required));
    CHECK(!UiCatalogHealthy(0x5Fu, required)); // 6 bits (ui6 instead of ui5) but a required one is missing
    CHECK(!UiCatalogHealthy(0x7Eu & ~1u, required));
    CHECK(UiCatalogHealthy(0x3Fu, required, 7)); // required wins over the count threshold
    // An explicit count threshold other than the named constant is honoured (the wiring only ever passes the constant).
    CHECK(UiCatalogHealthy(0x0Fu, 0, 4));
    CHECK(!UiCatalogHealthy(0x07u, 0, 4));
}

// refFingerprint is 0 (unknown) unless set and never takes part in CRC identity.
void RefIsOptional()
{
    constexpr Entry plain { 123u, "x" };
    static_assert(plain.refFingerprint == 0 && plain.bytes == 0);
    constexpr Entry withRef { 123u, "x", 0, 0xABCDEF0123456789ull };
    static_assert(withRef.crc == plain.crc);
    CHECK_EQ(withRef.refFingerprint, 0xABCDEF0123456789ull);
    CHECK(Find(Catalog::Ui, 3865947726u) != nullptr); // membership is by CRC whatever the reference says
}
} // namespace

int main()
{
    Sizes();
    Values();
    Owners();
    Overlaps();
    Baselines();
    Health();
    RefIsOptional();
    return Finish("catalog_table");
}
