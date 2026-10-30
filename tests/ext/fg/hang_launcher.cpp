// Out-of-process hang watchdog for tests that load product DLLs.
//
// A hang inside the loader lock (for example a LoadLibrary hook that waits on a thread that cannot start) cannot be
// observed from inside the hung process: a watchdog thread would block in thread attach, and symbol loading calls
// LoadLibrary. This launcher starts the test as a CHILD process and waits. On timeout it works from outside only:
// it suspends every thread of the child, prints each thread's start address, owning module and a stack walk
// (dbghelp with the child's process handle), reports the owner of the child's loader lock, prints "HANG: <seconds>",
// kills the child and exits with 3.
//
// Usage: hang_launcher <timeout seconds> <symbol dirs separated by ';' or -> <child exe> [child args...]
// The symbol path is exactly what is given here (local folders only; no symbol server is ever used).
// Exit code: the child's exit code, or 3 after a hang, or 2 on a launcher error.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

// ntdll exports, resolved at run time (not in the public SDK import libraries for these classes)
typedef LONG(NTAPI* NtQueryInformationThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG(NTAPI* NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct ProcessBasicInformationData
{
    PVOID ExitStatus;
    PVOID PebBaseAddress;
    PVOID AffinityMask;
    PVOID BasePriority;
    PVOID UniqueProcessId;
    PVOID InheritedFromUniqueProcessId;
};

// x64 PEB: LoaderLock (PRTL_CRITICAL_SECTION) is not in the public winternl.h. Offset 0x110 is checked at run time by
// the launcher's own self-test (a child that holds the loader lock inside a DllMain) before the value is trusted.
static const SIZE_T kPebLoaderLockOffset = 0x110;

static std::string ModuleOf(HANDLE process, DWORD64 address, DWORD64* baseOut)
{
    IMAGEHLP_MODULE64 module = {};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfo64(process, address, &module))
    {
        if (baseOut)
            *baseOut = module.BaseOfImage;
        std::string name = module.ImageName;
        size_t slash = name.find_last_of("\\/");
        return slash == std::string::npos ? name : name.substr(slash + 1);
    }
    if (baseOut)
        *baseOut = 0;
    return "?";
}

static std::string Describe(HANDLE process, DWORD64 address)
{
    char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 500;
    DWORD64 base = 0;
    std::string module = ModuleOf(process, address, &base);
    char text[1024];
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol))
    {
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
            std::snprintf(text, sizeof(text), "%s!%s+0x%llx (%s:%lu)", module.c_str(), symbol->Name,
                          (unsigned long long) displacement, line.FileName, line.LineNumber);
        else
            std::snprintf(text, sizeof(text), "%s!%s+0x%llx", module.c_str(), symbol->Name,
                          (unsigned long long) displacement);
    }
    else
        std::snprintf(text, sizeof(text), "%s+0x%llx", module.c_str(), (unsigned long long) (address - base));
    return text;
}

static void DumpThread(HANDLE process, DWORD tid, NtQueryInformationThreadFn queryThread)
{
    HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    if (!thread)
    {
        std::printf("  thread %lu: OpenThread failed (%lu)\n", tid, GetLastError());
        return;
    }
    PVOID startAddress = nullptr;
    if (queryThread)
        queryThread(thread, 9 /* ThreadQuerySetWin32StartAddress */, &startAddress, sizeof(startAddress), nullptr);
    std::printf("thread %lu start %p = %s\n", tid, startAddress, Describe(process, (DWORD64) startAddress).c_str());

    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context))
    {
        std::printf("  GetThreadContext failed (%lu)\n", GetLastError());
        CloseHandle(thread);
        return;
    }
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int depth = 0; depth < 64; ++depth)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0)
            break;
        std::printf("  #%02d %p %s\n", depth, (void*) frame.AddrPC.Offset,
                    Describe(process, frame.AddrPC.Offset).c_str());
    }
    CloseHandle(thread);
}

