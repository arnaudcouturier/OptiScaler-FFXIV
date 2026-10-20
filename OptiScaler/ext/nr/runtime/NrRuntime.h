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

// Non-empty when the last scan failed or came back incomplete (a failed rescan keeps the previous list).
std::string ScanNote();

// The config value [DlssNr] Runtime, read under the same mutex RequestRuntime writes it with.
std::string Requested();

// Stops the SHA-256 worker and waits for it (the NR bridge shutdown, before NGX goes away); Resume() allows hashing again.
void Shutdown();
void Resume();

// Look at the disk again (a folder was added or removed). Automatic never moves by itself when a folder appears; this is the moment.
void Rescan();

// A request is waiting for the quiet period.
bool Pending();

// The candidates with their size, version and (once the background hash is done) SHA-256.
std::vector<Entry> Catalog();

// Null while the loaded model can be switched live; otherwise the reason from the real state of the session (hybrid FP4 precision,
// the AMD bridge). The card shows it and the selection stays where it is.
const char* SwitchLockReason();

// AppendNgxSearchPaths reports the folder it really put first on the NGX search list (set once at init; the first report counts).
void NoteNgxSearchFolder(const std::filesystem::path& folder);

// That folder; empty when the NGX core was not given one.
std::filesystem::path NgxSearchFolder();

// False when the selected model sits outside the folder the NGX core was told to search first (the card then says a restart is needed
// to move that path too). True when there is no model or no recorded folder: nothing to disagree with.
bool FollowsNgxSearchPath(const Snapshot& s);
} // namespace ext::nr::runtime
