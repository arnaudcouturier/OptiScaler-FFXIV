// NR runtime scan (ext/nr/runtime/NrRuntimeScan.h) on temporary folders: the legacy search keeps the order of Util::FindFilePath but
// never enters an NR.* folder, the folder scan lists only valid folders holding the model, and the version and hash helpers.
#include "ext/nr/runtime/NrRuntimeScan.h"

#include <cstdio>
#include <fstream>
#include <random>

using namespace ext::nr::runtime;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static const fs::path kFile = L"nvngx_dlssnr.dll";

static void Touch(const fs::path& file, const std::string& content = "x")
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

struct TempTree
{
    fs::path root;
    TempTree()
    {
        std::random_device rd;
        root = fs::temp_directory_path() / (L"nr_runtime_scan_" + std::to_wstring(rd()) + L"_" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

static void LegacySearch()
{
    {   // only NR.* folders: there is no legacy file
        TempTree t;
        Touch(t.root / L"OptiScaler" / L"NR.310.9" / kFile);
        Touch(t.root / L"NR.310.8" / kFile);
        CHECK(!FindLegacy(t.root, kFile).has_value());
    }
    {   // an NR.* folder that sorts first and sits shallower does not hide the legacy file
        TempTree t;
        Touch(t.root / L"aaa" / L"NR.310.8" / kFile);
        Touch(t.root / L"NR.310.9" / kFile);
        Touch(t.root / L"zzz" / L"deep" / kFile);
        const auto found = FindLegacy(t.root, kFile);
        CHECK(found.has_value() && found->parent_path().filename() == L"deep");
    }
    {   // a direct hit comes first
        TempTree t;
        Touch(t.root / kFile);
        Touch(t.root / L"NR.1" / kFile);
        Touch(t.root / L"a" / kFile);
        CHECK(FindLegacy(t.root, kFile) == t.root / kFile);
    }
    {   // breadth first: the shallowest hit wins over a deeper one in an earlier folder
        TempTree t;
        Touch(t.root / L"a" / L"b" / L"c" / kFile);
        Touch(t.root / L"z" / kFile);
        const auto found = FindLegacy(t.root, kFile);
        CHECK(found.has_value() && found->parent_path().filename() == L"z");
    }
    {   // invalid names that still start with NR. are skipped too; look-alikes are searched
        TempTree t;
        Touch(t.root / L"NR.old" / kFile);
        Touch(t.root / L"nr.5" / kFile);
        Touch(t.root / L"NRX" / kFile);
        const auto found = FindLegacy(t.root, kFile);
        CHECK(found.has_value() && found->parent_path().filename() == L"NRX");
    }
    {   // the streamline folder is excluded like Util::FindFilePath does
        TempTree t;
        Touch(t.root / L"streamline" / kFile);
        const ExcludeFn skipStreamline = [&](const fs::path& p) { return IsUnder(p, t.root / L"streamline"); };
        CHECK(!FindLegacy(t.root, kFile, skipStreamline).has_value());
        CHECK(FindLegacy(t.root, kFile).has_value()); // without the predicate it is found
        CHECK(FindLegacy(t.root / L"streamline", kFile, skipStreamline).has_value()); // a direct hit is not filtered (as in FindFilePath)
        Touch(t.root / L"x" / kFile);
        const auto found = FindLegacy(t.root, kFile, skipStreamline);
        CHECK(found.has_value() && found->parent_path().filename() == L"x");
    }
    {   // the Unreal Engine fallback (game root found through Binaries\Win64) still works and still skips NR.*
        TempTree t;
        const fs::path start = t.root / L"Game" / L"Binaries" / L"Win64" / L"OptiScaler";
        fs::create_directories(start);
        Touch(t.root / L"NR.1" / kFile);
        Touch(t.root / L"Other" / kFile); // outside Game: only the game root search reaches it
        const auto found = FindLegacy(start, kFile);
        CHECK(found.has_value() && found->parent_path().filename() == L"Other");
        fs::remove(t.root / L"Other" / kFile);
        CHECK(!FindLegacy(start, kFile).has_value());
    }
    {   // a missing start folder is not an error
        TempTree t;
        CHECK(!FindLegacy(t.root / L"nothing", kFile).has_value());
    }
}

static void FolderScan()
{
    TempTree t;
    const fs::path r1 = t.root / L"one", r2 = t.root / L"two";
    Touch(r1 / L"NR.310.9" / kFile);
    Touch(r1 / L"NR.310.8" / kFile);
    Touch(r1 / L"NR.bad" / kFile);                 // invalid name
    fs::create_directories(r1 / L"NR.310.7");      // valid name, no model inside
    Touch(r1 / L"NR.310.5");                       // a FILE with a valid folder name
    Touch(r1 / L"plain" / kFile);                  // not an NR folder
    Touch(r2 / L"NR.310.9" / kFile);               // duplicate of r1's: the first root wins
    Touch(r2 / L"nr.311.0" / kFile);
    fs::create_directories(t.root / L"elsewhere");
    const auto found = ScanFolders({ t.root / L"elsewhere" / L".." / L"one", r2, t.root / L"missing" }, kFile); // a root spelled with ..
    CHECK(found.size() == 3u);
    if (found.size() == 3u)
    {
        CHECK(found[0].id == L"NR.310.8");
        CHECK(found[1].id == L"NR.310.9");
        CHECK(found[2].id == L"nr.311.0");
        CHECK(!found[0].legacy);
        CHECK((found[0].folderVersion == VersionParts { 310, 8 }));
        CHECK(found[0].fileVersion.empty());
        CHECK(fs::equivalent(found[1].path, r1 / L"NR.310.9" / kFile));
        CHECK(found[1].path == fs::canonical(r1 / L"NR.310.9" / kFile)); // canonical: one file, one spelling
        CHECK(found[1].path.native().rfind(L"\\\\?\\", 0) != 0);     // and not in the extended-length form (it feeds the NGX search path and the log)
    }
}

static void Versions()
{
    char system[MAX_PATH] = {};
    GetSystemDirectoryA(system, MAX_PATH);
    const auto v = ReadFileVersion(fs::path(system) / L"kernel32.dll");
    CHECK(v.size() == 4u);
    CHECK(!v.empty() && v[0] >= 6u && v[2] >= 1000u); // the build number is there
    TempTree t;
    Touch(t.root / L"plain.dll", "not a pe file");
    CHECK(ReadFileVersion(t.root / L"plain.dll").empty());
    CHECK(ReadFileVersion(t.root / L"absent.dll").empty());
    Touch(t.root / L"five.bin", "12345");
    CHECK(FileSize(t.root / L"five.bin") == 5u);
    CHECK(FileSize(t.root / L"absent.bin") == 0u);
}

static void Hashing()
{
    CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    TempTree t;
    Touch(t.root / L"abc.bin", "abc");
    CHECK(Sha256File(t.root / L"abc.bin") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // bigger than the 1 MiB read piece, with a size that is not a multiple of it
    std::string big(3u * 1024 * 1024 + 17, '\0');
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>((i * 31 + (i >> 11)) & 0xff);
    Touch(t.root / L"big.bin", big);
    const std::string whole = Sha256Hex(big.data(), big.size());
    CHECK(whole.size() == 64u);
    CHECK(Sha256File(t.root / L"big.bin") == whole);
    CHECK(Sha256File(t.root / L"absent.bin").empty());
}

static void Paths()
{
    CHECK(IsUnder(L"C:/g/OptiScaler/streamline/x.dll", L"C:/g/OptiScaler/streamline"));
    CHECK(IsUnder(L"C:/g/OptiScaler/Streamline/a/b.dll", L"C:/g/OptiScaler/streamline/"));
    CHECK(IsUnder(L"C:/g/OptiScaler/streamline", L"C:/g/OptiScaler/streamline"));
    CHECK(!IsUnder(L"C:/g/OptiScaler/streamline2/x.dll", L"C:/g/OptiScaler/streamline"));
    CHECK(!IsUnder(L"C:/g/OptiScaler/x.dll", L"C:/g/OptiScaler/streamline"));
    CHECK(!IsUnder(L"C:/g", L"C:/g/OptiScaler"));
}

int main()
{
    LegacySearch();
    FolderScan();
    Versions();
    Hashing();
    Paths();
    if (failures == 0)
        std::puts("PASS: NR runtime scan");
    return failures == 0 ? 0 : 1;
}
