// [Menu] Style parsing: classic | ember, anything else is classic (fail open: a typo never changes the look).
#include "ext/menu/MenuStyle.h"

#include <cstdio>
#include <string_view>

using namespace ext::menustyle;

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
    CHECK(Parse("ember") == Style::Ember);
    CHECK(Parse("classic") == Style::Classic);

    // Case and surrounding blanks do not matter (an ini value may carry a CR).
    CHECK(Parse("EMBER") == Style::Ember);
    CHECK(Parse("Ember") == Style::Ember);
    CHECK(Parse("  ember\r\n") == Style::Ember);

    // The shipped template carries "auto", which is unset: classic. Garbage and look-alikes never switch the style.
    CHECK(Parse("auto") == Style::Classic);
    CHECK(Parse("") == Style::Classic);
    CHECK(Parse("true") == Style::Classic);
    CHECK(Parse("1") == Style::Classic);
    CHECK(Parse("embers") == Style::Classic);
    CHECK(Parse("em ber") == Style::Classic);
    CHECK(Parse("ember2") == Style::Classic);

    CHECK(std::string_view(Name(Style::Classic)) == "classic");
    CHECK(std::string_view(Name(Style::Ember)) == "ember");
    CHECK(Parse(Name(Style::Ember)) == Style::Ember);
    CHECK(Parse(Name(Style::Classic)) == Style::Classic);

    // The default style is the first enumerator: classic.
    CHECK(Style {} == Style::Classic);

    return g_failed ? 1 : 0;
}
