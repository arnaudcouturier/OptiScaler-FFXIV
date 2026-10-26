#include "../../../OptiScaler/ext/ffxiv/companion/CompanionPackets.h"

#include <cstdio>
#include <cstring>
#include <string>

// The companion code reads ext::ffxiv::ActiveBuild() (the resolved build), not the compile-time table. Before the startup publishes anything
// ActiveBuild() IS the table (so every other companion test is unchanged); after it, a healed offset is followed and an offset outside the 64-byte
// header refuses the packet.
//   argv[1] = healed : publish a build whose batch format offset moved from 0x30 to 0x34, check both before and after;
//   argv[1] = oob    : publish a build whose format offset is 0x3d (4 bytes would end past the header): no packet is supported, nothing is read outside.
using namespace ext::ffxiv::companion::Packets;

namespace
{
int failures = 0;
#define CHECK(x) ((x) ? (void) 0 : (std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures, (void) 0))

// A header that is supported when the format word is read from `formatAt`: kind 0x22, extension 0, 64 vertices, a vertices pointer, layout 4 there.
Packet MakePacket(unsigned formatAt, unsigned layout, unsigned otherAt, unsigned otherLayout)
{
    Packet p;
    p.original = 0x1000;
    std::uint32_t kind = 0x22, count = 64;
    std::uintptr_t vertices = 0x12345000;
    std::memcpy(p.header.data() + 0x00, &kind, 4);
    std::memcpy(p.header.data() + 0x20, &count, 4);
    std::memcpy(p.header.data() + 0x28, &vertices, 8);
    std::memcpy(p.header.data() + formatAt, &layout, 4);
    std::memcpy(p.header.data() + otherAt, &otherLayout, 4);
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "healed";
    using namespace ext::ffxiv;

    // before the startup: the table
    CHECK(!ActiveStarted());
    CHECK(&ActiveBuild() == &kCurrentBuild);
    CHECK(UiModuleGlobalRva() == kCurrentBuild.companion.nameplateModulePointerRva);
    CHECK(VertexSlotTableRva() == kCurrentBuild.companion.layoutSlotTableRva);
    CHECK(kCurrentBuild.companion.batchFormatOffset == 0x30);
    const Packet atTable = MakePacket(0x30, 4, 0x34, 3); // valid layout where the table reads it, an invalid one at 0x34
    const Packet atHealed = MakePacket(0x34, 4, 0x30, 3);
    CHECK(Supported(atTable) && !Supported(atHealed));

    GameBuild build = kCurrentBuild;
    if (mode == "healed")
    {
        build.companion.batchFormatOffset = 0x34;
        build.companion.nameplateModulePointerRva = 0x1234560;
        build.companion.layoutSlotTableRva = 0x765430;
    }
    else
        build.companion.batchFormatOffset = 0x3d; // 0x3d + 4 > 64: outside the header
    resolve::GateReport gate; // the gate content does not matter here: only the build is read by these functions
    CHECK(PublishActive(build, gate));
    CHECK(ActiveStarted());

    if (mode == "healed")
    {
        CHECK(!Supported(atTable) && Supported(atHealed)); // the healed offset is followed
        CHECK(UiModuleGlobalRva() == 0x1234560 && VertexSlotTableRva() == 0x765430);
        CHECK(ActiveBuild().companion.batchExtensionOffset == kCurrentBuild.companion.batchExtensionOffset); // the rest is the table's
    }
    else
    {
        // a header offset outside the header: nothing is supported, whatever the bytes say, and no byte outside the 64-byte array is touched
        CHECK(!CompanionOffsetsInBounds(ActiveBuild().companion));
        CHECK(!Supported(atTable) && !Supported(atHealed));
    }
    if (failures == 0)
        std::printf("PASS: companion_active_build %s\n", mode.c_str());
    return failures == 0 ? 0 : 1;
}
