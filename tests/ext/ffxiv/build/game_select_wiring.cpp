// Wiring pins of the game selector's name-recognised-build warning (ext/game/Game.cpp, SelectGame). The selector itself
// reads the live executable and keeps process statics, so no unit test can drive it; these pins keep the structure that
// makes the warning true: on a build the table does not know but the exe name recognises, the game-module seams that read
// no executable address stay on while the address-reading ones stay off, and the upstream exe-name FG quirk stays active
// with them. That silent mismatch gets exactly one warning naming the quirk and both halves, so a player on a new game
// build can report it. Source text only, no git inside a test.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

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

std::string Squash(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

std::string Read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return Squash(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string game = Read(repo + "/OptiScaler/ext/game/Game.cpp");
    CHECK(game.size() > 1000);

    // The exact build logs the match; any other recognised exe logs the name-recognised rule (seams without an
    // executable address on, the rest following the gate lines). The fork is on the stamp-and-size match alone.
    CHECK(Count(game, "if(exactBuild)LOG_INFO(\"MatchedsupportedFFXIVbuildforgame-moduleseams\");") == 1);
    CHECK(Count(game, "elseLOG_INFO(\"FFXIVexecutablerecognisedbyname") == 1);
    // The warning only where the name recognised the build AND the quirk bit is set: an exact build never warns,
    // and without the bit there is nothing to warn about. A dropped conjunct warns on every launch or never.
    CHECK(Count(game, "if(!exactBuild&&(State::Instance().gameQuirks&GameQuirk::ReleaseBackbufferOnce))") == 1);
    CHECK(Count(game, "LOG_WARN(\"ReleaseBackbufferOncequirkisactivebyexenameonagamebuildtheresolverdoesnotknow:"
                      "theFGbackbufferisstillreleasedoncewhiletheaddress-readingseamsstayoff\");") == 1);
    CHECK(Count(game, "ReleaseBackbufferOnce") == 2); // the condition and the warning: nowhere else
    // The warning comes after the name-recognised line it explains, inside the same not-exact branch.
    {
        const size_t info = game.find("elseLOG_INFO(\"FFXIVexecutablerecognisedbyname");
        const size_t warn = game.find("LOG_WARN(\"ReleaseBackbufferOncequirkisactive");
        CHECK(info != std::string::npos && warn != std::string::npos && info < warn);
    }

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: game select wiring (exact-build match, name-recognised rule, exe-name quirk warning)");
    return 0;
}
