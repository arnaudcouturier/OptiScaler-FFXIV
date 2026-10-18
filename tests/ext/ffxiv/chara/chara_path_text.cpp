// ext::PathText (ext/core/PathText.h): a path as UTF-8 text for log lines. std::filesystem::path::string() converts through the ANSI
// code page and throws for a folder name the code page cannot represent; a log call evaluates its arguments in the caller, so the
// throw would leave the game's Draw hook (REVIEW-NR-2 H1). PathText never throws.
#include "check.h"
#include "ext/core/PathText.h"

#include <cstdio>
#include <string>

namespace
{
void TestNonAnsiPaths()
{
    // A Cyrillic folder and a Japanese one: not representable in the Western ANSI code page.
    const std::filesystem::path russian = L"C:\\Games\\\u0418\u0433\u0440\u0430\\OptiScaler";
    const std::filesystem::path japanese = L"D:\\\u30B2\u30FC\u30E0\\FINAL FANTASY XIV";
    const std::filesystem::path mixed = L"E:\\\u0418\u0433\u0440\u0430\\\u30B2\u30FC\u30E0\\nvngx.dll_dlssnr.dll";

    CHECK(ext::PathText(russian) == "C:\\Games\\\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0\\OptiScaler");
    CHECK(ext::PathText(japanese) == "D:\\\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\\FINAL FANTASY XIV");
    CHECK(ext::PathText(mixed) == "E:\\\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0\\\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\\nvngx.dll_dlssnr.dll");

    // Plain ASCII and the empty path come back as they are.
    CHECK(ext::PathText(std::filesystem::path(L"C:\\Games\\FFXIV")) == "C:\\Games\\FFXIV");
    CHECK(ext::PathText(std::filesystem::path()).empty());

    // A lone surrogate is not valid UTF-16: no exception, some text.
    const std::filesystem::path broken = std::wstring(L"C:\\x") + wchar_t(0xD800) + L"y";
    const std::string brokenText = ext::PathText(broken);
    CHECK(!brokenText.empty());

    // The old call: on a machine whose ANSI code page cannot represent these names it throws. (On a machine with a UTF-8 or Japanese
    // code page it does not, so this is reported, not required.)
    bool oldCallThrows = false;
    try
    {
        (void) japanese.string();
        (void) russian.string();
    }
    catch (const std::exception&)
    {
        oldCallThrows = true;
    }
    std::printf("note: ANSI code page %u, path::string() on the non-ANSI names %s\n", GetACP(),
                oldCallThrows ? "throws (the case this helper fixes)" : "does not throw here");
}
} // namespace

int main()
{
    TestNonAnsiPaths();
    return TEST_RESULT("chara_path_text");
}
