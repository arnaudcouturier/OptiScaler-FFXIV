// Engine pass overrides: the shader index's table (SeenTable in ext/ffxiv/engine/EnginePassRules.h), on the host.
//
// The table maps a bound pixel shader's address to the packed anchors its layout carries. It used to be a 1,024-slot direct-mapped table, and
// about 1,585 pixel shaders of a live session declare the wetness buffer alone, so a collision silently dropped a shader and its draws kept the
// engine's own value while the rest were patched. These checks are the properties the replacement owes the draw path:
//
//   * a shader never evicts another one: every inserted address is found, at 5,000 and at the index's full 20,000-shader cap;
//   * the probe is a bounded linear walk from the address's home slot, and a full probe refuses the insert instead of overwriting a record;
//   * a recycled address is caught by the id the object itself carries, not trusted;
//   * the record carries the anchors whole (the map is 64 bits) and a record re-indexed under another id never answers the previous one;
//   * a layout with no anchor bit for any knob is not stored at all;
//   * a bind of an address the table does not hold asks the object nothing, so the common bind pays one probe and no D3D call.
#include "check.h"

#include "ext/ffxiv/engine/EnginePassRules.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ext::ffxiv::engine;

namespace
{
// 16-byte-aligned addresses in the range the D3D runtime hands a process (the low 4 bits of a shader object are zero, and objects of one
// heap share most of their high bits - the pattern that makes a weak hash collide).
constexpr uintptr_t kBase = uintptr_t(0x00000212A1B2C000);
constexpr uintptr_t AddressAt(uint32_t i) { return kBase + uintptr_t(i) * 16; }

// The packed anchors the seam stores for the knobs' draws: one shader may carry both, and the ambient slot rides in the low byte while the
// wetness slot rides in bits 8..15, so a table that stores the value must give back every bit of it.
constexpr Anchors kAmbientAnchors = kAmbientDrawBit | 2u;
constexpr Anchors kWetnessAnchors = kWetnessDrawBit | (5u << kWetnessSlotShift);
constexpr Anchors kBothAnchors = kAmbientAnchors | kWetnessAnchors;

// The later knobs' bits of the map: a record stores all 64 of them, so a value that carries them must come back unchanged.
constexpr Anchors kLaterBits = kOcclusionReaderBit | kShaftsLightBit | (Anchors(7) << kShaftsSlotShift) | kGlossReaderBit;

// The id the shader object carries: what SetPrivateData stored on it, 0 when it carries none. The product asks the object; here a map answers,
// and it counts the asks, because a bind of an address the table does not hold must not ask at all.
struct Stamps
{
    std::unordered_map<uintptr_t, uint32_t> byAddress;
    mutable uint32_t asked = 0;

    uint32_t operator()(uintptr_t address) const
    {
        ++asked;
        const auto found = byAddress.find(address);
        return found == byAddress.end() ? 0u : found->second;
    }
};

// The addresses whose home slot is `home`, in the order the table sees them.
std::vector<uintptr_t> Colliding(uint32_t home, uint32_t mask, size_t count)
{
    std::vector<uintptr_t> found;
    for (uint32_t i = 0; found.size() < count && i < 200000; ++i)
    {
        const uintptr_t address = AddressAt(i);
        if (SeenSlotOf(address, mask) == home)
            found.push_back(address);
    }
    return found;
}
} // namespace

