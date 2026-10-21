// [DlssNr] PauseOnFocusLoss is another name for DisableOnFocusLoss (a key of another build). NrConfig::PostLoad moves it into
// DisableOnFocusLoss once, right after the ini was read, and clears it, so the next Save writes `auto` for the alias and the real
// key. Resolving the alias at use time instead would bring the pause back after the user unticked the box (the key saves an
// explicit false as `auto`, the alias line would still say true). The migration is pure (ext/nr/NrFocusKey.h); the real
// CustomOptional (sliced from OptiScaler\Config.h by the Prepare script) shows what Save would write.
#include "Config.h" // the sliced real CustomOptional, from the build folder
#include "ext/nr/NrFocusKey.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
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

// The smallest thing the template needs: has_value / value / operator= / reset.
struct FakeOpt
{
    bool set = false;
    bool v = false;
    bool has_value() const { return set; }
    bool value() const { return v; }
    FakeOpt& operator=(bool x)
    {
        set = true;
        v = x;
        return *this;
    }
    void reset() { set = false; }
};

FakeOpt Opt(int state) // -1 unset, 0 false, 1 true
{
    FakeOpt o;
    if (state >= 0)
        o = (state == 1);
    return o;
}

void Table()
{
    struct Row
    {
        int key, alias; // before: -1 unset, 0 false, 1 true
        int keyAfter;   // after
    };
    const Row rows[] = {
        { -1, 1, 1 },   // only the alias, true: the pause is on
        { -1, 0, 0 },   // only the alias, false: the key holds an explicit false
        { 0, 1, 0 },    // an explicit false of the key beats the alias
        { 1, 0, 1 },    // an explicit true of the key beats the alias
        { 1, 1, 1 },
        { 0, 0, 0 },
        { -1, -1, -1 }, // neither: stays unset
        { 0, -1, 0 },
        { 1, -1, 1 },
    };
    for (const Row& r : rows)
    {
        FakeOpt key = Opt(r.key), alias = Opt(r.alias);
        ext::nr::MigrateFocusAlias(key, alias);
        CHECK(key.has_value() == (r.keyAfter >= 0));
        if (r.keyAfter >= 0)
            CHECK(key.value() == (r.keyAfter == 1));
        CHECK(!alias.has_value()); // cleared in every case

        ext::nr::MigrateFocusAlias(key, alias); // a second load changes nothing
        CHECK(key.has_value() == (r.keyAfter >= 0));
        if (r.keyAfter >= 0)
            CHECK(key.value() == (r.keyAfter == 1));
        CHECK(!alias.has_value());
    }
}

// The members as NrConfig declares them: the key with a default (an explicit false is not saved), the alias without one.
void WithTheRealOptional()
{
    // Ini holds only PauseOnFocusLoss=true.
    {
        CustomOptional<bool> key { false };
        CustomOptional<bool, NoDefault> alias;
        key.set_from_config(std::nullopt);
        alias.set_from_config(true);
        ext::nr::MigrateFocusAlias(key, alias);
        CHECK(key.value_or_default());
        CHECK(key.value_for_config() == std::optional<bool>(true)); // Save writes DisableOnFocusLoss=true
        CHECK(!alias.value_for_config().has_value());               // and PauseOnFocusLoss=auto
    }
    // The user unticks the box in the menu in that session, then Saves; the next launch starts from the saved ini.
    {
        CustomOptional<bool> key { false };
        CustomOptional<bool, NoDefault> alias;
        alias.set_from_config(true);
        ext::nr::MigrateFocusAlias(key, alias);
        key = false; // the checkbox
        CHECK(!key.value_or_default());
        CHECK(!key.value_for_config().has_value());   // saved as auto
        CHECK(!alias.value_for_config().has_value()); // the alias line no longer says true

        CustomOptional<bool> next { false }; // next launch: both lines are `auto`
        CustomOptional<bool, NoDefault> nextAlias;
        next.set_from_config(std::nullopt);
        nextAlias.set_from_config(std::nullopt);
        ext::nr::MigrateFocusAlias(next, nextAlias);
        CHECK(!next.value_or_default());
    }
    // Both lines in the ini, the key says false: the key wins and the alias is gone from the next Save.
    {
        CustomOptional<bool> key { false };
        CustomOptional<bool, NoDefault> alias;
        key.set_from_config(false);
        alias.set_from_config(true);
        ext::nr::MigrateFocusAlias(key, alias);
        CHECK(!key.value_or_default());
        CHECK(!alias.value_for_config().has_value());
    }
    // Both lines, the key says true: nothing changes.
    {
        CustomOptional<bool> key { false };
        CustomOptional<bool, NoDefault> alias;
        key.set_from_config(true);
        alias.set_from_config(false);
        ext::nr::MigrateFocusAlias(key, alias);
        CHECK(key.value_or_default() && key.value_for_config() == std::optional<bool>(true));
        CHECK(!alias.value_for_config().has_value());
    }
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end()); // the sources are CRLF
    return text;
}

