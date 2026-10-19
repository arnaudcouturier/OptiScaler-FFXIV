// The "Window mode (FFXIV)" menu card is ImGui glue and cannot run offline: this pins what it must do by reading the source
// (comments and string literals stripped): the [Window] Mode key is only written from its combo with one of the three known
// names, the card shows the ACTIVE mode from the published copy (a change needs a restart), it is registered in the FFXIV
// tab with the FFXIV visibility rule, and it holds no decision of its own.
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

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Removes // comments, /* */ comments and the CONTENT of string literals (the quotes stay).
std::string Strip(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size();)
    {
        if (s.compare(i, 2, "//") == 0)
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
        }
        else if (s.compare(i, 2, "/*") == 0)
        {
            const size_t e = s.find("*/", i + 2);
            i = e == std::string::npos ? s.size() : e + 2;
        }
        else if (s[i] == '"')
        {
            out += '"';
            ++i;
            while (i < s.size() && s[i] != '"')
                i += (s[i] == '\\' && i + 1 < s.size()) ? 2 : 1;
            out += '"';
            ++i;
        }
        else
            out += s[i++];
    }
    return out;
}

size_t Count(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

std::string Squash(const std::string& s)
{
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string raw = ReadFile(repo + "/OptiScaler/ext/window/WindowMenu.cpp");
    CHECK(!raw.empty());
    const std::string code = Squash(Strip(raw));

    // the key is written exactly once, from the combo, with one of the three known names
    CHECK(Count(code, "cfg.mode=") == 1);
    CHECK(Count(code, "if(ImGui::Combo(\"\",&index,\"\"))cfg.mode=std::string(kNames[index]);") == 1);
    CHECK(raw.find("\"off\", \"probe\", \"on\"") != std::string::npos);
    CHECK(Count(code, "constexprconstchar*kNames[]={\"\",\"\",\"\"};") == 1);
    // the stored mode is read through the parser (anything unknown reads as off), the active one from the published copy
    CHECK(Count(code, "constModestored=cfg.GetMode();") == 1);
    CHECK(Count(code, "if(ParseMode(kNames[i])==stored)index=i;") == 1);
    CHECK(Count(code, "ModeName(CurrentMode())") >= 1);
    CHECK(Count(code, "if(CurrentMode()!=stored)") == 1);
    // registered in the FFXIV tab (its place comes from the layout table), visible only for the FFXIV build entry
    CHECK(Count(code, "ext::menu::AutoRegisterregistration{{\"\",\"\",\"\",Draw,\"\",Visible}};") == 1);
    CHECK(raw.find("{ \"window_mode\", \"FFXIV\", ") != std::string::npos);
    CHECK(raw.find("\"fg_dlssg\"") != std::string::npos);
    CHECK(raw.find("\"window_mode\"") != std::string::npos);
    CHECK(Count(code, "returnext::game::Current().lighting!=nullptr;") == 1);
    // the card decides nothing and never publishes a mode itself (the mode is published once at config load)
    CHECK(code.find("Publish") == std::string::npos);
    CHECK(code.find("Controller") == std::string::npos);

    // the stripper itself: a pin kept alive only by a comment or a string must not count
    CHECK(Squash(Strip("a // cfg.mode = x;\nb")).find("cfg.mode") == std::string::npos);
    CHECK(Squash(Strip("a /* cfg.mode = x; */ b")).find("cfg.mode") == std::string::npos);
    CHECK(Squash(Strip("a \"cfg.mode = x;\" b")).find("cfg.mode") == std::string::npos);

    if (failures)
    {
        std::printf("window_menu_text: %d failure(s)\n", failures);
        return 2;
    }
    std::printf("window_menu_text: all passed\n");
    return 0;
}
