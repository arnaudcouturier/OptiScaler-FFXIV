// Two files with the same name in different folders, loaded by FULL PATH the way the forwarder loads the NR model
// (LoadLibraryExW + LOAD_WITH_ALTERED_SEARCH_PATH), must be two modules of one process. The NR runtime picker relies on it: it
// keeps every selected model loaded and switches by path. Also pins the caveat that a lookup by NAME returns the first one.
#include <windows.h>
#include <cstdio>
#include <string>

typedef int (*IdFn)();
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::printf("usage: nr_same_name_modules <dllA> <dllB>\n");
        return 2;
    }
    HMODULE a = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE b = LoadLibraryExW(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE aAgain = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);
    CHECK(aAgain == a); // the same full path is the same module (the forwarder caches by path)
    IdFn fa = a ? reinterpret_cast<IdFn>(GetProcAddress(a, "dlssnr_probe_id")) : nullptr;
    IdFn fb = b ? reinterpret_cast<IdFn>(GetProcAddress(b, "dlssnr_probe_id")) : nullptr;
    CHECK(fa != nullptr && fb != nullptr);
    if (fa && fb)
    {
        CHECK(reinterpret_cast<void*>(fa) != reinterpret_cast<void*>(fb));
        CHECK(fa() == 1);
        CHECK(fb() == 2);
    }
    wchar_t pa[MAX_PATH] = {}, pb[MAX_PATH] = {};
    if (a) GetModuleFileNameW(a, pa, MAX_PATH);
    if (b) GetModuleFileNameW(b, pb, MAX_PATH);
    CHECK(std::wstring(pa) == argv[1]);
    CHECK(std::wstring(pb) == argv[2]);
    // The caveat: by NAME the loader answers with the first module loaded.
    CHECK(GetModuleHandleW(L"nvngx_dlssnr.dll") == a);
    if (failures == 0)
        std::printf("PASS: same-named modules from two folders are two modules when loaded by full path\n");
    return failures == 0 ? 0 : 1;
}