int main()
{
    // ---- the slot count: a power of two, twice the index's cap, and the hash stays inside it
    {
        // the record is three fields - the address, the anchors and the id - and the table is 3 MiB at 131,072 slots
        CHECK_EQ(sizeof(SeenShader), 24u);
        CHECK_EQ(kSeenSlots * sizeof(SeenShader), 3072u * 1024u);
        CHECK_EQ(kSeenSlots & (kSeenSlots - 1), 0u);
        CHECK(kSeenSlots >= 2 * kMaxIndexedShaders);
        CHECK_EQ(kSeenProbeLength, 16u); // the bound the docs and the report name; the checks below pin its behaviour
        for (uint32_t i = 0; i < 100000; ++i)
        {
            const uintptr_t address = AddressAt(i);
            const uint32_t slot = SeenSlotOf(address, kSeenSlots - 1);
            CHECK(slot < kSeenSlots);
            CHECK_EQ(slot, SeenSlotOf(address, kSeenSlots - 1)); // the same address always lands in the same slot
        }
        // the probe walks consecutive slots and wraps the mask instead of reading past the end
        CHECK_EQ(SeenProbeSlot(30, 4, 31), 2u);
        CHECK_EQ(SeenProbeSlot(0, kSeenProbeLength - 1, kSeenSlots - 1), uint32_t(kSeenProbeLength - 1));
    }

    // ---- the probe: colliding addresses take consecutive slots of the same home, and a probe that runs out refuses instead of overwriting
    {
        SeenTable<32> table; // two probes' worth of slots: the bound, not the table being full, is what refuses the seventeenth address
        Stamps stamps;
        constexpr uint32_t kMask = 31;
        const std::vector<uintptr_t> colliding = Colliding(3, kMask, kSeenProbeLength + 1);
        CHECK_EQ(colliding.size(), size_t(kSeenProbeLength) + 1);
        for (uint32_t i = 0; i < kSeenProbeLength; ++i)
        {
            const uint32_t anchors = i % 2 == 0 ? kAmbientAnchors : kWetnessAnchors;
            CHECK(SeenInsert::Inserted == table.Insert(colliding[i], 100 + i, anchors));
            stamps.byAddress[colliding[i]] = 100 + i;
        }
        CHECK_EQ(table.Count(), kSeenProbeLength);
        for (uint32_t i = 0; i < kSeenProbeLength; ++i)
        {
            const uint32_t anchors = i % 2 == 0 ? kAmbientAnchors : kWetnessAnchors;
            CHECK_EQ(table.Find(colliding[i], stamps), anchors); // each address answered through the probe it was stored at
        }
        // the seventeenth address has the same home and every slot of its probe is taken: it is not stored, its draws fail open, and the
        // sixteen records the table already held are untouched
        stamps.byAddress[colliding[kSeenProbeLength]] = 123;
        CHECK(SeenInsert::Full == table.Insert(colliding[kSeenProbeLength], 123, kBothAnchors));
        CHECK_EQ(table.Count(), kSeenProbeLength);
        CHECK_EQ(table.Find(colliding[kSeenProbeLength], stamps), 0u);
        for (uint32_t i = 0; i < kSeenProbeLength; ++i)
        {
            const uint32_t anchors = i % 2 == 0 ? kAmbientAnchors : kWetnessAnchors;
            CHECK_EQ(table.Find(colliding[i], stamps), anchors);
        }
    }

    // ---- 5,000 addresses: every one is stored and answers its own anchors
    {
        SeenTable<kSeenSlots> table;
        Stamps stamps;
        uint32_t stored = 0;
        for (uint32_t i = 0; i < 5000; ++i)
        {
            const uintptr_t address = AddressAt(i);
            const uint32_t anchors = i % 3 == 0 ? kBothAnchors : (i % 3 == 1 ? kAmbientAnchors : kWetnessAnchors);
            CHECK(SeenInsert::Inserted == table.Insert(address, 1 + i, anchors));
            stamps.byAddress[address] = 1 + i;
            ++stored;
        }
        CHECK_EQ(table.Count(), stored);
        for (uint32_t i = 0; i < 5000; ++i)
        {
            const uint32_t anchors = i % 3 == 0 ? kBothAnchors : (i % 3 == 1 ? kAmbientAnchors : kWetnessAnchors);
            CHECK_EQ(table.Find(AddressAt(i), stamps), anchors);
        }
    }

    // ---- the index's whole cap: 40,960 addresses, the table's probe the only thing that may refuse one, and every stored one found
    {
        SeenTable<kSeenSlots> table;
        Stamps stamps;
        uint32_t refused = 0;
        for (uint32_t i = 0; i < kMaxIndexedShaders; ++i)
        {
            const uintptr_t address = AddressAt(i);
            const uint32_t anchors = i % 2 == 0 ? kAmbientAnchors : kWetnessAnchors;
            const SeenInsert result = table.Insert(address, 1 + i, anchors);
            CHECK(result == SeenInsert::Inserted || result == SeenInsert::Full);
            if (result == SeenInsert::Inserted)
                stamps.byAddress[address] = 1 + i;
            else
                ++refused;
        }
        // The cap fits the table: at 131,072 slots the load is 0.31, so a bounded 16-probe insert refuses at most the one address whose home and
        // neighbourhood are already taken - on this test's dense 16-byte pattern exactly one - and that refusal is the module's own full-table
        // line, not a silent pass-on. Everything the table accepted answers with its own anchors.
        CHECK(refused <= 1u);
        CHECK_EQ(table.Count(), kMaxIndexedShaders - refused);
        for (uint32_t i = 0; i < kMaxIndexedShaders; ++i)
        {
            const uint32_t anchors = i % 2 == 0 ? kAmbientAnchors : kWetnessAnchors;
            const Anchors found = table.Find(AddressAt(i), stamps);
            CHECK_EQ(found, stamps.byAddress.count(AddressAt(i)) != 0 ? anchors : Anchors(0));
        }
    }

    // ---- an address the table does not hold asks the object nothing: the common bind pays one probe and no D3D call
    {
        SeenTable<64> table;
        Stamps stamps;
        const uintptr_t known = AddressAt(7);
        CHECK(SeenInsert::Inserted == table.Insert(known, 7, kAmbientAnchors));
        stamps.byAddress[known] = 7;
        CHECK_EQ(table.Find(AddressAt(8), stamps), 0u);
        CHECK_EQ(stamps.asked, 0u); // a pointer miss never asks
        CHECK_EQ(table.Find(known, stamps), kAmbientAnchors);
        CHECK_EQ(stamps.asked, 1u); // a hit asks once
    }

    // ---- a recycled address: the object's own stamp must still be the record's id, and a real re-index replaces the record in place
    {
        SeenTable<256> table;
        Stamps stamps;
        const uintptr_t address = AddressAt(3);
        CHECK(SeenInsert::Inserted == table.Insert(address, 7, kAmbientAnchors));
        stamps.byAddress[address] = 7;
        CHECK_EQ(table.Find(address, stamps), kAmbientAnchors);

        // the game released the object and the runtime handed the same address to another shader, whose stamp is another layout's id: the
        // old record is not trusted
        stamps.byAddress[address] = 9;
        CHECK_EQ(table.Find(address, stamps), 0u);

        // an object with no stamp at all answers the same
        stamps.byAddress.erase(address);
        CHECK_EQ(table.Find(address, stamps), 0u);

        // the new layout is indexed in its turn: the record is replaced in place, so the address never takes two slots
        CHECK(SeenInsert::Inserted == table.Insert(address, 9, kWetnessAnchors));
        stamps.byAddress[address] = 9;
        CHECK_EQ(table.Count(), 1u);
        CHECK_EQ(table.Find(address, stamps), kWetnessAnchors);
        // and the id it replaced is not an answer any more
        CHECK_EQ(table.Find(address, [](uintptr_t) { return 7u; }), 0u);
    }

    // ---- the record carries the whole 64-bit word, and a stale id beside new anchors is refused instead of answered
    {
        SeenTable<64> table;
        const uintptr_t address = AddressAt(11);
        const Anchors wide = kBothAnchors | kLaterBits;
        CHECK(SeenInsert::Inserted == table.Insert(address, 21, wide));
        CHECK_EQ(table.Find(address, [](uintptr_t) { return 21u; }), wide); // every bit of the map round-trips
        CHECK_EQ(uint32_t(table.Find(address, [](uintptr_t) { return 21u; })), uint32_t(kBothAnchors));
        CHECK_EQ(table.Count(), 1u);

        // the same address re-indexed as another layout: the record holds the new pair, and a reader that still holds the previous id is
        // refused rather than given the new anchors under it - the one pair a replace can leave visible
        CHECK(SeenInsert::Inserted == table.Insert(address, 22, kWetnessAnchors));
        CHECK_EQ(table.Count(), 1u);
        CHECK_EQ(table.Find(address, [](uintptr_t) { return 22u; }), kWetnessAnchors);
        CHECK_EQ(table.Find(address, [](uintptr_t) { return 21u; }), Anchors(0));

        // and a replacement publishes its own anchors with its own id, never the ones it replaced
        CHECK(SeenInsert::Inserted == table.Insert(address, 23, wide));
        CHECK_EQ(table.Find(address, [](uintptr_t) { return 23u; }), wide);
        CHECK_EQ(table.Find(address, [](uintptr_t) { return 22u; }), Anchors(0));
    }

    // ---- the hash spreads one heap, not only a sequence of adjacent addresses
    {
        // A session's shader objects come from a few heaps and are 16 KiB apart far more often than 16 bytes apart: 4,096 addresses at a 16 KiB
        // stride agree in bits 0..11, so a hash that only folded those bits would answer 256 home slots for the whole heap - and every bind of
        // the heap would walk a probe that long. The table has 65,536 slots; the addresses must land in a spread of them.
        std::set<uint32_t> homes;
        for (uint32_t i = 0; i < 4096; ++i)
            homes.insert(SeenSlotOf(kBase + uintptr_t(i) * 0x1000u, kSeenSlots - 1));
        CHECK(homes.size() >= 3000); // about 3,970 distinct slots: 4,096 addresses in 65,536 slots, birthday collisions only

        // Two addresses that differ only above the low half of the word are two objects, and the table must give them their own slots: a shared
        // home would put the second behind the first record on every bind of it.
        const uintptr_t high = kBase + (uintptr_t(1) << 40);
        CHECK(SeenSlotOf(high, kSeenSlots - 1) != SeenSlotOf(kBase, kSeenSlots - 1));
        SeenTable<kSeenSlots> table;
        CHECK(SeenInsert::Inserted == table.Insert(kBase, 11, kAmbientAnchors));
        CHECK(SeenInsert::Inserted == table.Insert(high, 12, kWetnessAnchors));
        CHECK_EQ(table.Count(), 2u);
        CHECK_EQ(table.Find(kBase, [](uintptr_t) { return 11u; }), kAmbientAnchors);
        CHECK_EQ(table.Find(high, [](uintptr_t) { return 12u; }), kWetnessAnchors);
    }

    // ---- a layout with no anchor bit for any knob is not stored, so it costs neither a slot nor an id
    {
        SeenTable<16> table;
        Stamps stamps;
        const uintptr_t address = AddressAt(1);
        CHECK(SeenInsert::NoAnchors == table.Insert(address, 3, 0));
        CHECK_EQ(table.Count(), 0u);
        stamps.byAddress[address] = 3;
        CHECK_EQ(table.Find(address, stamps), 0u);
        CHECK_EQ(stamps.asked, 0u); // nothing is stored, so a bind finds an empty slot and asks nothing

        // a null address or a zero id is not a record either
        CHECK(SeenInsert::Invalid == table.Insert(0, 3, kAmbientAnchors));
        CHECK(SeenInsert::Invalid == table.Insert(address, 0, kAmbientAnchors));
        CHECK_EQ(table.Count(), 0u);
        CHECK_EQ(table.Find(0, stamps), 0u);
    }

    // ---- the full-table line exists and says what happens, once
    {
        const std::string line = FormatIndexFull();
        CHECK(line.find("the shader index is full") != std::string::npos);
        CHECK(line.find("left alone") != std::string::npos);
    }

    return Finish("engine_pass_seen");
}
