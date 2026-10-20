// DLSS-G preset guards (ext/fg/dlssg/PresetGuards.h): the structural scan that finds NVIDIA's cached-settings reader and the
// endpoint create / configuration functions in a mapped nvngx_dlssg.dll image. Here against a synthetic image built from the
// exact byte patterns (positive case, every guard broken one at a time, the provider choice). The real DLL is the manual
// recipe fg_preset_guards_dll (needs the DLL path in OPTISCALER_DLSSG_DLL).
#include "fg_preset_harness.h"

#include "ext/fg/dlssg/PresetGuards.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ext::fg::preset::guards;

namespace
{
constexpr uint32_t kImageSize = 0x6000;
constexpr uint32_t kTextVa = 0x1000, kTextSize = 0x3000;
constexpr uint32_t kDataVa = 0x4000, kDataSize = 0x1000; // not executable: the shared configuration global
constexpr uint32_t kPdataVa = 0x5000, kPdataSize = 0x1000;

constexpr uint32_t kReader = 0x1100, kReadFn = 0x1200, kCreate = 0x1400, kDefaults = 0x1800, kCopy = 0x1900;
constexpr uint32_t kGlobal = 0x4100;

struct Spec
{
    // each field breaks one guard when changed from its default
    bool readerPrologueOk = true;
    int readCallCount = 1;       // copies of the read call pattern (1 = unique)
    bool afterCallTestOk = true; // the "test al,al" after the reader call
    bool readFnPrologueOk = true;
    int createCallers = 1;       // calls to the read function (1 = unique)
    bool createPrologueOk = true;
    uint32_t frameDisp = 0xFFFFFCA0; // lea rbp,[rsp+disp] (310.9.1.0 has -0x360, Nickle's build -0x390)
    uint32_t frameSub = 0x460;
    bool movR13Ok = true;
    bool readSetupOk = true;     // mov rdx,r13 / mov rcx,[rip+g] before the read call
    bool defaultsOk = true;
    int defaultsHits = 1;
    bool copyOk = true;
    int copySites = 2;
    uint32_t copySiteGlobal = kGlobal; // global read before the second copy
    bool copySetupOk = true;
    bool pdataOk = true;
    bool copyInsideOtherFunction = false; // the real copy function is a leaf: it has no exception-table entry
};

void Put(std::vector<uint8_t>& image, uint32_t at, std::initializer_list<int> bytes)
{
    for (int b : bytes)
        image[at++] = static_cast<uint8_t>(b);
}
void PutRel(std::vector<uint8_t>& image, uint32_t at, uint32_t targetRva) // rel32 at `at`, relative to at+4
{
    const int32_t rel = static_cast<int32_t>(targetRva) - static_cast<int32_t>(at + 4);
    std::memcpy(&image[at], &rel, 4);
}

struct Fake
{
    std::vector<uint8_t> image;
    std::vector<Section> sections;
    uint32_t sites[2] {};
    uint32_t pdataSize = kPdataSize;

