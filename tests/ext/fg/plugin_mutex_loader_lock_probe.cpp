// Probe for the plugin-mutex / loader-lock inversion in the product's LoadLibrary hook
// (docs/notes/streamline-loader-lock-hazard.md).
//
// Two threads use the hook at the same time:
//   thread B loads lock_holder.dll, whose DllMain holds the Windows loader lock, signals the named event and then makes one
//            ordinary LoadLibrary call, which re-enters the hook with the loader lock held;
//   thread A waits for that event and then loads an empty sl.dlss_g.dll, the plugin branch of the hook that takes a plugin
//            hook mutex.
// While the branch takes the mutex before the load, A holds the mutex and waits for the loader lock that B owns, and B
// waits for that mutex in its nested load: the process stops at 0 % CPU, which a player sees as a frozen start. The runner
// puts this exe under hang_launcher, so a hang is exit 3 with every thread stack and the owner of the loader lock printed;
// a run where both loads finish prints PASS and exits 0.
//
// Usage: plugin_mutex_loader_lock_probe <OptiScaler.dll> <sl.dlss_g.dll> <lock_holder.dll>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace
{
constexpr wchar_t kEventVariable[] = L"OPTISCALER_LOADER_LOCK_PROBE_EVENT";
constexpr DWORD kBoundMilliseconds = 30000;
constexpr DWORD kEventWaitMilliseconds = 10000;

HANDLE g_ready = nullptr;
std::atomic<bool> g_finished { false };
std::atomic<int> g_failures { 0 };
std::atomic<DWORD> g_threadA { 0 };
std::atomic<DWORD> g_threadB { 0 };
std::atomic<HMODULE> g_stubModule { nullptr };

// Only reached when the probe is started without the launcher: it must never hang a shell.
void Watchdog()
{
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < kBoundMilliseconds)
    {
        if (g_finished.load())
            return;

        ::Sleep(100);
    }

    std::printf("HANG: no finish within %lu s (thread A %lu, thread B %lu)\n", kBoundMilliseconds / 1000,
                (unsigned long) g_threadA.load(), (unsigned long) g_threadB.load());
    std::fflush(stdout);
    ExitProcess(3);
}

void LoadPlugin(const std::wstring path)
{
    g_threadA = GetCurrentThreadId();
    const DWORD wait = WaitForSingleObject(g_ready, kEventWaitMilliseconds);

    if (wait != WAIT_OBJECT_0)
    {
        std::printf("FAIL: thread A %lu: lock_holder.dll signalled no event within %lu ms (wait %lu)\n",
                    (unsigned long) GetCurrentThreadId(), (unsigned long) kEventWaitMilliseconds, (unsigned long) wait);
        std::fflush(stdout);
        ++g_failures;
        return;
    }

    const ULONGLONG start = GetTickCount64();
    HMODULE module = LoadLibraryW(path.c_str());
    std::printf("thread A %lu: sl.dlss_g.dll loaded after %llu ms: %p\n", (unsigned long) GetCurrentThreadId(),
                (unsigned long long) (GetTickCount64() - start), (void*) module);
    std::fflush(stdout);

    if (module == nullptr)
    {
        std::printf("FAIL: thread A: the LoadLibrary call for sl.dlss_g.dll failed (%lu)\n", GetLastError());
        std::fflush(stdout);
        ++g_failures;
        return;
    }

    g_stubModule = module;
}

void LoadHolder(const std::wstring path)
{
    g_threadB = GetCurrentThreadId();
    HMODULE module = LoadLibraryW(path.c_str());
    std::printf("thread B %lu: lock_holder.dll loaded: %p\n", (unsigned long) GetCurrentThreadId(), (void*) module);
    std::fflush(stdout);

    if (module == nullptr)
    {
        std::printf("FAIL: thread B: the LoadLibrary call for lock_holder.dll failed (%lu)\n", GetLastError());
        std::fflush(stdout);
        ++g_failures;
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (argc < 4)
    {
        std::fprintf(stderr, "usage: plugin_mutex_loader_lock_probe <OptiScaler.dll> <sl.dlss_g.dll> <lock_holder.dll>\n");
        return 2;
    }

    const std::wstring product(argv[1]);
    const std::wstring stub(argv[2]);
    const std::wstring holder(argv[3]);

    // A unique event name per run, handed to lock_holder.dll through the environment.
    wchar_t eventName[128] = {};
    std::swprintf(eventName, 128, L"Local\\OptiScalerLoaderLockProbe-%lu-%llu", GetCurrentProcessId(),
                  (unsigned long long) GetTickCount64());
    SetEnvironmentVariableW(kEventVariable, eventName);
    g_ready = CreateEventW(nullptr, TRUE, FALSE, eventName);

    if (g_ready == nullptr)
    {
        std::printf("FAIL: CreateEvent failed (%lu)\n", GetLastError());
        return 2;
    }

    if (LoadLibraryW(product.c_str()) == nullptr)
    {
        std::printf("FAIL: the product DLL could not be loaded (%lu)\n", GetLastError());
        return 2;
    }

    // One unmatched load before the race: the hook resolves the paths it reads ahead of the plugin branches once (the exe
    // path is cached after the first call), so thread A reaches the branch mutex without anything in the hook needing the
    // loader lock first. advapi32.dll is resident and matches no special branch.
    LoadLibraryW(L"advapi32.dll");

    std::thread watchdog(Watchdog);
    watchdog.detach();

    std::thread a(LoadPlugin, stub);
    std::thread b(LoadHolder, holder);
    a.join();
    b.join();

    g_finished = true;

    if (g_stubModule.load() == nullptr)
        ++g_failures;

    if (g_failures.load() != 0)
    {
        std::printf("FAIL: %d check(s) failed\n", (int) g_failures.load());
        return 1;
    }

    std::printf("PASS: both loads finished (thread A %lu, thread B %lu)\n", (unsigned long) g_threadA.load(),
                (unsigned long) g_threadB.load());
    return 0;
}
