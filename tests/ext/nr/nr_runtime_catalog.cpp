// NR runtime catalog rules (ext/nr/runtime/NrRuntimeCatalog.h). Every rule below is a statement of the picker's spec.
#include "ext/nr/runtime/NrRuntimeCatalog.h"

#include <cstdio>

using namespace ext::nr::runtime;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static Entry Folder(const wchar_t* name, VersionParts fileVersion = {})
{
    Entry e;
    e.id = name;
    e.path = std::filesystem::path(L"X:/game/OptiScaler") / name / L"nvngx_dlssnr.dll";
    e.folderVersion = *ParseFolderVersion(name);
    e.fileVersion = std::move(fileVersion);
    return e;
}
static Entry Legacy(VersionParts fileVersion = {})
{
    Entry e;
    e.id = L"legacy";
    e.legacy = true;
    e.path = L"X:/game/OptiScaler/nvngx_dlssnr.dll";
    e.fileVersion = std::move(fileVersion);
    return e;
}

static void FolderNames()
{
    for (const wchar_t* ok : { L"NR.310.9", L"NR.310.9.16.0", L"NR.1", L"nr.310.9", L"Nr.0", L"NR.000.1", L"NR.123456789" })
        CHECK(IsRuntimeFolderName(ok));
    for (const wchar_t* bad : { L"", L"NR", L"NR.", L"NR..", L"NR.310.", L"NR..310", L"NR.310..9", L"NR.31a", L"NR.-1",
                                L"NR.310.9 ", L" NR.310.9", L"XNR.310", L"NR.1234567890", L"NR.310.9.old", L"NR.310,9",
                                L"NR.310.9\\..", L"NR.310/9", L"nvngx", L"NR310.9", L"legacy" })
        CHECK(!IsRuntimeFolderName(bad));
    CHECK(HasRuntimePrefix(L"NR.old")); // not a valid candidate, but the legacy search still skips it
    CHECK(HasRuntimePrefix(L"nr.x"));
    CHECK(!HasRuntimePrefix(L"NR"));
    CHECK(!HasRuntimePrefix(L"NRX.1"));
    CHECK((*ParseFolderVersion(L"NR.310.9.16.0") == VersionParts { 310, 9, 16, 0 }));
    CHECK(!ParseFolderVersion(L"NR.310.9.old").has_value());
}

static void Versions()
{
    CHECK(CompareVersions({ 310, 9 }, { 310, 10 }) < 0); // numeric, not text, order
    CHECK(CompareVersions({ 310, 10 }, { 310, 9 }) > 0);
    CHECK(CompareVersions({ 310, 9 }, { 310, 9, 0 }) == 0);
    CHECK(CompareVersions({ 310, 9 }, { 310, 9, 0, 1 }) < 0);
    CHECK(CompareVersions({}, { 0 }) == 0);
    CHECK(CompareVersions({ 1 }, { 0, 99999 }) > 0);
    CHECK(VersionText({ 310, 9, 16, 0 }) == L"310.9.16.0");
    CHECK(VersionText({}).empty());
}

static void Automatic()
{
    CHECK(!ChooseAutomatic({}).has_value());
    {
        std::vector<Entry> e { Legacy({ 310, 8, 0, 0 }) };
        CHECK(ChooseAutomatic(e) == 0u); // only the legacy file: today's behaviour
    }
    {
        std::vector<Entry> e { Legacy({ 310, 8, 0, 0 }), Folder(L"NR.310.9", { 310, 9, 16, 0 }), Folder(L"NR.310.10") };
        CHECK(ChooseAutomatic(e) == 2u); // NR.310.10 is newer than NR.310.9 (numeric)
    }
    {
        std::vector<Entry> e { Folder(L"NR.310.9"), Legacy({ 310, 9, 0, 0 }) };
        CHECK(ChooseAutomatic(e) == 1u); // equal versions: the legacy file wins, whatever the order
        std::swap(e[0], e[1]);
        CHECK(ChooseAutomatic(e) == 0u);
    }
    {
        // the file's own version beats the folder's name: a folder named NR.310.9 holding a 310.8 file is a 310.8
        std::vector<Entry> e { Folder(L"NR.310.9", { 310, 8, 0, 0 }), Legacy({ 310, 8, 5, 0 }) };
        CHECK(ChooseAutomatic(e) == 1u);
    }
    {
        // a legacy file without a version resource is the oldest thing there is
        std::vector<Entry> e { Legacy({}), Folder(L"NR.310.8") };
        CHECK(ChooseAutomatic(e) == 1u);
        std::vector<Entry> only { Legacy({}) };
        CHECK(ChooseAutomatic(only) == 0u);
    }
    {
        // 310.9 and 310.9.0 are the same version: the name that sorts first wins, in either order
        std::vector<Entry> a { Folder(L"NR.310.9.0"), Folder(L"NR.310.9") };
        std::vector<Entry> b { Folder(L"NR.310.9"), Folder(L"NR.310.9.0") };
        CHECK(a[*ChooseAutomatic(a)].id == L"NR.310.9");
        CHECK(b[*ChooseAutomatic(b)].id == L"NR.310.9");
    }
}