    ImageView View() const
    {
        return { image.data(), image.size(), sections.data(), sections.size(), kPdataVa, pdataSize };
    }
};

Fake Build(const Spec& s)
{
    Fake f;
    f.image.assign(kImageSize, 0xCC);
    f.sections = { { kTextVa, kTextSize, true }, { kDataVa, kDataSize, false }, { kPdataVa, kPdataSize, false } };
    auto& im = f.image;

    // reader
    if (s.readerPrologueOk)
        Put(im, kReader, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x8B, 0xD9, 0xE8 });
    else
        Put(im, kReader, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xFA, 0x8B, 0xD9, 0xE8 });
    PutRel(im, kReader + 20, kReader + 0x80);

    // the read function (EndpointConfiguration::ReadRegkeys): prologue, then the reader call pattern once or more
    if (s.readFnPrologueOk)
        Put(im, kReadFn, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40 });
    else
        Put(im, kReadFn, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x48 });
    for (int i = 0; i < s.readCallCount; ++i)
    {
        const uint32_t at = kReadFn + 0x20 + static_cast<uint32_t>(i) * 0x20;
        Put(im, at, { 0x48, 0x8D, 0x54, 0x24, 0x68, 0xC7, 0x44, 0x24, 0x68, 0, 0, 0, 0, 0xB9, 0xF1, 0x1D, 0xE4, 0x10, 0xE8 });
        PutRel(im, at + 19, kReader);
        if (s.afterCallTestOk)
            Put(im, at + 23, { 0x84, 0xC0 });
        else
            Put(im, at + 23, { 0x85, 0xC0 });
    }

    // the endpoint create function
    if (s.createPrologueOk)
        Put(im, kCreate, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24 });
    else
        Put(im, kCreate, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x25 });
    std::memcpy(&im[kCreate + 20], &s.frameDisp, 4);
    Put(im, kCreate + 24, { 0x48, 0x81, 0xEC });
    std::memcpy(&im[kCreate + 27], &s.frameSub, 4);
    if (s.movR13Ok)
        Put(im, kCreate + 0x34, { 0x4C, 0x8B, 0xEA }); // mov r13, rdx
    else
        Put(im, kCreate + 0x34, { 0x4C, 0x8B, 0xE2 });

    uint32_t at = kCreate + 0x60;
    // defaults(rbp+0x40)
    for (int i = 0; i < s.defaultsHits; ++i)
    {
        Put(im, at, { 0x48, 0x8D, 0x4D, 0x40, 0xE8 });
        PutRel(im, at + 5, kDefaults);
        at += 0x10;
    }
    // the read call: mov rdx,r13 / mov rcx,[rip+global] / call ReadRegkeys (the unique caller, plus extra callers if asked)
    for (int i = 0; i < s.createCallers; ++i)
    {
        if (s.readSetupOk)
            Put(im, at, { 0x49, 0x8B, 0xD5 });
        else
            Put(im, at, { 0x49, 0x8B, 0xD4 });
        Put(im, at + 3, { 0x48, 0x8B, 0x0D });
        {
            const int32_t d = static_cast<int32_t>(kGlobal) - static_cast<int32_t>(at + 3 + 7);
            std::memcpy(&im[at + 6], &d, 4);
        }
        Put(im, at + 10, { 0xE8 });
        PutRel(im, at + 11, kReadFn);
        at += 0x20;
    }
    // copy(rbp+0x40, global) at two sites
    for (int i = 0; i < s.copySites; ++i)
    {
        const uint32_t global = (i == 1) ? s.copySiteGlobal : kGlobal;
        if (s.copySetupOk)
            Put(im, at, { 0x48, 0x8B, 0x15 });
        else
            Put(im, at, { 0x48, 0x8B, 0x05 });
        const int32_t d = static_cast<int32_t>(global) - static_cast<int32_t>(at + 7);
        std::memcpy(&im[at + 3], &d, 4);
        Put(im, at + 7, { 0x48, 0x8D, 0x4D, 0x40, 0xE8 });
        PutRel(im, at + 12, kCopy);
        if (i < 2)
            f.sites[i] = at + 16; // the address after the call
        at += 0x20;
    }
    Put(im, at, { 0xC3 }); // end of create
    const uint32_t createEnd = at + 1;

    // defaults and copy functions
    if (s.defaultsOk)
        Put(im, kDefaults, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x20, 0x41, 0xBB, 0x03, 0, 0, 0 });
    if (s.copyOk)
        Put(im, kCopy, { 0x8B, 0x02, 0x89, 0x01, 0x0F, 0xB7, 0x42, 0x04, 0x66, 0x89, 0x41, 0x04, 0x8B, 0x42, 0x14, 0x89, 0x41, 0x14 });

    // .pdata: begin, end, unwind
    struct Fn
    {
        uint32_t begin, end;
    };
    std::vector<Fn> fns = { { kReader, kReader + 0x100 }, { kReadFn, kReadFn + 0x100 }, { kCreate, createEnd }, { kDefaults, kDefaults + 0x40 } };
    if (s.copyInsideOtherFunction)
        fns.push_back({ kCopy - 4, kCopy + 0x40 });
    uint32_t p = kPdataVa;
    for (const auto& fn : fns)
    {
        std::memcpy(&im[p], &fn.begin, 4);
        std::memcpy(&im[p + 4], &fn.end, 4);
        p += 12;
    }
    if (!s.pdataOk)
        f.pdataSize = 0; // no exception table
    return f;
}

