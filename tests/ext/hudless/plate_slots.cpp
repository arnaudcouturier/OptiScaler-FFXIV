// The three plate slots (ext/hudless/PlateSlots.h) without a GPU: reservation only hands out a slot that is free (not
// drawable, not staged, not reserved), unpinned and done being read; the free rule checks every fence; a stale
// reservation is reclaimed; pins belong to the slot generation they were taken in.
#include <windows.h>

#include "ext/hudless/PlateSlots.h"

#include <cstdio>
#include <map>

using namespace ext::hudless::plates;
using ext::hudless::FenceReader;

namespace
{
int g_failures = 0;

#define EXPECT(cond, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("    FAIL line %d: %s  ", __LINE__, #cond);                                                    \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

// Fence completion the test decides: a fence value is completed up to the number stored for it.
class FakeFences : public FenceReader
{
  public:
    std::map<const void*, uint64_t> completed;
    void Set(const void* fence, uint64_t value) { completed[fence] = value; }
    uint64_t Completed(const void* fence) override
    {
        const auto at = completed.find(fence);
        return at == completed.end() ? 0 : at->second;
    }
};

static int g_fenceA = 0;
static int g_fenceB = 0;
static int g_fenceC = 0;
const void* const kFenceA = &g_fenceA;
const void* const kFenceB = &g_fenceB;
const void* const kFenceC = &g_fenceC;

SlotState StateOf(SlotTable& table, int slot) { return table.State(slot); }

void Free(SlotTable& table, int slot)
{
    table.Release(slot); // Reserved/Staged that never finished
}
} // namespace

