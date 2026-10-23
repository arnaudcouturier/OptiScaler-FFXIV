// "Linear light + composed" is ReversibleMode 5 (docs/RECONSTRUCTION-REFERENCE.md, F3). Saved NR presets validate the
// field against its range, so the range has to include 5 and nothing above it. Uses the real ext::NrConfig and the
// real preset code, like nr_user_presets.
#include "ext/nr/NrConfig.h"
#include "ext/nr/presets/NrUserPresets.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace DlssNr::UserPresets;

    ext::NrConfig c;
    assert(c.reversibleMode.value_or_default() == 0); // soft knee stays the default

    // Every mode, the new one included, survives a capture / apply round trip.
    for (uint32_t mode = 0; mode <= 5; ++mode)
    {
        c.reversibleMode = mode;
        const auto saved = Capture(c);
        assert(saved.at("ReversibleMode") == mode);

        ext::NrConfig loaded;
        Apply(saved, loaded);
        assert(loaded.reversibleMode.value_or_default() == mode);
    }

    // One past the last mode is rejected, atomically: the config keeps what it had.
    c.reversibleMode = 5u;
    const auto before = Capture(c);
    for (const auto& outOfRange : { Json(6), Json(-1), Json(5.5), Json("5") })
    {
        auto broken = before;
        broken["ReversibleMode"] = outOfRange;
        bool failed = false;
        try { Apply(broken, c); } catch (...) { failed = true; }
        assert(failed && Capture(c) == before && c.reversibleMode.value_or_default() == 5);
    }

    std::cout << "PASS: ReversibleMode 0..5 round-trips through saved presets, 6 and non-integers are rejected\n";
    return 0;
}