Located Scan(const Spec& s) { return Locate(Build(s).View()); }

void FindsEverything()
{
    const Fake f = Build(Spec {});
    const Located l = Locate(f.View());
    CHECK_MSG(l.why == Why::Ok, "why=%s", WhyText(l.why));
    CHECK(l.reader == kReader && l.readFn == kReadFn && l.create == kCreate);
    CHECK(l.defaults == kDefaults && l.copy == kCopy);
    CHECK(l.copySites[0] == f.sites[0] && l.copySites[1] == f.sites[1]);
    CHECK(l.sharedGlobal == kGlobal);
}

void FrameSizeDoesNotMatter()
{
    // 310.9.1.0 and Nickle's build differ only in the stack frame constants of create
    for (uint32_t disp : { 0xFFFFFCA0u, 0xFFFFFC70u, 0xFFFFFD00u })
        for (uint32_t sub : { 0x460u, 0x490u, 0x300u })
        {
            Spec s;
            s.frameDisp = disp;
            s.frameSub = sub;
            CHECK(Scan(s).why == Why::Ok);
        }
    Spec huge; // not a stack frame: the sub is a 32-bit size far above any real frame
    huge.frameSub = 0x90000;
    CHECK(Scan(huge).why == Why::CreatePrologue);
    Spec positive; // lea rbp,[rsp+positive]
    positive.frameDisp = 0x00000360;
    CHECK(Scan(positive).why == Why::CreatePrologue);
}

void EachGuardFailsOnItsOwn()
{
    auto expect = [](const char* name, const Spec& s, Why why)
    {
        const Located l = Scan(s);
        CHECK_MSG(l.why == why, "%s: got %s", name, WhyText(l.why));
        CHECK_MSG(l.create == 0 && l.reader == 0, "%s: located something anyway", name);
    };
    { Spec s; s.readerPrologueOk = false; expect("reader prologue", s, Why::ReaderPrologue); }
    { Spec s; s.readCallCount = 0; expect("no read call", s, Why::NoReader); }
    { Spec s; s.readCallCount = 2; expect("two read calls", s, Why::ReaderNotUnique); }
    { Spec s; s.afterCallTestOk = false; expect("test al,al missing", s, Why::NoReader); }
    { Spec s; s.readFnPrologueOk = false; expect("read function prologue", s, Why::ReadFnPrologue); }
    { Spec s; s.createCallers = 0; expect("no create", s, Why::NoCreate); }
    { Spec s; s.createCallers = 2; expect("two callers of the read function", s, Why::CreateNotUnique); }
    { Spec s; s.createPrologueOk = false; expect("create prologue", s, Why::CreatePrologue); }
    { Spec s; s.movR13Ok = false; expect("mov r13,rdx", s, Why::CreateContext); }
    { Spec s; s.readSetupOk = false; expect("read call setup", s, Why::ReadCallSetup); }
    { Spec s; s.defaultsOk = false; expect("defaults body", s, Why::DefaultsNotFound); }
    { Spec s; s.defaultsHits = 2; expect("two defaults calls", s, Why::DefaultsNotFound); }
    { Spec s; s.copyOk = false; expect("copy body", s, Why::CopySites); }
    { Spec s; s.copySites = 1; expect("one copy site", s, Why::CopySites); }
    { Spec s; s.copySites = 0; expect("no copy site", s, Why::CopySites); }
    { Spec s; s.copySetupOk = false; expect("copy setup", s, Why::CopySetup); }
    { Spec s; s.copySiteGlobal = kGlobal + 8; expect("copy reads another global", s, Why::GlobalMismatch); }
    { Spec s; s.pdataOk = false; expect("no exception table", s, Why::NoPdata); }
    { Spec s; s.copyInsideOtherFunction = true; expect("copy target is inside another function", s, Why::NotFunctionStart); }
}

