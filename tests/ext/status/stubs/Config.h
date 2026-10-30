#pragma once
// Test stand-in for the process-wide Config: only the DLL-folder path the status
// collector reads is modelled. The test drives it directly.

#include <filesystem>
#include <optional>
#include <string>

struct Config
{
    static Config* Instance()
    {
        static Config instance;
        return &instance;
    }

    std::optional<std::wstring> MainDllPath;
};