static void ReportHang(HANDLE process, DWORD pid, const char* symbolPath)
{
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto queryThread = (NtQueryInformationThreadFn) GetProcAddress(ntdll, "NtQueryInformationThread");
    auto queryProcess = (NtQueryInformationProcessFn) GetProcAddress(ntdll, "NtQueryInformationProcess");

    std::vector<DWORD> tids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == pid)
                tids.push_back(entry.th32ThreadID);
        } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE)
        CloseHandle(snapshot);

    for (DWORD tid : tids)
    {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
        if (thread)
        {
            SuspendThread(thread);
            CloseHandle(thread);
        }
    }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitialize(process, symbolPath, TRUE))
        std::printf("SymInitialize failed (%lu)\n", GetLastError());

    std::printf("---- %zu thread(s) of pid %lu, symbol path: %s ----\n", tids.size(), pid, symbolPath);
    {
        // loaded modules, so a frame can be placed in the module that really contains it
        HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32W module = {};
        module.dwSize = sizeof(module);
        if (modules != INVALID_HANDLE_VALUE && Module32FirstW(modules, &module))
        {
            do
            {
                std::printf("module %p +0x%lx %ls\n", module.modBaseAddr, module.modBaseSize, module.szModule);
            } while (Module32NextW(modules, &module));
        }
        if (modules != INVALID_HANDLE_VALUE)
            CloseHandle(modules);
    }
    for (DWORD tid : tids)
        DumpThread(process, tid, queryThread);

    // Owner of the loader lock: PEB->LoaderLock->OwningThread
    ProcessBasicInformationData info = {};
    if (queryProcess && queryProcess(process, 0, &info, sizeof(info), nullptr) == 0 && info.PebBaseAddress)
    {
        PVOID lockAddress = nullptr;
        RTL_CRITICAL_SECTION lock = {};
        if (ReadProcessMemory(process, (char*) info.PebBaseAddress + kPebLoaderLockOffset, &lockAddress,
                              sizeof(lockAddress), nullptr) &&
            lockAddress && ReadProcessMemory(process, lockAddress, &lock, sizeof(lock), nullptr))
        {
            std::printf("LOADER LOCK: cs=%p LockCount=%ld RecursionCount=%ld OwningThread=%llu\n", lockAddress,
                        lock.LockCount, lock.RecursionCount, (unsigned long long) (ULONG_PTR) lock.OwningThread);
        }
        else
            std::printf("LOADER LOCK: could not read (peb %p)\n", info.PebBaseAddress);
    }
    else
        std::printf("LOADER LOCK: could not query the PEB\n");
    std::fflush(stdout);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: hang_launcher <timeout seconds> <symbol dirs ';' or -> <exe> [args...]\n");
        return 2;
    }
    const DWORD timeoutMs = (DWORD) (std::wcstoul(argv[1], nullptr, 10) * 1000);
    std::string symbolPath;
    if (wcscmp(argv[2], L"-") != 0)
    {
        int n = WideCharToMultiByte(CP_ACP, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        symbolPath.resize(n ? n - 1 : 0);
        WideCharToMultiByte(CP_ACP, 0, argv[2], -1, symbolPath.data(), n, nullptr, nullptr);
    }

    std::wstring commandLine;
    for (int i = 3; i < argc; ++i)
    {
        if (i > 3)
            commandLine += L' ';
        commandLine += L'"' + std::wstring(argv[i]) + L'"';
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child))
    {
        std::fprintf(stderr, "CreateProcess failed (%lu)\n", GetLastError());
        return 2;
    }

    const DWORD wait = WaitForSingleObject(child.hProcess, timeoutMs);
    if (wait == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return (int) code;
    }

    ReportHang(child.hProcess, child.dwProcessId, symbolPath.c_str());
    std::printf("HANG: %lu seconds without exit, child killed\n", timeoutMs / 1000);
    std::fflush(stdout);
    TerminateProcess(child.hProcess, 3);
    WaitForSingleObject(child.hProcess, 5000);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 3;
}
