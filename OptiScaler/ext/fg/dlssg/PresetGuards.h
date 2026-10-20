#pragma once
// DLSS-G preset guards: find, in a mapped nvngx_dlssg.dll image, the functions the live preset switch depends on, and refuse
// anything that does not match the exact code shape. Pure over (bytes, sections, exception table): tests/ext/fg/
// fg_preset_guards.cpp runs it on a synthetic image; the manual recipe fg_preset_guards_dll runs it on the real DLL.
//
// What is searched (all names are ours; the shapes were read in 310.9.1.0 and cross-checked on 310.8.0):
//   reader    bool(id, uint* out): NVIDIA's cached-settings lookup. Found by its unique call site
//             "lea rdx,[rsp+68h]; mov dword [rsp+68h],0; mov ecx,10E41DF1h; call reader; test al,al".
//   readFn    EndpointConfiguration::ReadRegkeys(config*, context*): the function holding that call site.
//   create    the endpoint create function: the one unique caller of readFn. It copies the process-wide configuration into a
//             private one for each endpoint:  defaults(rbp+40h)  ...  copy(rbp+40h, [shared global])  at two sites.
// The switch re-runs "defaults + readFn(context)" on each new endpoint's private copy, with `reader` answering the active
// request, so every guard below is about proving those four functions and the data flow between them:
//   * every pattern matches exactly once (or exactly twice for the two copy sites) across the executable sections;
//   * every target is the first byte of a function of the exception table and lies in an executable section;
//   * create's second argument is the context: `mov r13,rdx` near its start, `mov rdx,r13` right before the readFn call;
//   * the global read before the readFn call and before both copy calls is the same global;
//   * the stack-frame constants of create are masked (they differ between builds) but must still look like a frame.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ext::fg::preset::guards
{
struct Section
{
    uint32_t va;
    uint32_t size;
    bool executable;
};

// A mapped image: bytes[rva] is the byte at that RVA. The exception table (.pdata) is `pdataSize` bytes at `pdataVa`.
struct ImageView
{
    const uint8_t* bytes;
    size_t size;
    const Section* sections;
    size_t sectionCount;
    uint32_t pdataVa;
    uint32_t pdataSize;
};

enum class Why : uint8_t
{
    Ok,
    OutOfImage,
    NoPdata,
    NoReader,
    ReaderNotUnique,
    ReaderPrologue,
    ReadFnPrologue,
    NoCreate,
    CreateNotUnique,
    CreatePrologue,
    CreateContext,
    CreateSignature,
    ReadCallSetup,
    DefaultsNotFound,
    CopySites,
    CopySetup,
    GlobalMismatch,
    NotFunctionStart
};

constexpr const char* WhyText(Why why)
{
    switch (why)
    {
    case Why::Ok:
        return "ok";
    case Why::OutOfImage:
        return "the image is unreadable";
    case Why::NoPdata:
        return "no exception table";
    case Why::NoReader:
        return "the cached-settings reader call was not found";
    case Why::ReaderNotUnique:
        return "the cached-settings reader call is not unique";
    case Why::ReaderPrologue:
        return "the cached-settings reader does not start as expected";
    case Why::ReadFnPrologue:
        return "the settings read function does not start as expected";
    case Why::NoCreate:
        return "the endpoint create function was not found";
    case Why::CreateNotUnique:
        return "the settings read function has more than one caller";
    case Why::CreatePrologue:
        return "the endpoint create function does not start as expected";
    case Why::CreateContext:
        return "the endpoint create function does not keep its context argument as expected";
    case Why::CreateSignature:
        return "the endpoint create function does not look like a six-argument function returning a 32-bit status";
    case Why::ReadCallSetup:
        return "the settings read call is not set up as expected";
    case Why::DefaultsNotFound:
        return "the configuration defaults function was not found exactly once";
    case Why::CopySites:
        return "the configuration copy function was not found at exactly two call sites";
    case Why::CopySetup:
        return "a configuration copy call is not set up as expected";
    case Why::GlobalMismatch:
        return "the shared configuration is not the same object at every site";
    case Why::NotFunctionStart:
        return "a located address is not the start of a function";
    }
    return "";
}

// RVAs. All zero unless why == Ok.
struct Located
{
    Why why = Why::Ok;
    uint32_t reader = 0;
    uint32_t readFn = 0;
    uint32_t create = 0;
    uint32_t defaults = 0;
    uint32_t copy = 0;
    uint32_t copySites[2] {};  // address after each copy call inside create (the hook compares return addresses)
    uint32_t sharedGlobal = 0; // RVA of the shared configuration pointer
};

namespace detail
{
// pattern bytes; a mask byte of 0 means "any"
template <size_t N> struct Pat
{
    uint8_t b[N];
    uint8_t m[N];
    static constexpr size_t size = N;
};

constexpr uint8_t kAny = 0x100 & 0xFF; // placeholder only; masks are separate arrays

class Scanner
{
  public:
    explicit Scanner(const ImageView& v) : v_(v) {}

    bool InImage(uint64_t rva, uint64_t n) const { return v_.bytes != nullptr && rva + n <= v_.size; }

    bool Exec(uint64_t rva, uint64_t n) const
    {
        for (size_t i = 0; i < v_.sectionCount; ++i)
        {
            const auto& s = v_.sections[i];
            if (s.executable && rva >= s.va && rva + n <= static_cast<uint64_t>(s.va) + s.size && InImage(rva, n))
                return true;
        }
        return false;
    }

    template <size_t N> bool Match(uint64_t at, const uint8_t (&bytes)[N], const uint8_t (&mask)[N]) const
    {
        if (!InImage(at, N))
            return false;
        for (size_t i = 0; i < N; ++i)
            if (mask[i] && v_.bytes[at + i] != bytes[i])
                return false;
        return true;
    }

    template <size_t N> bool Match(uint64_t at, const uint8_t (&bytes)[N]) const
    {
        return InImage(at, N) && std::memcmp(v_.bytes + at, bytes, N) == 0;
    }

    uint32_t U32(uint64_t at) const
    {
        uint32_t x = 0;
        std::memcpy(&x, v_.bytes + at, 4);
        return x;
    }
    int32_t I32(uint64_t at) const
    {
        int32_t x = 0;
        std::memcpy(&x, v_.bytes + at, 4);
        return x;
    }

    // Calls fn(rva) for every offset of every executable section where the first byte equals `first`.
    template <class Fn> void ForEachExec(Fn&& fn) const
    {
        for (size_t i = 0; i < v_.sectionCount; ++i)
        {
            const auto& s = v_.sections[i];
            if (!s.executable || !InImage(s.va, s.size))
                continue;
            for (uint64_t at = s.va; at < static_cast<uint64_t>(s.va) + s.size; ++at)
                fn(static_cast<uint32_t>(at));
        }
    }

    const ImageView& view() const { return v_; }

  private:
    const ImageView& v_;
};

struct Function
{
    uint32_t begin, end;
};

inline bool LoadFunctions(const Scanner& sc, std::vector<Function>& out)
{
    const auto& v = sc.view();
    if (v.pdataSize < 12 || !sc.InImage(v.pdataVa, v.pdataSize))
        return false;
    for (uint32_t off = 0; off + 12 <= v.pdataSize; off += 12)
    {
        const uint32_t begin = sc.U32(v.pdataVa + off), end = sc.U32(v.pdataVa + off + 4);
        if (begin == 0 && end == 0)
            break;
        if (end > begin)
            out.push_back({ begin, end });
    }
    return !out.empty();
}

inline const Function* OwnerOf(const std::vector<Function>& fns, uint32_t rva)
{
    for (const auto& f : fns)
        if (f.begin <= rva && rva < f.end)
            return &f;
    return nullptr;
}

inline const Function* StartOf(const std::vector<Function>& fns, uint32_t rva)
{
    for (const auto& f : fns)
        if (f.begin == rva)
            return &f;
    return nullptr;
}
// True when rva lies strictly inside a function of the table (a leaf function has no entry at all, which is fine).
inline bool InsideFunction(const std::vector<Function>& fns, uint32_t rva)
{
    for (const auto& f : fns)
        if (f.begin < rva && rva < f.end)
            return true;
    return false;
}
} // namespace detail

inline Located Locate(const ImageView& view)
{
    using namespace detail;
    Located fail;
    auto no = [&](Why why)
    {
        fail.why = why;
        return fail;
    };

    if (view.bytes == nullptr || view.size < 0x100 || view.sections == nullptr || view.sectionCount == 0)
        return no(Why::OutOfImage);
    Scanner sc(view);
    std::vector<Function> fns;
    if (!LoadFunctions(sc, fns))
        return no(Why::NoPdata);

    // ---- 1. the cached-settings reader, by its unique call site ----------------------------------------------------------
    // lea rdx,[rsp+68h] / mov dword [rsp+68h],0 / mov ecx,10E41DF1h / call rel32 / test al,al
    constexpr uint8_t kReadCall[] = { 0x48, 0x8D, 0x54, 0x24, 0x68, 0xC7, 0x44, 0x24, 0x68, 0, 0, 0, 0, 0xB9, 0xF1, 0x1D, 0xE4, 0x10, 0xE8 };
    constexpr uint8_t kReaderPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x8B, 0xD9, 0xE8 };
    uint32_t readerCallSite = 0;
    int readerHits = 0;
    uint32_t readerTarget = 0;
    sc.ForEachExec(
        [&](uint32_t at)
        {
            if (!sc.Match(at, kReadCall) || !sc.InImage(at + 25, 1))
                return;
            const uint8_t* p = view.bytes + at;
            if (p[23] != 0x84 || p[24] != 0xC0)
                return;
            ++readerHits;
            readerCallSite = at;
            readerTarget = static_cast<uint32_t>(static_cast<int64_t>(at) + 23 + sc.I32(at + 19));
        });
    if (readerHits == 0)
        return no(Why::NoReader);
    if (readerHits > 1)
        return no(Why::ReaderNotUnique);
    if (!sc.Exec(readerTarget, sizeof(kReaderPrologue)) || !sc.Match(readerTarget, kReaderPrologue))
        return no(Why::ReaderPrologue);
    if (StartOf(fns, readerTarget) == nullptr)
        return no(Why::NotFunctionStart);

    // ---- 2. the read function: owner of the call site ---------------------------------------------------------------------
    const Function* readOwner = OwnerOf(fns, readerCallSite);
    constexpr uint8_t kReadFnPrologue[] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x40 };
    if (readOwner == nullptr || !sc.Match(readOwner->begin, kReadFnPrologue))
        return no(Why::ReadFnPrologue);
    const uint32_t readFn = readOwner->begin;

    // ---- 3. create: the unique caller of the read function ----------------------------------------------------------------
    uint32_t readCallInCreate = 0;
    int callers = 0;
    sc.ForEachExec(
        [&](uint32_t at)
        {
            if (view.bytes[at] != 0xE8 || !sc.InImage(at, 5))
                return;
            if (static_cast<int64_t>(at) + 5 + sc.I32(at + 1) != static_cast<int64_t>(readFn))
                return;
            ++callers;
            readCallInCreate = at;
        });
    if (callers == 0)
        return no(Why::NoCreate);
    if (callers > 1)
        return no(Why::CreateNotUnique);
    const Function* createOwner = OwnerOf(fns, readCallInCreate);
    if (createOwner == nullptr)
        return no(Why::NoCreate);
    const uint32_t create = createOwner->begin;
    const uint32_t createEnd = createOwner->end;
    if (createEnd - create > 0x10000)
        return no(Why::CreatePrologue);

    // push-heavy prologue, then lea rbp,[rsp-N] (N small, so the two high bytes are FF FF), sub rsp,M (M < 64 KiB)
    constexpr uint8_t kCreateHead[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24 };
    constexpr uint8_t kSubRsp[] = { 0x48, 0x81, 0xEC };
    if (!sc.Match(create, kCreateHead) || !sc.InImage(create + 31, 1))
        return no(Why::CreatePrologue);
    {
        const uint8_t* p = view.bytes + create;
        if (p[22] != 0xFF || p[23] != 0xFF) // high bytes of the negative displacement
            return no(Why::CreatePrologue);
        if (!sc.Match(create + 24, kSubRsp) || p[29] != 0 || p[30] != 0)
            return no(Why::CreatePrologue);
    }

    // mov r13,rdx (the context argument) within the first 0x80 bytes
    constexpr uint8_t kMovR13Rdx[] = { 0x4C, 0x8B, 0xEA };
    bool contextKept = false;
    for (uint32_t at = create + 32; at < create + 0x80 && at + 3 <= createEnd; ++at)
        if (sc.Match(at, kMovR13Rdx))
            contextKept = true;
    if (!contextKept)
        return no(Why::CreateContext);

    // The hook forwards six pointer arguments and returns a 32-bit value (an NGX status). Proven here from the code of create:
    //   * no memory operand reaches a stack argument after the sixth: with the frame the prologue just showed, the argument slots
    //     are at entry-rsp + 28h, 30h, 38h ..., which is [rbp + (56 - disp) + k] and [rsp + (sub + 56) + k]; the window of the 7th..
    //     14th argument slots is refused (disp32 encodings; a 7th argument load with a disp8 would need a frame under 128 bytes);
    //   * the status is a 32-bit value: `mov eax, 0BAD0xxxxh` occurs in create (the error returns).
    {
        const int32_t frameDisp = sc.I32(create + 20);                      // lea rbp,[rsp + frameDisp]
        const int64_t frameSub = static_cast<int64_t>(sc.U32(create + 27)); // sub rsp, frameSub
        const int64_t rbpLow = 56 - static_cast<int64_t>(frameDisp) + 0x38, rspLow = frameSub + 56 + 0x38;
        constexpr int64_t kWindow = 0x40; // eight slots
        bool seventh = false, status = false;
        for (uint32_t at = create; at + 6 <= createEnd; ++at)
        {
            const uint8_t b = view.bytes[at];
            if ((b & 0xC7) == 0x85 && at + 5 <= createEnd) // modrm: [rbp + disp32]
            {
                const int64_t disp = sc.I32(at + 1);
                if (disp >= rbpLow && disp < rbpLow + kWindow)
                    seventh = true;
            }
            if ((b & 0xC7) == 0x84 && view.bytes[at + 1] == 0x24 && at + 6 <= createEnd) // modrm + SIB: [rsp + disp32]
            {
                const int64_t disp = sc.I32(at + 2);
                if (disp >= rspLow && disp < rspLow + kWindow)
                    seventh = true;
            }
            if (b == 0xB8 && view.bytes[at + 3] == 0xD0 && view.bytes[at + 4] == 0xBA) // mov eax, 0BAD0xxxxh
                status = true;
        }
        if (seventh || !status)
            return no(Why::CreateSignature);
    }

    // mov rdx,r13 / mov rcx,[rip+global] right before the call to the read function
    constexpr uint8_t kMovRdxR13[] = { 0x49, 0x8B, 0xD5 };
    constexpr uint8_t kMovRcxRip[] = { 0x48, 0x8B, 0x0D };
    if (readCallInCreate < create + 10 || !sc.Match(readCallInCreate - 10, kMovRdxR13) || !sc.Match(readCallInCreate - 7, kMovRcxRip))
        return no(Why::ReadCallSetup);
    const int64_t globalRead =
        static_cast<int64_t>(readCallInCreate) + sc.I32(readCallInCreate - 4); // after the 7-byte mov at call-7

    // ---- 4. defaults and copy, inside create -----------------------------------------------------------------------------
    constexpr uint8_t kLeaRcxRbp40Call[] = { 0x48, 0x8D, 0x4D, 0x40, 0xE8 };
    constexpr uint8_t kDefaultsStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x20, 0x41, 0xBB, 0x03, 0, 0, 0 };
    constexpr uint8_t kCopyStart[] = { 0x8B, 0x02, 0x89, 0x01, 0x0F, 0xB7, 0x42, 0x04, 0x66, 0x89, 0x41, 0x04, 0x8B, 0x42, 0x14, 0x89, 0x41, 0x14 };
    constexpr uint8_t kMovRdxRip[] = { 0x48, 0x8B, 0x15 };
    uint32_t defaults = 0, copy = 0;
    int defaultsHits = 0, copyHits = 0;
    bool copyDiffers = false;
    uint32_t sites[2] {};
    Why late = Why::Ok;
    for (uint32_t at = create; at + 9 <= createEnd; ++at)
    {
        if (!sc.Match(at, kLeaRcxRbp40Call))
            continue;
        const int64_t target64 = static_cast<int64_t>(at) + 9 + sc.I32(at + 5);
        if (target64 < 0 || target64 > static_cast<int64_t>(view.size))
            continue;
        const uint32_t target = static_cast<uint32_t>(target64);
        if (!sc.Exec(target, sizeof(kDefaultsStart)))
            continue;
        if (sc.Match(target, kDefaultsStart))
        {
            ++defaultsHits;
            defaults = target;
        }
        else if (sc.Match(target, kCopyStart))
        {
            if (copyHits > 0 && copy != target)
                copyDiffers = true;
            if (copyHits < 2)
            {
                // mov rdx,[rip+global] right before "lea rcx,[rbp+40h]"
                if (at < create + 7 || !sc.Match(at - 7, kMovRdxRip))
                    late = Why::CopySetup;
                else if (static_cast<int64_t>(at) - 7 + 7 + sc.I32(at - 4) != globalRead)
                    late = Why::GlobalMismatch;
                sites[copyHits] = at + 9;
            }
            ++copyHits;
            copy = target;
        }
    }
    if (defaultsHits != 1)
        return no(Why::DefaultsNotFound);
    if (copyHits != 2 || copyDiffers)
        return no(Why::CopySites);
    if (late != Why::Ok)
        return no(late);
    // defaults is a normal function; copy is a leaf (no exception-table entry) but must not sit inside another function
    if (StartOf(fns, defaults) == nullptr || InsideFunction(fns, copy))
        return no(Why::NotFunctionStart);

    Located ok;
    ok.why = Why::Ok;
    ok.reader = readerTarget;
    ok.readFn = readFn;
    ok.create = create;
    ok.defaults = defaults;
    ok.copy = copy;
    ok.copySites[0] = sites[0];
    ok.copySites[1] = sites[1];
    ok.sharedGlobal = static_cast<uint32_t>(globalRead);
    return ok;
}

