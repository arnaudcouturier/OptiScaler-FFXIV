#pragma once
// The D3D side of "Save preview images" (CharaProbe.cpp decides what to save and when): staging copies of the chosen textures made on
// the draw thread without stalling, an event query, and a read back with D3D11_MAP_FLAG_DO_NOT_WAIT on a later call once the query
// says the GPU is done. The rows are then converted and written by CharaProbeFiles.h into
// <game folder>\OptiScaler\PreviewDebug\<yyyyMMdd-HHmmss>\.
//
// Draw thread only, except the status functions (any thread). One dump is in flight at a time. The session budget (40 files, 300 MiB)
// lives here.

#include "ext/ffxiv/chara/CharaProbeFiles.h"

#include <d3d11.h>

#include <optional>
#include <string>
#include <vector>

namespace ext::ffxiv::chara::probe::dump
{
// One texture to save. `texture` is borrowed (used before Begin returns); null means "there is none" and `missing` says why.
struct Source
{
    std::string name; // file name stem: "native", "native-3", "aa", "dlss"
    ID3D11Texture2D* texture = nullptr;
    std::string missing;
    std::string age; // how old the picture is (ImageAgeText); written to the image line of state.txt
};

// <game folder>\OptiScaler (the folder of the module that holds this code), and its PreviewDebug sub folder. Empty when unknown.
fs::path OptiScalerDirectory();
fs::path OutputRoot();

bool InFlight();

// Starts a dump: creates a staging texture per usable source, copies into it under ReplayScope and ends an event query. Images that are
// missing, multisampled, in an unsupported format or too large are not staged; they get a line in state.txt. Returns false with `why`
// set when a dump is already in flight or the query could not be made. `stateText` is written as state.txt when the dump finishes.
bool Begin(ID3D11DeviceContext* context, const std::vector<Source>& sources, std::string stateText, uint64_t nowMs, std::string& why);

// Call about every 100 ms while InFlight(). Returns the outcome once the dump finished (written, refused by the budget, or failed)
// and nothing while the GPU is still working. A dump the GPU did not finish within 10 seconds is dropped (outcome Failed).
std::optional<DumpOutcome> Pump(ID3D11DeviceContext* context, uint64_t nowMs);

// Writes state.txt alone into a new folder, right away (the `state` command).
DumpOutcome WriteStateOnly(const std::string& stateText);

struct Status
{
    bool inFlight = false;
    uint32_t dumps = 0;       // dumps that wrote at least one file
    uint32_t refusals = 0;    // dumps refused by the budget
    uint32_t files = 0;       // files written this session
    uint64_t bytes = 0;
    std::string lastFolder;   // UTF-8, empty until a dump was written
    std::string lastMessage;  // outcome of the last dump (or request)
};
Status GetStatus();

// A line for the menu: the outcome of the last request (also used for requests that were refused before a dump started).
void SetMessage(const std::string& message);

// Drops a dump that is in flight (the probe was switched off, or the hooks are going away).
void Release();

// Tests: the output root (instead of the game folder) and a fresh session budget.
void SetOutputRootForTest(const fs::path& root);
void ResetSessionForTest();
} // namespace ext::ffxiv::chara::probe::dump
