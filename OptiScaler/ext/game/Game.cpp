#include "pch.h"

#include "ext/ExtFeatures.h"
#include "ext/ffxiv/GameBuild.h"
#include "ext/ffxiv/lighting/FfxivLighting.h"
#include "ext/ffxiv/quality/FfxivQuality.h"
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
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        ext::ffxiv::MatchBuild(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage) == nullptr)
        return none;

    static Game ffxiv;
    static const bool initialized = []
    {
        ffxiv.hdr = &ext::hdr::SceneProvider();
        ffxiv.lighting = &ext::ffxiv::lighting::Provider();
        ffxiv.quality = &ext::ffxiv::quality::Provider();
        ffxiv.ngx = &quirks;
        ffxiv.nrQuirks = &quirks;
        ext::SetActive(ext::F::Game, true);
        LOG_INFO("Matched supported FFXIV build for game-module seams");
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