int main()
{
    // 1. Round robin over three free slots, then nothing: reservation never reuses a live slot
    {
        FakeFences fences;
        SlotTable table;
        const int a = table.Reserve(0, 1000, 1, fences);
        const int b = table.Reserve(0, 1000, 1, fences);
        const int c = table.Reserve(0, 1000, 1, fences);
        EXPECT(a == 0 && b == 1 && c == 2, "three reservations in order: %d %d %d", a, b, c);
        EXPECT(table.Reserve(0, 1000, 1, fences) == -1, "a fourth frame gets no slot");
        EXPECT(StateOf(table, 0) == SlotState::Reserved && StateOf(table, 1) == SlotState::Reserved &&
                   StateOf(table, 2) == SlotState::Reserved,
               "all three are reserved");
    }

    // 2. States follow the frame: Reserved -> Staged -> Drawable -> Draining -> Free; only Free can be reserved
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(1, 1000, 1, fences);
        EXPECT(slot == 1, "round robin starts at the hint");
        EXPECT(!table.Promote(slot), "a reserved slot cannot be promoted straight to drawable");
        EXPECT(table.Stage(slot), "a reserved slot stages");
        EXPECT(!table.Stage(slot), "a staged slot does not stage twice");
        EXPECT(table.Promote(slot), "a staged slot is promoted");
        EXPECT(table.Reserve(1, 1000, 2, fences) == 2, "a drawable slot is not handed out again");
        EXPECT(table.Retire(slot), "a drawable slot retires");
        EXPECT(StateOf(table, slot) == SlotState::Draining, "the retired slot drains");
        table.FreeDrained(3, fences);
        EXPECT(StateOf(table, slot) == SlotState::Free, "nothing reads it: free");
        EXPECT(table.Reserve(1, 1000, 4, fences) == 1, "the drained slot is handed out again");
    }

    // 3. A slot with an outstanding reader fence is not reserved, and becomes free when the fence completes
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 1000, 1, fences);
        table.Stage(slot);
        table.Promote(slot);
        EXPECT(table.AddReader(slot, kFenceA, 7, 1), "the produce fence is recorded");
        fences.Set(kFenceA, 6);
        table.Retire(slot);
        table.FreeDrained(2, fences);
        EXPECT(StateOf(table, slot) == SlotState::Draining, "a pending read keeps the slot out of service");
        EXPECT(table.Reserve(0, 1000, 2, fences) != slot, "the slot is not handed out while it is read");
        fences.Set(kFenceA, 7);
        table.FreeDrained(3, fences);
        EXPECT(StateOf(table, slot) == SlotState::Free, "the completed read frees the slot");
        EXPECT(table.Reserve(0, 1000, 3, fences) == slot, "and it is handed out again");
    }

    // 4. A pinned slot is never reserved, even after its readers completed: the pin is the pass's proof it reads it
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 0, 1, fences);
        table.Stage(slot);
        table.Promote(slot);
        const SlotTable::Pin pin = table.TakePin(slot);
        EXPECT(static_cast<bool>(pin), "the pass pins the drawable slot");
        EXPECT(table.Pins(slot) == 1, "one pin");
        table.Retire(slot); // a newer record replaced it while the pass still reads it
        table.FreeDrained(2, fences);
        EXPECT(StateOf(table, slot) == SlotState::Draining, "a pinned slot does not drain");
        EXPECT(table.Reserve(0, 0, 2, fences) != slot, "a pinned slot is not handed out");
        EXPECT(table.DropPin(pin), "the pin is dropped");
        table.FreeDrained(3, fences);
        EXPECT(StateOf(table, slot) == SlotState::Free, "after the pin and the fences the slot is free");
    }

    // 5. Only a drawable or draining slot can be pinned: a reservation is not readable by the pass
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 0, 1, fences);
        EXPECT(!static_cast<bool>(table.TakePin(slot)), "a reserved slot cannot be pinned");
        table.Stage(slot);
        EXPECT(!static_cast<bool>(table.TakePin(slot)), "a staged slot cannot be pinned");
        EXPECT(!static_cast<bool>(table.TakePin(7)), "an invalid slot cannot be pinned");
    }

    // 6. A stale reservation is reclaimed after 500 ms; a stale drawable one is not (the outputs may draw it)
    {
        FakeFences fences;
        SlotTable table;
        const int a = table.Reserve(0, 1000, 1, fences);
        table.Stage(a);
        EXPECT(table.Reserve(1, 1499, 2, fences) == 1, "at 499 ms the stale slot is still out of service");
        EXPECT(table.StaleReclaims() == 0, "nothing reclaimed yet");
        const int again = table.Reserve(1, 1500, 3, fences);
        EXPECT(again == 2, "at 500 ms the stale slot is reclaimed and the next free one handed out: %d", again);
        EXPECT(table.StaleReclaims() == 1, "one reclaim counted");
        EXPECT(StateOf(table, 0) == SlotState::Free, "the stale reservation is free");
        EXPECT(StateOf(table, 1) == SlotState::Reserved && StateOf(table, 2) == SlotState::Reserved,
               "the live reservations are untouched");

        FakeFences done;
        SlotTable other;
        const int d = other.Reserve(0, 0, 1, done);
        other.Stage(d);
        other.Promote(d);
        EXPECT(other.Reserve(1, 10000, 2, done) == 1, "a drawable slot is never reclaimed");
        EXPECT(StateOf(other, d) == SlotState::Drawable, "the drawable slot keeps its state");
    }

    // 7. A released reservation is free at once (a frame that was declined after the reservation)
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 0, 1, fences);
        table.Stage(slot);
        Free(table, slot);
        EXPECT(StateOf(table, slot) == SlotState::Free, "a reservation that never finished is released");
        EXPECT(table.Reserve(0, 0, 2, fences) == slot, "and reserved again at once");
    }

    // 8. A pin belongs to its generation: a reset drops it, and the old pin cannot release the new slot
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 0, 1, fences);
        table.Stage(slot);
        table.Promote(slot);
        const SlotTable::Pin pin = table.TakePin(slot);
        const uint64_t generation = table.Generation();
        table.Reset();
        EXPECT(table.Generation() != generation, "a reset starts a new generation");
        EXPECT(table.Pins(slot) == 0, "the reset dropped the pin");
        EXPECT(!table.DropPin(pin), "the old pin does not release anything");
        EXPECT(table.Pins(slot) == 0, "still no pins after the stale unpin");
        EXPECT(StateOf(table, slot) == SlotState::Free, "a reset leaves every slot free");
        // A pass of the new generation pins the same slot: the old pin must not release the new one.
        const int again = table.Reserve(0, 0, 2, fences);
        table.Stage(again);
        table.Promote(again);
        const SlotTable::Pin fresh = table.TakePin(again);
        EXPECT(table.Pins(again) == 1, "the new pin is held");
        EXPECT(!table.DropPin(pin), "the pin from before the reset still releases nothing");
        EXPECT(table.Pins(again) == 1, "so the new pin is still held");
        EXPECT(table.DropPin(fresh), "and the new pin is dropped normally");
        Free(table, again); // the test needs the slot back for the loop below
        for (int i = 0; i < 3; ++i)
            EXPECT(table.Reserve(0, 0, 2, fences) >= 0, "every slot is reservable after the reset");
    }

    // 9. Pins add up: two passes on two threads, both must be dropped before the slot drains
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 0, 1, fences);
        table.Stage(slot);
        table.Promote(slot);
        const auto first = table.TakePin(slot);
        const auto second = table.TakePin(slot);
        EXPECT(table.Pins(slot) == 2, "two pins");
        table.Retire(slot);
        EXPECT(table.DropPin(first), "the first pin is dropped");
        table.FreeDrained(2, fences);
        EXPECT(StateOf(table, slot) == SlotState::Draining, "one pin still holds the slot");
        EXPECT(table.DropPin(second), "the second pin is dropped");
        table.FreeDrained(3, fences);
        EXPECT(StateOf(table, slot) == SlotState::Free, "with both pins gone the slot drains");
        EXPECT(!table.DropPin(second), "an extra unpin does not underflow");
        EXPECT(table.Pins(slot) == 0, "the pin count never goes below zero");
    }

    // 10. The reader table refuses a slot with too many fences instead of silently dropping one
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 0, 1, fences);
        int fences_[8] = {};
        bool all = true;
        for (int i = 0; i < 6; ++i)
            all = table.AddReader(slot, &fences_[i], 1, 1) && all;
        EXPECT(all, "six readers fit");
        EXPECT(!table.AddReader(slot, &fences_[7], 1, 1), "the seventh reader is refused");
        EXPECT(table.Readers(slot) == 6, "and not recorded");
        EXPECT(!table.AddReader(slot, nullptr, 1, 1), "a null fence is refused");
        table.DropReaders(slot);
        EXPECT(table.Readers(slot) == 0, "dropping the readers empties the slot");
    }

    // 11. A slot released while one of its readers is still pending is not handed out again
    {
        FakeFences fences;
        SlotTable table;
        const int slot = table.Reserve(0, 0, 1, fences);
        table.Stage(slot);
        EXPECT(table.AddReader(slot, kFenceA, 5, 1), "a reader of the frame's image is recorded");
        Free(table, slot); // the frame never finished and its slot goes back at once
        EXPECT(StateOf(table, slot) == SlotState::Free, "the released slot is free");
        EXPECT(table.Reserve(0, 0, 2, fences) != slot, "but the pending read keeps it out of service");
        fences.Set(kFenceA, 5);
        EXPECT(table.Reserve(0, 0, 3, fences) == slot, "and it is handed out once that read completed");
    }

    if (g_failures != 0)
    {
        std::printf("FAIL: %d check(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: plate slots: reservation excludes live slots, a pending reader holds a released slot, the free rule "
              "checks fences, stale reservations are reclaimed, pins belong to a generation, readers are tracked");
    return 0;
}
