// The NR time readout: two decimals and the unit when a time is known, "n/a" when it is not.
#include "ext/nr/NrReadout.h"

#include <cstdio>
#include <limits>
#include <optional>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)
} // namespace

int main()
{
    using DlssNr::NrTimeText;
    CHECK(NrTimeText(4.2134) == "NR time: 4.21 ms");
    CHECK(NrTimeText(4.2166) == "NR time: 4.22 ms");
    CHECK(NrTimeText(0.004) == "NR time: 0.00 ms");   // any positive time is a measurement
    CHECK(NrTimeText(12.5) == "NR time: 12.50 ms");
    CHECK(NrTimeText(123.456) == "NR time: 123.46 ms");
    // not measured yet
    CHECK(NrTimeText(std::nullopt) == "NR time: n/a");
    CHECK(NrTimeText(0.0) == "NR time: n/a" && NrTimeText(-0.0) == "NR time: n/a");
    CHECK(NrTimeText(-1.0) == "NR time: n/a");
    CHECK(NrTimeText(std::numeric_limits<double>::quiet_NaN()) == "NR time: n/a");
    CHECK(NrTimeText(std::numeric_limits<double>::infinity()) == "NR time: n/a");
    CHECK(NrTimeText(-std::numeric_limits<double>::infinity()) == "NR time: n/a");
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: NR time text (two decimals, n/a when not measured)");
    return 0;
}
