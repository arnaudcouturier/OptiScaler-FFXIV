// The plate-draw rule (ext/ffxiv/plates/PlateClassify.h), with no device and no game: the whole state matrix, and the
// fail-closed property that makes a wrong condition cost the feature rather than a plate.
//
// Every condition is flipped on its own against an otherwise perfect plate draw and the rule has to refuse; the perfect
// state alone has to pass; a default-constructed state (a field a future reader forgot) has to refuse.
#include "ext/ffxiv/plates/PlateClassify.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace
{
using ext::ffxiv::plates::DrawState;
using ext::ffxiv::plates::IsPlateDraw;

// The state a real nameplate draw has, as the reader fills it in.
DrawState Plate()
{
    DrawState s;
    s.immediateContext = true;
    s.singleTarget = true;
    s.sdrTarget = true;
    s.singleSample = true;
    s.noUav = true;
    s.depthView = true;
    s.depthSameSize = true;
    s.depthTested = true;
    s.depthWriteOff = true;
    s.depthGreaterEqual = true;
    s.stencilOff = true;
    s.standardBlend = true;
    s.nameplateShader = true;
    s.nameplateLayout = true;
    return s;
}

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        assert(false);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)
} // namespace

int main()
{
    // The depth function the rule pins, and the two formats a plate slot is made of.
    static_assert(ext::ffxiv::plates::kDepthGreaterEqual == 7);
    static_assert(ext::ffxiv::plates::IsSdrFormat(87));  // DXGI_FORMAT_B8G8R8A8_UNORM
    static_assert(ext::ffxiv::plates::IsSdrFormat(28));  // DXGI_FORMAT_R8G8B8A8_UNORM
    static_assert(!ext::ffxiv::plates::IsSdrFormat(10)); // R16G16B16A16_FLOAT
    static_assert(!ext::ffxiv::plates::IsSdrFormat(24)); // R10G10B10A2_UNORM
    static_assert(!ext::ffxiv::plates::IsSdrFormat(0));  // UNKNOWN

    // A perfect draw passes, and so does the reader's order of conditions: nothing is required twice.
    CHECK(IsPlateDraw(Plate()));

    // Fail closed: a zeroed state (a field nobody filled in) is a native draw.
    CHECK(!IsPlateDraw(DrawState {}));
    // ... and so is a state with every field set except one: each condition is load bearing on its own.
    {
        struct Condition
        {
            const char* name;
            void (*flip)(DrawState&);
        };
        const Condition conditions[] = {
            { "immediate context", [](DrawState& s) { s.immediateContext = false; } },
            { "one colour target", [](DrawState& s) { s.singleTarget = false; } },
            { "8-bit SDR target", [](DrawState& s) { s.sdrTarget = false; } },
            { "single sample", [](DrawState& s) { s.singleSample = false; } },
            { "no UAV", [](DrawState& s) { s.noUav = false; } },
            { "depth view", [](DrawState& s) { s.depthView = false; } },
            { "depth size", [](DrawState& s) { s.depthSameSize = false; } },
            { "depth test", [](DrawState& s) { s.depthTested = false; } },
            { "no depth write", [](DrawState& s) { s.depthWriteOff = false; } },
            { "GREATER_EQUAL", [](DrawState& s) { s.depthGreaterEqual = false; } },
            { "no stencil", [](DrawState& s) { s.stencilOff = false; } },
            { "standard blend", [](DrawState& s) { s.standardBlend = false; } },
            { "nameplate pixel shader", [](DrawState& s) { s.nameplateShader = false; } },
            { "nameplate input layout", [](DrawState& s) { s.nameplateLayout = false; } },
        };
        int checked = 0;
        for (const Condition& condition : conditions)
        {
            DrawState s = Plate();
            condition.flip(s);
            if (IsPlateDraw(s))
                std::printf("FAIL: a draw that fails '%s' was taken for a plate draw\n", condition.name);
            CHECK(!IsPlateDraw(s));
            ++checked;
        }
        CHECK(checked == 14);
    }

    // Two conditions missing at once still refuse (the conjunction does not hide a single failure).
    {
        DrawState s = Plate();
        s.stencilOff = false;
        s.standardBlend = false;
        CHECK(!IsPlateDraw(s));
    }

    std::puts("PASS: plate rule refuses a draw that fails any one of its 14 conditions and accepts the perfect state");
    return 0;
}