static void Requests()
{
    CHECK(ParseRequest(L"").kind == RequestKind::Automatic);
    CHECK(ParseRequest(L"auto").kind == RequestKind::Automatic);
    CHECK(ParseRequest(L"  AUTO ").kind == RequestKind::Automatic);
    CHECK(ParseRequest(L"legacy").kind == RequestKind::Legacy);
    CHECK(ParseRequest(L"Legacy").kind == RequestKind::Legacy);
    CHECK(ParseRequest(L" NR.310.9 ").kind == RequestKind::Folder);
    CHECK(ParseRequest(L" NR.310.9 ").folder == L"NR.310.9");
    for (const wchar_t* bad : { L"..\\evil", L"C:\\x\\nvngx_dlssnr.dll", L"NR.310.9\\..\\..", L"NR.", L"310.9", L"latest" })
        CHECK(ParseRequest(bad).kind == RequestKind::Invalid);
}

static void Resolving()
{
    std::vector<Entry> e { Legacy({ 310, 8, 0, 0 }), Folder(L"NR.310.9", { 310, 9, 16, 0 }) };
    {
        const auto r = Resolve(e, L"");
        CHECK(r.status == Status::Ok && r.index == 1u);
    }
    {
        const auto r = Resolve(e, L"legacy");
        CHECK(r.status == Status::Ok && r.index == 0u);
    }
    {
        const auto r = Resolve(e, L"nr.310.9"); // the file system is not case sensitive
        CHECK(r.status == Status::Ok && r.index == 1u);
    }
    {
        // an explicit choice that is not installed never falls back to another model
        const auto r = Resolve(e, L"NR.310.8");
        CHECK(r.status == Status::ExplicitMissing && !r.index.has_value());
    }
    {
        std::vector<Entry> noLegacy { Folder(L"NR.310.9") };
        const auto r = Resolve(noLegacy, L"legacy");
        CHECK(r.status == Status::ExplicitMissing && !r.index.has_value());
    }
    {
        const auto r = Resolve(e, L"..\\NR.310.9");
        CHECK(r.status == Status::InvalidName && !r.index.has_value());
    }
    {
        const auto r = Resolve({}, L"");
        CHECK(r.status == Status::NothingInstalled && !r.index.has_value());
    }
    {
        // a folder entry is never answered for "legacy", and the legacy entry never for a folder name
        std::vector<Entry> onlyFolder { Folder(L"NR.1") };
        CHECK(Resolve(onlyFolder, L"legacy").status == Status::ExplicitMissing);
        std::vector<Entry> onlyLegacy { Legacy() };
        CHECK(Resolve(onlyLegacy, L"NR.1").status == Status::ExplicitMissing);
    }
}

static void Text()
{
    CHECK(SizeText(0) == "0 B");
    CHECK(SizeText(1023) == "1023 B");
    CHECK(SizeText(1024) == "1 KB");
    CHECK(SizeText(276162560) == "263.4 MB");
    CHECK(SizeText(1048576) == "1.0 MB");
    CHECK(ShortSha("") == "hashing...");
    CHECK(ShortSha("0123456789a") == "hashing..."); // fewer than 12 characters: not a hash yet
    CHECK(ShortSha("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == "ba7816bf8f01");
}

int main()
{
    FolderNames();
    Versions();
    Automatic();
    Requests();
    Resolving();
    Text();
    if (failures == 0)
        std::puts("PASS: NR runtime catalog");
    return failures == 0 ? 0 : 1;
}
