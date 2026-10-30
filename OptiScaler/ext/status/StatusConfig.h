#pragma once

#include <Config.h>

namespace ext
{
namespace config
{
class Binder;
}

// [Status] (ext/ini/Status.ini): the generic status.json file beside the DLL.
struct StatusConfig
{
    // Enabled: write status.json next to the DLL (atomic replace, throttled, worker thread). Off = nothing is
    // started, no file is written or read. Default false. Named fileEnabled (not enabled) so the ini hygiene scan
    // tells it apart from [DlssNr] Enabled, whose member is also called enabled.
    CustomOptional<bool> fileEnabled { false };

    void Bind(config::Binder& b);
};
} // namespace ext
