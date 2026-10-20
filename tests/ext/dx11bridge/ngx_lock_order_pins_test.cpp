// [REL] The premise of the lock order (ext/dx11bridge/NgxFeatureTable.h): the table lock is a leaf because NR and the frame
// generation module run inside NGX / submission hooks and never call back into the DX11 feature table. The unit tests of the
// table prove its own types do not nest locks; whether the real NR lock (`g_nrMutex`, DlssNr_Dx12.cpp) or the FG code could reach the
// table is a property of the source, so it is pinned here: no code token under ext/nr or ext/fg (comments and string literals
// stripped) names the table, its pin, its locked setters, the release seam or the DX11 NGX feature entry points.
//
// The scan is negative-controlled: the same scanner must find each forbidden token in a synthetic file.
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

static std::string Read(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Code only: comments and the contents of string/char literals replaced by spaces.
static std::string CodeOnly(const std::string& s)
{
    std::string out(s.size(), ' ');
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '"' || s[i] == '\'')
        {
            const char q = s[i];
            for (++i; i < s.size() && s[i] != q; ++i)
                if (s[i] == '\\')
                    ++i;
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
        }
        else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
        {
            for (i += 2; i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'); ++i)
                ;
            ++i;
        }
        else
            out[i] = s[i];
    }
    return out;
}

static bool HasToken(const std::string& code, const std::string& token)
{
    for (size_t p = code.find(token); p != std::string::npos; p = code.find(token, p + 1))
    {
        const bool before = p == 0 || !(std::isalnum(static_cast<unsigned char>(code[p - 1])) || code[p - 1] == '_');
        const size_t e = p + token.size();
        const bool after = e >= code.size() || !(std::isalnum(static_cast<unsigned char>(code[e])) || code[e] == '_');
        if (before && after)
            return true;
    }
    return false;
}

static const std::vector<std::string>& Forbidden()
{
    static const std::vector<std::string> tokens = {
        "Dx11Contexts",   "ContextTable",       "EvaluatePin",         "TableGuard",
        "EvalRegistry",   "SetCurrentFeature",  "ClearCurrentFeatureIf", "BeforeReleaseFeature",
        "ShouldSkipReleaseSleep", "NVSDK_NGX_D3D11_CreateFeature", "NVSDK_NGX_D3D11_EvaluateFeature", "NVSDK_NGX_D3D11_ReleaseFeature",
    };
    return tokens;
}

// the forbidden includes are checked on the raw text (an include path is a string literal)
static bool IncludesTable(const std::string& raw) { return raw.find("NgxFeatureTable.h") != std::string::npos || raw.find("ReleaseSequence.h") != std::string::npos; }

int main(int argc, char** argv)
{
    const fs::path repo = argc > 1 ? argv[1] : ".";

    // negative controls: the scanner sees a token in code, ignores it in a comment and in a string
    for (const auto& token : Forbidden())
    {
        CHECK(HasToken(CodeOnly("void f() { " + token + "(); }"), token));
        CHECK(!HasToken(CodeOnly("// " + token + "\nconst char* s = \"" + token + "\";"), token));
        CHECK(!HasToken(CodeOnly("int My" + token + "Suffix = 0;"), token));
    }

    size_t scanned = 0;
    for (const char* dir : { "OptiScaler/ext/nr", "OptiScaler/ext/fg" })
    {
        for (const auto& entry : fs::recursive_directory_iterator(repo / dir))
        {
            const auto ext = entry.path().extension().string();
            if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".inl"))
                continue;
            ++scanned;
            const auto raw = Read(entry.path());
            const auto code = CodeOnly(raw);
            for (const auto& token : Forbidden())
                if (HasToken(code, token))
                {
                    std::printf("FAIL: %s names %s\n", entry.path().string().c_str(), token.c_str());
                    ++g_failed;
                }
            if (IncludesTable(raw))
            {
                std::printf("FAIL: %s includes the DX11 feature table headers\n", entry.path().string().c_str());
                ++g_failed;
            }
        }
    }
    CHECK(scanned > 40); // the scan saw the NR and FG sources (it would pass vacuously on an empty scan)

    if (g_failed == 0)
        std::printf("ngx_lock_order_pins: all checks passed (%zu files)\n", scanned);
    return g_failed;
}
