#pragma once
// The NR runtime picker as the rest of the fork sees it: which model file is selected right now, the catalog the card lists, and the
// request calls of the card. The rules live in NrRuntimeCatalog.h / NrRuntimeSelection.h / NrRuntimeSwitch.h (pure, tested); this
// is the glue to the file system, the config and the clock. Thread safe.

#include "ext/nr/runtime/NrRuntimeSelection.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ext::nr::runtime
{
// UTF-8 text for log lines and the config value, and back.
std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);

// The selection as it stands now: [DlssNr] Runtime folded in through the 250 ms coalescing. Called every frame by the scene pass and
// by the character previews; compare `revision` with the one you built for. While switching is locked (see SwitchLockReason) the
// last snapshot is returned unchanged.
std::shared_ptr<const Snapshot> Current();

// The selection at the first call of the session. The NGX core's first search path and native Vulkan are set once at init, so they
// follow this one, never a later switch.
std::shared_ptr<const Snapshot> Startup();

// The card chose `text` ("" / "auto", "legacy" or a folder name): written to [DlssNr] Runtime, applied after the quiet period.
void RequestRuntime(const std::string& text);

// Look at the disk again (a folder was added or removed). Automatic never moves by itself when a folder appears; this is the moment.
void Rescan();

// A request is waiting for the quiet period.
bool Pending();

// The candidates with their size, version and (once the background hash is done) SHA-256.
std::vector<Entry> Catalog();

// Null while the loaded model can be switched live; otherwise the reason from the real state of the session (hybrid FP4 precision,
// the AMD bridge). The card shows it and the selection stays where it is.
const char* SwitchLockReason();

// The folder the NGX core was told to search first, set once at init from Startup(). Empty when there was no model then.
std::filesystem::path NgxSearchFolder();

// True when the model has the name of the first NGX search path's file (the card says "restart" when a switch leaves that path).
bool FollowsNgxSearchPath(const Snapshot& s);
} // namespace ext::nr::runtime
