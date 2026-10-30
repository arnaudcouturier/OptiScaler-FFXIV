#include "pch.h"

#include "ext/ExtFeatures.h"
#include "ext/core/ExtConfig.h"
#include <State.h>
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/resolve/Gate.h"
#include "ext/ffxiv/resolve/ModulePath.h"
#include "ext/ffxiv/resolve/StartupDecision.h"
#include "ext/ffxiv/geometry/FfxivGeometry.h"
#include "ext/ffxiv/lighting/FfxivLighting.h"
#include "ext/ffxiv/markers/FfxivMarkers.h"
#include "ext/ffxiv/quality/FfxivQuality.h"
#include "ext/ffxiv/ui/FfxivUiBoundary.h"
#include "ext/game/Game.h"
#include "ext/hdr/HdrScene.h"

namespace ext::game
{
namespace
{
struct FfxivQuirks final : INgxLifecycle, INrQuirks
{
    bool IsolatesShutdown() override { return true; }
    bool BridgedNativeDx11() override { return true; }
};

// Group 1 of the game-build handling (ext/ffxiv/resolve/Gate.h): the providers that identify FFXIV by its executable name and by shader
// fingerprints and read no address of the executable. They run on ANY ffxiv_dx11.exe, whatever its build stamp: a game patch no longer turns
// scene HDR, lighting, the HUD-less boundary, geometry, markers, mip bias and the quirks off. The exact known build is identified by its PE stamp
// and size, whatever the file is called; the name only decides on a build the table does not know. The rollback key [FFXIV] FfxivLegacyGameBuild
// restores the old rule (the exact stamp and size, nothing else), which is what the pre-pattern-search build did.
// The features that read game addresses (Group 2: native quality, jitter, companion hooks, CharaView) are NOT decided here: they ask
// ext::ffxiv::ActiveGate() (ActiveBuild.h), filled once by ext::ffxiv::resolve::StartOnce() at the first device hook.
const Game& SelectGame()
{
    static const Game none {};
    static FfxivQuirks quirks;

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0)
        return none;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return none;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return none;

    // The exe name (read without the MAX_PATH cut: a long install path must not read as "not ffxiv_dx11.exe") and the exact build (the PE stamp and
    // size: with them the file is the game under any name). The keys and the gate input come from the same pure functions the startup uses
    // (StartupDecision.h, unit-tested).
    const std::wstring exePath = ext::ffxiv::resolve::ReadModulePath([](wchar_t* buffer, std::uint32_t size) { return GetModuleFileNameW(nullptr, buffer, size); });
    const bool exactBuild = ext::ffxiv::MatchBuild(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage) != nullptr;
    const ext::ffxiv::resolve::GateInput gateInput = ext::ffxiv::resolve::MakeGateInput(exePath, exactBuild);
    const ext::ffxiv::resolve::GateSettings settings = ext::ffxiv::resolve::ReadSettings(ext::Cfg().ffxiv);
    auto group1 = [&](ext::ffxiv::resolve::Feature f)
    { return ext::ffxiv::resolve::DecideGroup1(f, gateInput.exeIsFfxiv, gateInput.exactKnownBuild, settings).enabled; };
    using ext::ffxiv::resolve::Feature;
    if (!group1(Feature::Lighting))
        return none;

    static Game ffxiv;
    static const bool initialized = [&]
    {
        // one slot per Group 1 feature; each answers the same rule today, asked separately so a future split of the rule stays one edit
        ffxiv.hdr = group1(Feature::SceneHdr) ? &ext::hdr::SceneProvider() : nullptr;
        ffxiv.lighting = group1(Feature::Lighting) ? &ext::ffxiv::lighting::Provider() : nullptr;
        ffxiv.geometry = group1(Feature::Geometry) ? &ext::ffxiv::geometry::Provider() : nullptr;
        ffxiv.fgMarkers = group1(Feature::Markers) ? &ext::ffxiv::markers::Provider() : nullptr;
        ffxiv.hudless = group1(Feature::UiHudless) ? &ext::ffxiv::ui::Provider() : nullptr; // S10 A
        ffxiv.ngx = group1(Feature::QuirkIsolatesShutdown) ? &quirks : nullptr;
        ffxiv.nrQuirks = group1(Feature::QuirkBridgedNativeDx11) ? &quirks : nullptr;
        // Not a Group 1 feature: the native-quality provider is present with the rest of the game, and inert (EnsureHooks false, Live false,
        // DlaaRequired false, exactly what the old null slot answered) unless ext::ffxiv::ActiveGate() enables QualityRead / QualityHooks.
        ffxiv.quality = &ext::ffxiv::quality::Provider();
        ext::SetActive(ext::F::Game, true);
        if (exactBuild)
            LOG_INFO("Matched supported FFXIV build for game-module seams");
        else
            LOG_INFO("FFXIV executable recognised by name on a game build the table does not know (stamp {:#x}, size {:#x}): the game-module seams that read "
                     "no executable address are on; the ones that do follow the [ffxiv-gate] lines",
                     nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        // The upstream FG backbuffer quirk keys on the exe name only, so it stays active on a build the table does not know, while every game-module seam that reads executable addresses is off. That mismatch is silent upstream: say it once here, where the name-recognised unknown build is already logged.
        if (!exactBuild && (State::Instance().gameQuirks & GameQuirk::ReleaseBackbufferOnce))
            LOG_WARN("ReleaseBackbufferOnce quirk is active by exe name on a game build the resolver does not know: the FG backbuffer is still released once while the address-reading seams stay off");
        return true;
    }();
    (void) initialized;
    return ffxiv;
}
} // namespace

const Game& Current()
{
    static const Game& selected = SelectGame();
    return selected;
}

} // namespace ext::game
