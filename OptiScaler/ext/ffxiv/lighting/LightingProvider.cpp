#include "pch.h"

#include "ext/ffxiv/lighting/FfxivLighting.h"
#include "ext/ffxiv/lighting/FfxivLightingCapture.h"
#include "ext/game/GameInterfaces.h"

namespace ext::ffxiv::lighting
{
namespace
{
class SceneLightingProvider final : public game::ISceneLighting
{
  public:
    game::LightingStatus Status() override
    {
        const auto reading = FfxivLightingScan::Latest();
        const auto now = GetTickCount64();

        game::LightingStatus status;
        status.hooksInstalled = FfxivLightingCapture::installed.load();
        status.failed = reading.failed;
        status.valid = reading.valid;
        status.fresh = FfxivLightingScan::Fresh(reading, now);
        status.gain = reading.gain;
        status.samples = reading.samples;
        status.events = reading.events;
        status.ageMs = reading.valid && now >= reading.sampleTime ? now - reading.sampleTime : 0;
        status.reason = reading.reason;
        return status;
    }

    void OnUpscaleBoundary(ID3D11DeviceContext* context, ID3D11Resource* color, bool hdr) override
    {
        FfxivLightingScan::Boundary(context, color, hdr);
    }

    bool ConsumeHistoryReset(uint64_t& cursor) override
    {
        const auto reading = FfxivLightingScan::Latest();
        const auto now = GetTickCount64();
        if (!FfxivLightingScan::Fresh(reading, now))
        {
            cursor = reading.events;
            return false;
        }
        return LightingHistory::Consume(reading.events, reading.eventTime, now, cursor);
    }
};

SceneLightingProvider provider;
} // namespace

game::ISceneLighting& Provider() { return provider; }

void OnDeviceHooked(ID3D11Device* device) { FfxivLightingCapture::Install(device); }

void OnUnhook() { FfxivLightingCapture::Detach(); }
} // namespace ext::ffxiv::lighting