// ---- which module to hook ----------------------------------------------------------------------------------------------------
struct Candidate
{
    std::string path;
    bool loaded; // still mapped with the identity we recorded
    bool passes; // every guard above passed on it
};

enum class ChoiceKind : uint8_t
{
    None,
    One,
    Ambiguous
};

struct Choice
{
    ChoiceKind kind = ChoiceKind::None;
    size_t index = 0;
};

inline bool IsDriverCopy(const std::string& path)
{
    std::string lower(path);
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("\\driverstore\\") != std::string::npos || lower.find("/driverstore/") != std::string::npos;
}

// One module that passed. When two pass, the driver's own copy (loaded for its version check) loses to the game's copy; two
// that are equal candidates are ambiguous and nothing is hooked.
inline Choice Choose(const std::vector<Candidate>& candidates)
{
    std::vector<size_t> eligible;
    for (size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].loaded && candidates[i].passes)
            eligible.push_back(i);
    if (eligible.empty())
        return {};
    if (eligible.size() == 1)
        return { ChoiceKind::One, eligible[0] };

    std::vector<size_t> game;
    for (size_t i : eligible)
        if (!IsDriverCopy(candidates[i].path))
            game.push_back(i);
    if (game.size() == 1)
        return { ChoiceKind::One, game[0] };
    return { ChoiceKind::Ambiguous, 0 };
}

// ---- file identity -------------------------------------------------------------------------------------------------------------
// SHA-256 of runtimes whose code shape was checked. The hash is informational (logged); the structural guards decide.
inline const char* KnownHashLabel(std::string_view sha256Hex)
{
    struct Known
    {
        const char* hex;
        const char* label;
    };
    static constexpr Known kKnown[] = {
        { "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82", "NVIDIA DLSS-G 310.9.1.0, code shape read in full with a decompiler" },
        { "5d5cbf14d2727d47f93fd10bf77bd91708ae122482a6f86fd564971641ebd47b", "NVIDIA DLSS-G 310.8.0, structural scan only" },
        { "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7", "a build verified by the Nickle fork (not read here)" },
        { "cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72", "a build verified by the Nickle fork (not read here)" },
    };
    if (sha256Hex.size() != 64)
        return nullptr;
    std::string lower(sha256Hex);
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& k : kKnown)
        if (lower == k.hex)
            return k.label;
    return nullptr;
}

// The families whose code shape was checked: 310.8 and 310.9. Anything else is refused without looking further.
constexpr bool VersionAccepted(unsigned major, unsigned minor) { return major == 310 && (minor == 8 || minor == 9); }
} // namespace ext::fg::preset::guards