void ImageBounds()
{
    // an image view that is too small for the patterns never reads outside it
    Fake f = Build(Spec {});
    ImageView v = f.View();
    v.size = 0x1300; // cuts the image inside create
    const Located l = Locate(v);
    CHECK(l.why != Why::Ok);
    ImageView empty { nullptr, 0, nullptr, 0, 0, 0 };
    CHECK(Locate(empty).why != Why::Ok);
}

// ---- the provider choice ---------------------------------------------------------------------------------------------------
void ProviderChoice()
{
    auto cand = [](const char* path, bool loaded, bool passes) { return Candidate { path, loaded, passes }; };
    {
        std::vector<Candidate> c;
        CHECK(Choose(c).kind == ChoiceKind::None);
    }
    {
        std::vector<Candidate> c { cand("C:/Games/x/OptiScaler/nvngx_dlssg.dll", true, true) };
        const auto r = Choose(c);
        CHECK(r.kind == ChoiceKind::One && r.index == 0);
    }
    {   // a failing or unloaded module is never chosen
        std::vector<Candidate> c { cand("a/nvngx_dlssg.dll", true, false), cand("b/nvngx_dlssg.dll", false, true) };
        CHECK(Choose(c).kind == ChoiceKind::None);
    }
    {   // two pass, one is the driver's own copy: the game's copy wins
        std::vector<Candidate> c { cand("C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_x\\nvngx_dlssg.dll", true, true),
                                    cand("C:\\Games\\FFXIV\\OptiScaler\\nvngx_dlssg.dll", true, true) };
        const auto r = Choose(c);
        CHECK(r.kind == ChoiceKind::One && r.index == 1);
    }
    {   // two game copies: ambiguous, nothing is hooked
        std::vector<Candidate> c { cand("C:\\Games\\a\\nvngx_dlssg.dll", true, true), cand("C:\\Games\\b\\nvngx_dlssg.dll", true, true) };
        CHECK(Choose(c).kind == ChoiceKind::Ambiguous);
    }
    {   // only driver copies pass and there are two: ambiguous too
        std::vector<Candidate> c { cand("C:\\Windows\\System32\\DriverStore\\a\\nvngx_dlssg.dll", true, true),
                                    cand("C:\\Windows\\System32\\DriverStore\\b\\nvngx_dlssg.dll", true, true) };
        CHECK(Choose(c).kind == ChoiceKind::Ambiguous);
    }
    {   // a single driver copy is accepted (it is the one in use)
        std::vector<Candidate> c { cand("C:\\Windows\\System32\\DriverStore\\a\\nvngx_dlssg.dll", true, true) };
        CHECK(Choose(c).kind == ChoiceKind::One);
    }
}

void KnownHashes()
{
    CHECK(KnownHashLabel("ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82") != nullptr);
    CHECK(KnownHashLabel("FF6E90EB78B827927DFF5B4ECC6B1C870C2E9BCA29ED9F48C7D348CC9E170B82") != nullptr); // case does not matter
    CHECK(KnownHashLabel("0000000000000000000000000000000000000000000000000000000000000000") == nullptr);
    CHECK(KnownHashLabel("") == nullptr);
    // version gate: 310.8 and 310.9 families only
    CHECK(VersionAccepted(310, 9) && VersionAccepted(310, 8));
    CHECK(!VersionAccepted(310, 7) && !VersionAccepted(310, 10) && !VersionAccepted(311, 9) && !VersionAccepted(0, 0));
}
} // namespace

int main()
{
    FindsEverything();
    FrameSizeDoesNotMatter();
    EachGuardFailsOnItsOwn();
    ImageBounds();
    ProviderChoice();
    KnownHashes();
    if (g_failures)
    {
        std::printf("fg_preset_guards: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("fg_preset_guards: synthetic image located, every guard fails closed on its own, provider choice, hash and version gates passed");
    return 0;
}
