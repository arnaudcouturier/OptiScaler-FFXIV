#include "pch.h"

#include "PresetModule.h"

#include "PresetDrs.h"
#include "PresetGuards.h"
#include "PresetLogRelay.h"

#include <Util.h>

#include <bcrypt.h>
#include <detours/detours.h>
#include <intrin.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ext::fg::preset::module
{
namespace
{
namespace guards = ext::fg::preset::guards;

// A module that asked the driver for the preset. `refused` is final for this identity (base, stamp, image size): the guards or the
// version gate said no. A module that is merely not mapped any more is not refused, it is just not a candidate right now.
struct Seen
{
    HMODULE module = nullptr;
    DWORD stamp = 0;
    DWORD imageSize = 0;
    bool refused = false;
    std::string reason;
    std::string sha; // computed once, when the module is first evaluated
};

bool SameIdentity(const Seen& a, const Seen& b) { return a.module == b.module && a.stamp == b.stamp && a.imageSize == b.imageSize; }

std::mutex g_seenMutex;
std::vector<Seen> g_seen;

// what Prepare chose, for Install
struct Picked
{
    bool valid = false;
    Seen seen;
    guards::Located located;
    std::string path;
};
std::mutex g_stateMutex; // Prepare, Install
Picked g_picked;
std::atomic<bool> g_ready { false };
bool g_installFailed = false; // a failed Detours transaction is final for this process

std::mutex g_textMutex;
std::string g_text = "not needed yet: nothing is hooked until the first live change";

void SetText(std::string text)
{
    std::lock_guard lock(g_textMutex);
    g_text = std::move(text);
}

bool PeInfo(HMODULE module, DWORD& stamp, DWORD& imageSize)
{
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return false;
        stamp = nt->FileHeader.TimeDateStamp;
        imageSize = nt->OptionalHeader.SizeOfImage;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsRuntime(HMODULE module)
{
    return GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature") != nullptr && GetProcAddress(module, "NVSDK_NGX_D3D12_Init_Ext") != nullptr;
}

std::string Narrow(const std::wstring& w) { return std::filesystem::path(w).string(); }

std::wstring PathOf(HMODULE module)
{
    wchar_t buffer[32768] {};
    const DWORD n = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    return n ? std::wstring(buffer, n) : std::wstring();
}

// One block read and one hash call (about 15 ms for the 7 MB runtime); done once per module identity.
std::string Sha256OfFile(const std::wstring& path)
{
    std::ifstream in { std::filesystem::path(path), std::ios::binary | std::ios::ate };
    if (!in)
        return {};
    const auto size = in.tellg();
    if (size <= 0 || size > 128 * 1024 * 1024)
        return {};
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return {};
    BCRYPT_ALG_HANDLE alg {};
    unsigned char digest[32] {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    const bool ok = BCryptHash(alg, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()), digest, 32) >= 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok)
        return {};
    char hex[65] {};
    for (int i = 0; i < 32; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// The scan reads only executable sections and the exception table of the mapped module; an access violation (a section that
// is not readable) counts as a failed guard.
bool SafeLocate(const guards::ImageView& view, guards::Located& out)
{
    __try
    {
        out = guards::Locate(view);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool MappedView(HMODULE module, std::vector<guards::Section>& sections, guards::ImageView& view)
{
    DWORD stamp = 0, size = 0;
    if (!PeInfo(module, stamp, size))
        return false;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    sections.clear();
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        sections.push_back({ sec[i].VirtualAddress, sec[i].Misc.VirtualSize, (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 });
    const auto& exception = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    view = { base, size, sections.data(), sections.size(), exception.VirtualAddress, exception.Size };
    return true;
}

bool StillMapped(const Seen& s)
{
    HMODULE mapped = nullptr;
    DWORD stamp = 0, size = 0;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(s.module), &mapped) &&
           mapped == s.module && PeInfo(mapped, stamp, size) && stamp == s.stamp && size == s.imageSize;
}

// ---- the hooks ---------------------------------------------------------------------------------------------------------------
using ReaderFn = bool (*)(unsigned, unsigned*);
using CreateFn = unsigned (*)(void*, void*, void*, void*, void*, void*);
using CopyFn = void (*)(void*, const void*);
using DefaultsFn = void (*)(void*);
using ReadFn = bool (*)(void*, void*);

ReaderFn g_reader = nullptr;
CreateFn g_create = nullptr;
CopyFn g_copy = nullptr;
DefaultsFn g_defaults = nullptr;
ReadFn g_readFn = nullptr;
uintptr_t g_sites[2] {};
thread_local void* t_context = nullptr;

bool ReaderHook(unsigned id, unsigned* out)
{
    if (id == kRenderPresetSettingId)
    {
        const auto answer = Requests().ReadCached(id);
        if (answer.handled)
        {
            ReadoutState().NoteCachedRead();
            if (!answer.found)
                return false;
            if (out != nullptr)
                *out = answer.value;
            return true;
        }
    }
    return g_reader(id, out);
}

unsigned CreateHook(void* endpoint, void* context, void* a3, void* a4, void* a5, void* a6)
{
    struct Scope
    {
        void* previous;
        explicit Scope(void* value) : previous(t_context) { t_context = value; }
        ~Scope() { t_context = previous; }
    } scope(context);
    return g_create(endpoint, context, a3, a4, a5, a6);
}

void CopyHook(void* destination, const void* source)
{
    g_copy(destination, source);
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (t_context != nullptr && (caller == g_sites[0] || caller == g_sites[1]))
    {
        // The copy just gave the new endpoint the shared configuration, built at start-up. Rebuild this private copy with
        // NVIDIA's own code; the reader hook above makes it read the active request.
        g_defaults(destination);
        g_readFn(destination, t_context);
        static std::atomic<int> logged { -1 };
        const int request = Requests().Active();
        if (logged.exchange(request) != request)
            LOG_INFO("DLSS-G render preset: a new endpoint configuration was rebuilt for {}", Name(request));
    }
}

// The three Detours hooks, in ONE transaction that enlists only the calling thread. This runs only after FG was switched off
// and its resources were released (PresetSwitch.h): no endpoint is being created and the runtime's own threads are idle, so no
// other thread can be inside the first bytes of the three functions, and no thread has to be suspended (suspending every game
// thread while Detours allocates is the heap-lock deadlock hazard documented in ext/core/DetourThreads.h).
bool InstallHooks(HMODULE module, const guards::Located& found)
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    g_reader = reinterpret_cast<ReaderFn>(base + found.reader);
    g_create = reinterpret_cast<CreateFn>(base + found.create);
    g_copy = reinterpret_cast<CopyFn>(base + found.copy);
    g_defaults = reinterpret_cast<DefaultsFn>(base + found.defaults);
    g_readFn = reinterpret_cast<ReadFn>(base + found.readFn);
    g_sites[0] = base + found.copySites[0];
    g_sites[1] = base + found.copySites[1];

    if (DetourTransactionBegin() != NO_ERROR)
        return false;
    bool ok = DetourUpdateThread(GetCurrentThread()) == NO_ERROR;
    if (ok)
        ok = DetourAttach(reinterpret_cast<PVOID*>(&g_reader), ReaderHook) == NO_ERROR;
    if (ok)
        ok = DetourAttach(reinterpret_cast<PVOID*>(&g_create), CreateHook) == NO_ERROR;
    if (ok)
        ok = DetourAttach(reinterpret_cast<PVOID*>(&g_copy), CopyHook) == NO_ERROR;
    if (!ok)
    {
        DetourTransactionAbort();
        return false;
    }
    return DetourTransactionCommit() == NO_ERROR;
}

// Returns the reason a module is refused, empty when it passed. Read-only.
std::string Evaluate(Seen& seen, guards::Located& found)
{
    const auto path = PathOf(seen.module);
    version_t version {};
    if (path.empty() || !Util::GetFileVersion(path, &version))
        return "its file version could not be read";
    if (seen.sha.empty())
        seen.sha = Sha256OfFile(path);
    const char* label = guards::KnownHashLabel(seen.sha);
    LOG_INFO("DLSS-G render preset: runtime {} version {}.{}.{}, sha256 {} ({}); the live change is decided by the code checks, not by the hash",
             Narrow(path), version.major, version.minor, version.patch, seen.sha.empty() ? "unreadable" : seen.sha,
             label ? label : "not a build seen before");
    if (!guards::VersionAccepted(version.major, version.minor))
        return std::format("version {}.{} is not one the live change was checked on (310.8 and 310.9)", version.major, version.minor);

    std::vector<guards::Section> sections;
    guards::ImageView view {};
    if (!MappedView(seen.module, sections, view) || !SafeLocate(view, found))
        return "its memory image could not be read";
    if (found.why != guards::Why::Ok)
        return std::string("safety check failed: ") + guards::WhyText(found.why) +
               (found.failAt != 0 ? std::format(" (module offset 0x{:X})", found.failAt) : std::string());
    return {};
}

void Remember(const Seen& evaluated)
{
    std::lock_guard lock(g_seenMutex);
    for (auto& s : g_seen)
        if (SameIdentity(s, evaluated))
            s = evaluated;
}
} // namespace

void ObserveDriverQuery(void* caller)
{
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(caller), &owner) ||
        owner == nullptr || !IsRuntime(owner))
        return;
    Seen seen;
    seen.module = owner;
    if (!PeInfo(owner, seen.stamp, seen.imageSize))
        return;
    std::lock_guard lock(g_seenMutex);
    for (const auto& s : g_seen)
        if (SameIdentity(s, seen))
            return;
    if (g_seen.size() < 8)
        g_seen.push_back(seen);
}

bool Ready() { return g_ready.load(); }

bool Prepare()
{
    std::lock_guard lock(g_stateMutex);
    if (g_ready.load())
        return true;
    if (g_installFailed)
        return false;

    std::vector<Seen> seen;
    {
        std::lock_guard seenLock(g_seenMutex);
        seen = g_seen;
    }

    std::vector<guards::Candidate> candidates;
    std::vector<guards::Located> located(seen.size());
    for (size_t i = 0; i < seen.size(); ++i)
    {
        const bool loaded = StillMapped(seen[i]); // not mapped any more: not a candidate now, and not a refusal either
        bool passes = false;
        if (loaded && seen[i].refused)
        {
            // refused before: final for this identity, say so cheaply
        }
        else if (loaded)
        {
            const auto reason = Evaluate(seen[i], located[i]);
            passes = reason.empty();
            if (!passes)
            {
                seen[i].refused = true;
                seen[i].reason = reason;
                LOG_WARN("DLSS-G render preset: {} is not usable for a live change: {}", Narrow(PathOf(seen[i].module)), reason);
            }
            Remember(seen[i]);
        }
        candidates.push_back({ loaded ? Narrow(PathOf(seen[i].module)) : std::string(), loaded, passes });
    }

    const auto choice = guards::Choose(candidates);
    if (choice.kind == guards::ChoiceKind::None)
    {
        std::string why = seen.empty() ? "the runtime was not seen asking the driver for its settings" : "no loaded runtime passed the safety checks";
        for (size_t i = 0; i < seen.size(); ++i)
            if (seen[i].refused && candidates[i].loaded)
                why = seen[i].reason;
        SetText("restart needed: " + why);
        LOG_WARN("DLSS-G render preset: live change refused ({})", why);
        return false;
    }
    if (choice.kind == guards::ChoiceKind::Ambiguous)
    {
        SetText("restart needed: more than one DLSS-G runtime is loaded and they cannot be told apart");
        LOG_WARN("DLSS-G render preset: live change refused (several runtimes passed, none preferred)");
        return false;
    }

    const auto& picked = seen[choice.index];
    // Keep the module for the rest of the process: the hooks and their trampolines will point into it.
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(picked.module), &retained) || retained != picked.module)
    {
        SetText("restart needed: the runtime could not be pinned in memory");
        return false;
    }
    g_picked.valid = true;
    g_picked.seen = picked;
    g_picked.located = located[choice.index];
    g_picked.path = Narrow(PathOf(picked.module));
    SetText("checked, not hooked yet: " + g_picked.path);
    return true;
}

bool Install()
{
    std::lock_guard lock(g_stateMutex);
    if (g_ready.load())
        return true;
    if (g_installFailed || !g_picked.valid)
        return false;
    if (!StillMapped(g_picked.seen)) // cannot happen while pinned; fail closed anyway
    {
        SetText("restart needed: the runtime module changed after it was checked");
        return false;
    }
    if (!InstallHooks(g_picked.seen.module, g_picked.located))
    {
        g_installFailed = true;
        SetText("restart needed: the hooks could not be installed");
        LOG_ERROR("DLSS-G render preset: installing the hooks failed; the old request stays");
        return false;
    }
    SetText("ready: " + g_picked.path + " (if \"read the new request\" stays no after a change, another copy of the runtime is the one in use)");
    g_ready.store(true);
    LOG_INFO("DLSS-G render preset: live change ready on {} (reader {:X}, create {:X}, copy {:X})", g_picked.path, g_picked.located.reader,
             g_picked.located.create, g_picked.located.copy);
    return true;
}

std::string StatusText()
{
    std::lock_guard lock(g_textMutex);
    return g_text;
}
} // namespace ext::fg::preset::module