size_t Count(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
        ++n;
    return n;
}

void Wiring(const std::string& repo)
{
    const std::string extDir = repo + "/OptiScaler/ext/";
    const std::string header = ReadFile(extDir + "nr/NrConfig.h");
    const std::string cfg = ReadFile(extDir + "nr/NrConfig.cpp");
    CHECK(!header.empty() && !cfg.empty());

    // the alias member has no default (an explicit value must survive being read), the key keeps its false default
    CHECK(header.find("CustomOptional<bool, NoDefault> pauseOnFocusLoss;") != std::string::npos);
    CHECK(header.find("CustomOptional<bool> disableOnFocusLoss { false };") != std::string::npos);
    CHECK(cfg.find("b.Add(\"DlssNr\", \"PauseOnFocusLoss\", pauseOnFocusLoss);") != std::string::npos);
    CHECK(cfg.find("b.Add(\"DlssNr\", \"DisableOnFocusLoss\", disableOnFocusLoss);") != std::string::npos);

    // migrated at load, in PostLoad
    const size_t postLoad = cfg.find("void NrConfig::PostLoad()");
    CHECK(postLoad != std::string::npos);
    CHECK(cfg.find("nr::MigrateFocusAlias(disableOnFocusLoss, pauseOnFocusLoss);", postLoad) != std::string::npos);
    CHECK(cfg.find("#include \"ext/nr/NrFocusKey.h\"") != std::string::npos);

    // nothing but the config reads the alias: the pass and the menu keep reading DisableOnFocusLoss
    for (const char* file : { "nr/pass/DlssNr_Dx12.cpp", "nr/menu/DlssNr_Menu.cpp", "nr/NrFocus.cpp", "nr/NrFocusGate.h" })
        CHECK(ReadFile(extDir + file).find("pauseOnFocusLoss") == std::string::npos);
    CHECK(Count(cfg, "pauseOnFocusLoss") == 2); // the binder line and the one migration call

    // the ini line and its words
    const std::string ini = ReadFile(extDir + "ini/DlssNr.ini");
    const size_t at = ini.find("\nPauseOnFocusLoss=auto\n");
    const size_t dis = ini.find("\nDisableOnFocusLoss=auto\n");
    CHECK(dis != std::string::npos && at != std::string::npos && dis < at);
    if (dis != std::string::npos && at != std::string::npos && dis < at)
    {
        const std::string note = ini.substr(dis, at - dis);
        CHECK(note.find("Another name for DisableOnFocusLoss") != std::string::npos);
        CHECK(note.find("DisableOnFocusLoss wins when both are set") != std::string::npos);
        CHECK(note.find("No menu control") != std::string::npos);
    }

    // ini-only in the menu manifest (menu_key_controls needs it: the key has no control)
    CHECK(ReadFile(extDir + "core/MenuManifest.h").find("\"[DlssNr] PauseOnFocusLoss\"") != std::string::npos);
}
} // namespace

int main(int argc, char** argv)
{
    Table();
    WithTheRealOptional();
    Wiring(argc > 1 ? argv[1] : ".");

    if (failures == 0)
        std::printf("nr_focus_key: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
