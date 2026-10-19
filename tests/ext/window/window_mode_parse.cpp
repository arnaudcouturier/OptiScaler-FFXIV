// [Window] Mode parsing: off | probe | on, anything else is off (fail open).
#include "ext/window/WindowMode.h"

#include <cstdio>
#include <string_view>

using namespace ext::window;

static int g_failed = 0;
#define CHECK(cond)                                                                                                         \
    do                                                                                                                      \
    {                                                                                                                       \
        if (!(cond))                                                                                                        \
        {                                                                                                                   \
            std::printf("CHECK failed line %d: %s\n", __LINE__, #cond);                                                     \
            ++g_failed;                                                                                                     \
        }                                                                                                                   \
    } while (0)

int main()
{
    CHECK(ParseMode("on") == Mode::On);
    CHECK(ParseMode("probe") == Mode::Probe);
    CHECK(ParseMode("off") == Mode::Off);

    // Case and surrounding blanks do not matter (an ini value may carry a CR).
    CHECK(ParseMode("ON") == Mode::On);
    CHECK(ParseMode("Probe") == Mode::Probe);
    CHECK(ParseMode("  on\r\n") == Mode::On);

    // The default of a CustomOptional is "auto"/"off"; garbage and look-alikes never switch the module on.
    CHECK(ParseMode("auto") == Mode::Off);
    CHECK(ParseMode("") == Mode::Off);
    CHECK(ParseMode("true") == Mode::Off);
    CHECK(ParseMode("1") == Mode::Off);
    CHECK(ParseMode("online") == Mode::Off);
    CHECK(ParseMode("pro be") == Mode::Off);

    CHECK(std::string_view(ModeName(Mode::Off)) == "off");
    CHECK(std::string_view(ModeName(Mode::Probe)) == "probe");
    CHECK(std::string_view(ModeName(Mode::On)) == "on");
    // The names round-trip.
    CHECK(ParseMode(ModeName(Mode::On)) == Mode::On);
    CHECK(ParseMode(ModeName(Mode::Probe)) == Mode::Probe);
    CHECK(ParseMode(ModeName(Mode::Off)) == Mode::Off);

    return g_failed ? 1 : 0;
}
