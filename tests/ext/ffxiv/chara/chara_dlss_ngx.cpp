#include "check.h"
#include "ext/ffxiv/chara/CharaDlssNgx.h"

#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace ext::ffxiv::chara::dlss;

namespace
{
using Value = std::variant<uint32_t, int, float, const void*>;

struct Fake
{
    std::vector<std::pair<std::string, Value>> calls;
    void SetUInt(const char* n, uint32_t v) { calls.emplace_back(n, v); }
    void SetInt(const char* n, int v) { calls.emplace_back(n, v); }
    void SetFloat(const char* n, float v) { calls.emplace_back(n, v); }
    void SetResource(const char* n, const void* r) { calls.emplace_back(n, r); }

    const Value* Find(const char* name) const
    {
        for (const auto& c : calls)
        {
            if (c.first == name)
                return &c.second;
        }
        return nullptr;
    }
    template <class T> bool Is(const char* name, T expected) const
    {
        const Value* v = Find(name);
        return v != nullptr && std::holds_alternative<T>(*v) && std::get<T>(*v) == expected;
    }
};

void TestFlags()
{
    CHECK(ngx::CreateFlags(false) == 0x42u);
    CHECK(ngx::CreateFlags(true) == 0x4Au);
    CHECK(ngx::kFlagMotionLowRes == 0x2u && ngx::kFlagAutoExposure == 0x40u && ngx::kFlagDepthInverted == 0x8u);
    // no HDR (0x1), no jittered vectors (0x4), no sharpening (0x10), no alpha upscaling
    CHECK((ngx::CreateFlags(true) & (0x1u | 0x4u | 0x10u | 0x20u | 0x80u | 0x100u)) == 0);
}

void TestCreate()
{
    Fake f;
    ngx::ApplyCreate(f, 576, 960, false);
    CHECK(f.Is<uint32_t>("Width", 576u) && f.Is<uint32_t>("Height", 960u));
    CHECK(f.Is<uint32_t>("OutWidth", 1152u) && f.Is<uint32_t>("OutHeight", 1920u)); // 2x output
    CHECK(f.Is<int>("PerfQualityValue", 0));                                         // MaxPerf
    CHECK(f.Is<uint32_t>("DLSS.Feature.Create.Flags", 0x42u));
    CHECK(f.calls.size() == 6);

    Fake r;
    ngx::ApplyCreate(r, 288, 480, true);
    CHECK(r.Is<uint32_t>("DLSS.Feature.Create.Flags", 0x4Au));
    CHECK(r.Is<uint32_t>("OutWidth", 576u));
}

void TestEvaluate()
{
    int color = 1, output = 2, depth = 3, mv = 4;
    ngx::EvaluateInputs in;
    in.width = 576;
    in.height = 960;
    in.jitterX = -0.25f;
    in.jitterY = 0.1666f;
    in.reset = true;

    Fake f;
    ngx::ApplyEvaluate<Fake, const void*>(f, in, &color, &output, &depth, &mv);
    CHECK(f.Is<const void*>("Color", &color) && f.Is<const void*>("Output", &output));
    CHECK(f.Is<const void*>("Depth", &depth) && f.Is<const void*>("MotionVectors", &mv));
    CHECK(f.Is<float>("Jitter.Offset.X", -0.25f) && f.Is<float>("Jitter.Offset.Y", 0.1666f)); // no sign flip
    CHECK(f.Is<float>("MV.Scale.X", 576.0f) && f.Is<float>("MV.Scale.Y", 960.0f));
    CHECK(f.Is<float>("Sharpness", 0.0f));
    CHECK(f.Is<float>("DLSS.Pre.Exposure", 1.0f) && f.Is<float>("DLSS.Exposure.Scale", 1.0f));
    CHECK(f.Is<uint32_t>("DLSS.Render.Subrect.Dimensions.Width", 576u) && f.Is<uint32_t>("DLSS.Render.Subrect.Dimensions.Height", 960u));
    CHECK(f.Is<int>("Reset", 1));
    CHECK(f.calls.size() == 14);
    CHECK(f.Find("Exposure") == nullptr && f.Find("TransparencyMask") == nullptr && f.Find("BiasCurrentColorMask") == nullptr);

    in.reset = false;
    Fake g;
    ngx::ApplyEvaluate<Fake, const void*>(g, in, &color, &output, &depth, &mv);
    CHECK(g.Is<int>("Reset", 0));
}

void TestPresetHints()
{
    // names: the six keys of the main DLSS path, in the order the PreSR hints use
    CHECK(std::string(ngx::kPresetHintNames[0]) == "DLSS.Hint.Render.Preset.DLAA");
    CHECK(std::string(ngx::kPresetHintNames[1]) == "DLSS.Hint.Render.Preset.UltraQuality");
    CHECK(std::string(ngx::kPresetHintNames[2]) == "DLSS.Hint.Render.Preset.Quality");
    CHECK(std::string(ngx::kPresetHintNames[3]) == "DLSS.Hint.Render.Preset.Balanced");
    CHECK(std::string(ngx::kPresetHintNames[4]) == "DLSS.Hint.Render.Preset.Performance");
    CHECK(std::string(ngx::kPresetHintNames[5]) == "DLSS.Hint.Render.Preset.UltraPerformance");

    // override off: what the game asked for, whatever the configuration holds
    ngx::PresetSource off;
    off.forAll = 5;
    off.perQuality[4] = 7;
    for (uint32_t i = 0; i < ngx::kPresetHintCount; ++i)
        off.game[i] = 10 + i;
    const ngx::PresetHints fromGame = ngx::ComposePresetHints(off);
    for (uint32_t i = 0; i < ngx::kPresetHintCount; ++i)
        CHECK(fromGame.value[i] == 10 + i);

    // override on, one preset for all (wins over the per-quality keys)
    ngx::PresetSource all = off;
    all.overrideOn = true;
    const ngx::PresetHints forAll = ngx::ComposePresetHints(all);
    for (uint32_t i = 0; i < ngx::kPresetHintCount; ++i)
        CHECK(forAll.value[i] == 5);

    // override on, per quality: only the keys that are set replace the game's value
    ngx::PresetSource per = off;
    per.overrideOn = true;
    per.forAll.reset();
    const ngx::PresetHints perQuality = ngx::ComposePresetHints(per);
    CHECK(perQuality.value[4] == 7 && perQuality.value[0] == 10 && perQuality.value[5] == 15);

    // the legacy "latest" sentinel (0x00FFFFFF) becomes the runtime default (0), in both sources
    ngx::PresetSource latest;
    latest.game[0] = 0x00FFFFFFu;
    latest.overrideOn = true;
    latest.perQuality[1] = 0x00FFFFFFu;
    const ngx::PresetHints mapped = ngx::ComposePresetHints(latest);
    CHECK(mapped.value[0] == 0 && mapped.value[1] == 0);

    // applied as six unsigned values, nothing else
    Fake f;
    ngx::ApplyPresetHints(f, perQuality);
    CHECK(f.calls.size() == 6);
    CHECK(f.Is<uint32_t>("DLSS.Hint.Render.Preset.Performance", 7u));
    CHECK(f.Is<uint32_t>("DLSS.Hint.Render.Preset.DLAA", 10u));
}
} // namespace

int main()
{
    TestPresetHints();
    TestFlags();
    TestCreate();
    TestEvaluate();
    return TEST_RESULT("chara_dlss_ngx");
}
