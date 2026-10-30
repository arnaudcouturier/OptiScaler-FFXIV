#include "pch.h"

#include "ext/Seams.h"
#include "ext/core/ExtConfig.h"
#include "ext/game/Game.h"
#include "ext/hdr/scene/DlssSceneInput.h"
#include "ext/hdr/scene/FfxivSceneHdr.h"
#include "ext/hdr/scene/NrSceneInput.h"
#include "ext/ffxiv/ui/UiConfig.h"
#include "ext/hdr/HdrScene.h"
#include "ext/nr/NrHooks.h"

// C10: the scene HDR input of NR (ext::nr::SceneHdrHooks, filled once at config load) and the ISceneHdr provider object a
// game module points Game::hdr at. The scene capture itself (FfxivSceneHdr, FfxivHdrPreview) is driven by the game's
// D3D11 hooks: ext::hdr::FfxivSceneHdr::Tag/AfterDraw/Copy/EndFrame are called by the FFXIV module (S7).
namespace ext::hdr
{
namespace
{
using nr::SceneImage;
using nr::ScenePacket;

SceneImage Wrap(SceneInput&& scene)
{
    SceneImage image;
    image.impl = std::make_shared<SceneInput>(std::move(scene));
    return image;
}

SceneInput Unwrap(const SceneImage& image)
{
    return image.impl ? *static_cast<SceneInput*>(image.impl.get()) : SceneInput {};
}

class FfxivProvider final : public game::ISceneHdr
{
  public:
    SceneInput Open(ID3D12Device* device, UINT width, UINT height, bool presentation) override
    {
        return FfxivSceneHdr::Open(device, width, height, presentation);
    }
    bool WouldSupplyScene(UINT width, UINT height) override { return FfxivSceneHdr::WouldSupply(width, height); }
    void PublishReconstructed(SceneInput&& image) override { FfxivSceneHdr::PublishReconstructed(std::move(image)); }
    void EndFrame() override
    {
        FfxivSceneHdr::EndFrame();
        const bool diagnostics = ext::ffxiv::ui::DiagnosticsOn();
        FfxivHdrPreview::SetCensusEnabled(diagnostics);
        if (!diagnostics)
            return;
        // HDR census: one line per 600 frames, only when tagged UI draws reached the mask path (HDR scene mode)
        std::string line;
        if (FfxivHdrPreview::TakeSummary(line))
            LOG_INFO("{}", line);
    }
};

bool GameHasScene() { return game::Current().hdr != nullptr; }

// was SceneNrRequested(): FfxivHDRNRSceneInput && FfxivSceneHdr::Requested() && the FFXIV bridge game
bool NrRequested()
{
    return ext::Cfg().hdr.nrSceneInput.value_or_default() && FfxivSceneHdr::Requested() && GameHasScene();
}

SceneImage Open(ID3D12Device* device, UINT width, UINT height, bool display)
{
    auto* scene = game::Current().hdr;
    if (!scene)
        return {};

    auto image = scene->Open(device, width, height, display);
    return image.hdr ? Wrap(std::move(image)) : SceneImage {};
}

bool Usable(const SceneImage& image)
{
    const auto scene = Unwrap(image);
    return scene.hdr && scene.reference;
}

ScenePacket Prepare(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* native,
                    D3D12_RESOURCE_STATES arrival, UINT width, UINT height, SceneImage&& scene,
                    gpu::GpuLifetime& lifetime, nr::SceneState* failure)
{
    auto* packet = NrSceneInput::Prepare(device, commands, native, arrival, width, height, Unwrap(scene), lifetime, failure);
    ScenePacket result;
    if (packet)
    {
        result.token = packet;
        result.work = packet->work.Get();
    }
    return result;
}

void Finish(ScenePacket& packet, ID3D12GraphicsCommandList* commands, D3D12_RESOURCE_STATES arrival)
{
    if (packet.token)
        NrSceneInput::Finish(*static_cast<NrSceneInput::Packet*>(packet.token), commands, arrival);
}

void Collect(bool keepWarm) { NrSceneInput::Collect(keepWarm); }

void SetState(nr::SceneState state) { nrSceneState.store(state, std::memory_order_relaxed); }

// the cascade at the top of every NR evaluate
void UpdateState(bool nrEnabled, bool preSrPlacement)
{
    using S = nr::SceneState;
    if (!ext::Cfg().hdr.nrSceneInput.value_or_default())
        nrSceneState = S::Off;
    else if (!nrEnabled)
        nrSceneState = S::NRDisabled;
    else if (!NrRequested())
        nrSceneState = S::HDRInactive;
    else if (!preSrPlacement)
        nrSceneState = S::RequiresBeforeSR;
    else if (nrSceneState.load() == S::Off || nrSceneState.load() == S::HDRInactive ||
             nrSceneState.load() == S::NRDisabled || nrSceneState.load() == S::RequiresBeforeSR)
        nrSceneState = S::WaitingNR;
}

bool DlssRequested()
{
    return ext::Cfg().hdr.dlssSceneInput.value_or_default() && FfxivSceneHdr::Requested() && GameHasScene();
}

bool HdrShoulder(const SceneImage& image) { return Unwrap(image).referenceHdrShoulder; }

bool Reconcile(SceneImage& image, ID3D12GraphicsCommandList* commands, ID3D12Resource* output,
               D3D12_RESOURCE_STATES arrival)
{
    return DlssSceneInput::Reconcile(Unwrap(image), commands, output, arrival);
}

void Publish(SceneImage&& image)
{
    if (auto* scene = game::Current().hdr)
        scene->PublishReconstructed(Unwrap(image));
}
} // namespace

game::ISceneHdr& SceneProvider()
{
    static FfxivProvider provider;
    return provider;
}

void InstallSceneHooks()
{
    nr::SceneHdrHooks hooks;
    hooks.nrRequested = &NrRequested;
    hooks.open = &Open;
    hooks.usable = &Usable;
    hooks.prepare = &Prepare;
    hooks.finish = &Finish;
    hooks.collect = &Collect;
    hooks.setState = &SetState;
    hooks.updateState = &UpdateState;
    hooks.dlssRequested = &DlssRequested;
    hooks.hdrShoulder = &HdrShoulder;
    hooks.reconcile = &Reconcile;
    hooks.publish = &Publish;
    nr::SetSceneHdrHooks(hooks);
}
} // namespace ext::hdr
