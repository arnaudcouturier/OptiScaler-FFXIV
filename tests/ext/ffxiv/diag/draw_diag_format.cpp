// The draw diagnostic's text side: the row and frame formats, the marker ordering rule, the constant-buffer row and the run folder
// naming. Pure: no device, no file, no clock.
#include "check.h"

#include "ext/ffxiv/diag/DrawDiagFormat.h"

#include <string>
#include <vector>

using namespace ext::ffxiv::diag;

namespace
{
// Splits a CSV line the way a reader does, without quoting (no field of this format can contain a comma: every name is a shader
// identifier and every number is a number).
std::vector<std::string> Fields(const std::string& line)
{
    std::vector<std::string> out;
    std::string current;
    for (const char c : line)
    {
        if (c == ',')
        {
            out.push_back(current);
            current.clear();
        }
        else if (c == '\n')
        {
            out.push_back(current);
            current.clear();
        }
        else
            current += c;
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

DrawRow FullRow()
{
    DrawRow row;
    row.frame = 12;
    row.ordinal = 1731;
    row.api = "DrawIndexedInstanced";
    row.gameContext = true;
    row.count = 36;
    row.instances = 4;
    row.start = 12;
    row.baseVertex = -3;
    row.stage = kStageToneMap;
    row.psCrc = 0x85E777EFu;
    row.hasCrc = true;
    row.markers = kMarkerTone | kMarkerUpscale;
    row.srvs = "0:g_SamplerDepth:R32_FLOAT:1920x1080|1:-:R16G16B16A16_FLOAT:?";
    row.cbs = "0:g_CommonParameter:64|2:g_LightParam:400";
    row.rtv = "0:R16G16B16A16_FLOAT:3840x2160|1:R8G8B8A8_UNORM:3840x2160";
    row.dsv = "D24_UNORM_S8_UINT:3840x2160";
    row.viewport = "1:3840x2160+0.5+0.5";
    row.blend = "0/1/1/1/1/1/1/15";
    row.depth = "1/0/4/0";
    return row;
}
} // namespace

int main()
{
    // ---- the header names every column the row writes, in the same order
    {
        const std::vector<std::string> header = Fields(DrawsHeaderText());
        const char* expected[] { "frame",      "ordinal", "api",    "context", "count", "instances", "start",     "base", "stage",
                                 "ps_crc",     "srv",     "cb",     "rtv",     "dsv",   "viewport",  "blend",     "depth", "markers" };
        CHECK_EQ(header.size(), std::size(expected));
        for (size_t i = 0; i < header.size() && i < std::size(expected); ++i)
            CHECK_EQ(header[i], std::string(expected[i]));
        CHECK(DrawsHeaderText().back() == '\n');
    }

    // ---- a full row: one field per header column, and the values a reader has to be able to tell apart
    {
        const DrawRow row = FullRow();
        const std::vector<std::string> fields = Fields(DrawsRowText(row));
        CHECK_EQ(fields.size(), Fields(DrawsHeaderText()).size());
        CHECK_EQ(fields[0], std::string("12"));
        CHECK_EQ(fields[1], std::string("1731"));
        CHECK_EQ(fields[2], std::string("DrawIndexedInstanced"));
        CHECK_EQ(fields[3], std::string("game"));
        CHECK_EQ(fields[4], std::string("36"));
        CHECK_EQ(fields[5], std::string("4"));
        CHECK_EQ(fields[6], std::string("12"));
        CHECK_EQ(fields[7], std::string("-3"));
        CHECK_EQ(fields[8], std::string("1")); // catalog stage 1 = the tone map
        CHECK_EQ(fields[9], std::string("0x85E777EF"));
        CHECK_EQ(fields[10], row.srvs);    // the reflected name of a bound texture is slot by slot
        CHECK_EQ(fields[11], row.cbs);     // ... and of a bound constant buffer
        CHECK_EQ(fields[12], row.rtv);     // every render target, not only the first
        CHECK_EQ(fields[13], row.dsv);
        CHECK_EQ(fields[14], row.viewport);
        CHECK_EQ(fields[15], row.blend);
        CHECK_EQ(fields[16], row.depth);
        CHECK_EQ(fields[17], std::string("tone+upscale"));
        CHECK(DrawsRowText(row).back() == '\n');
        // a CRC is always eight digits, so a reader can compare it as a number and as text
        DrawRow padded = row;
        padded.psCrc = 0x0000ABCDu;
        CHECK_EQ(Fields(DrawsRowText(padded))[9], std::string("0x0000ABCD"));
    }

    // ---- empty parts are dashes, never empty columns (a reader can tell "none" from a parsing mistake)
    {
        DrawRow row;
        row.api = "Draw";
        row.hasCrc = false;
        row.gameContext = false;
        const std::vector<std::string> fields = Fields(DrawsRowText(row));
        CHECK_EQ(fields.size(), Fields(DrawsHeaderText()).size());
        CHECK_EQ(fields[3], std::string("other")); // no latched context: reported, not hidden
        CHECK_EQ(fields[9], std::string("-"));     // no bytecode CRC (a shader created before the hook)
        CHECK_EQ(fields[10], std::string(""));
        CHECK_EQ(fields[11], std::string(""));
        CHECK_EQ(fields[12], std::string(""));
        CHECK_EQ(fields[13], std::string("-"));
        CHECK_EQ(fields[14], std::string("-"));
        CHECK_EQ(fields[15], std::string("-"));
        CHECK_EQ(fields[16], std::string("-"));
        CHECK_EQ(fields[17], std::string(""));
    }

    // ---- marker text: bit order, no separators left behind
    {
        CHECK_EQ(MarkerText(0), std::string(""));
        CHECK_EQ(MarkerText(kMarkerTone), std::string("tone"));
        CHECK_EQ(MarkerText(kMarkerUpscale), std::string("upscale"));
        CHECK_EQ(MarkerText(kMarkerFirstUi), std::string("ui1"));
        CHECK_EQ(MarkerText(kMarkerTone | kMarkerFirstUi), std::string("tone+ui1"));
        CHECK_EQ(MarkerText(kMarkerTone | kMarkerUpscale | kMarkerFirstUi), std::string("tone+upscale+ui1"));
    }

    // ---- the marker ordering rule of one frame
    {
        MarkerPlan plan;
        // draws before any marker
        CHECK_EQ(plan.NoteDraw(kStageNone, 1), uint8_t(0));
        CHECK_EQ(plan.NoteDraw(2, 2), uint8_t(0)); // a post-tonemap stage is not the tone map
        // the tone map is marked once, on its own row, however often it is drawn
        CHECK_EQ(plan.NoteDraw(kStageToneMap, 3), kMarkerTone);
        CHECK_EQ(plan.NoteDraw(kStageToneMap, 4), uint8_t(0));
        // the first UI draw of the frame, once
        CHECK_EQ(plan.NoteDraw(kStageUi, 5), kMarkerFirstUi);
        CHECK_EQ(plan.NoteDraw(kStageUi, 6), uint8_t(0));
        CHECK_EQ(plan.uiOrdinal, 5u);
        CHECK_EQ(plan.toneOrdinal, 3u);
        // the upscale boundary is carried by the row of the draw that follows it, and by that row only
        plan.NoteUpscale(6);
        CHECK_EQ(plan.NoteDraw(kStageNone, 7), kMarkerUpscale);
        CHECK_EQ(plan.NoteDraw(kStageNone, 8), uint8_t(0));
        // a second boundary before the next draw does not move the ordinals or double the marker
        plan.NoteUpscale(8);
        plan.NoteUpscale(9);
        CHECK_EQ(plan.NoteDraw(kStageNone, 9), kMarkerUpscale);
        CHECK_EQ(plan.upscaleAfter, 8u); // the first boundary of the frame is the one the draw order is read against
        // a boundary after the frame's last draw stays in the plan: the frame line reports it
        plan.NoteUpscale(9);
        const std::string line = plan.Line(12, 9);
        CHECK_EQ(line, std::string("# frame 12 draws 9 tone 3 ui1 5 upscale_after 9\n"));
        CHECK(line.front() == '#'); // a comment row, so a reader skips it like the byte-cap marker
        // reset for the next frame
        plan.Reset();
        CHECK_EQ(plan.toneOrdinal, 0u);
        CHECK_EQ(plan.uiOrdinal, 0u);
        CHECK_EQ(plan.upscaleAfter, 0u);
        CHECK(!plan.upscalePending);
        CHECK_EQ(plan.Line(13, 0), std::string("# frame 13 draws 0 tone 0 ui1 0 upscale_after 0\n"));
    }

    // ---- a frame that lost rows to the per-frame cap says so
    {
        MarkerPlan plan;
        CHECK_EQ(plan.NoteDraw(kStageToneMap, 1), kMarkerTone);
        CHECK_EQ(plan.Line(4, kMaxRowsPerFrame + 7, 7), std::string("# frame 4 draws 6007 tone 1 ui1 0 upscale_after 0 omitted 7\n"));
    }

    // ---- cb.csv: one row per read, the three float3s in the study's order (position, direction, colour)
    {
        ConstantRead read;
        read.frame = 3;
        read.seconds = 12.5;
        read.psCrc = 0x1A2B3C4Du;
        read.hasCrc = true;
        read.name = "g_LightParam";
        read.slot = 2;
        read.bytes = 400;
        read.position[0] = 0.0f;
        read.position[1] = 40.0f;
        read.position[2] = -1.5f;
        read.direction[0] = -0.267f;
        read.direction[1] = 0.834f;
        read.direction[2] = 0.4829f;
        read.diffuse[0] = 1.0f;
        read.diffuse[1] = 0.9375f;
        read.diffuse[2] = 0.8125f;
        const std::vector<std::string> header = Fields(CbHeaderText());
        CHECK_EQ(header.size(), size_t(31)); // 15 + the view matrix's 16
        CHECK_EQ(header[0], std::string("frame"));
        CHECK_EQ(header[6], std::string("pos_x"));
        CHECK_EQ(header[9], std::string("dir_x"));
        CHECK_EQ(header[12], std::string("diffuse_r"));
        CHECK_EQ(header[15], std::string("view_00"));
        CHECK_EQ(header[30], std::string("view_33"));
        // a read with no camera buffer (a shader that does not declare it) leaves the sixteen columns as "-"
        const std::vector<std::string> bare = Fields(CbRowText(read));
        CHECK_EQ(bare.size(), header.size());
        CHECK(!read.hasView);
        for (size_t i = 15; i < bare.size(); ++i)
            CHECK_EQ(bare[i], std::string("-"));
        // and one with it fills them row-major, in the order the game stores them
        for (int i = 0; i < 16; ++i)
            read.view[i] = float(i) * 0.5f;
        read.hasView = true;
        const std::vector<std::string> fields = Fields(CbRowText(read));
        CHECK_EQ(fields.size(), header.size());
        CHECK_EQ(fields[0], std::string("3"));
        CHECK_EQ(fields[1], std::string("12.500"));
        CHECK_EQ(fields[2], std::string("0x1A2B3C4D"));
        CHECK_EQ(fields[3], std::string("g_LightParam"));
        CHECK_EQ(fields[4], std::string("2"));
        CHECK_EQ(fields[5], std::string("400"));
        CHECK_EQ(fields[6], std::string("0"));
        CHECK_EQ(fields[7], std::string("40"));
        CHECK_EQ(fields[8], std::string("-1.5"));
        CHECK_EQ(fields[9], std::string("-0.267"));
        CHECK_EQ(fields[10], std::string("0.834"));
        CHECK_EQ(fields[11], std::string("0.4829"));
        CHECK_EQ(fields[12], std::string("1"));
        CHECK_EQ(fields[13], std::string("0.9375"));
        CHECK_EQ(fields[14], std::string("0.8125"));
        CHECK_EQ(fields[15], std::string("0"));
        CHECK_EQ(fields[16], std::string("0.5"));
        CHECK_EQ(fields[30], std::string("7.5"));
        // a read without a bytecode CRC (a shader created before the hook) still names its buffer
        read.hasCrc = false;
        CHECK_EQ(Fields(CbRowText(read))[2], std::string("-"));
        // a read taken while the diagnostic was still waiting for its capture has frame 0
        read.frame = 0;
        CHECK_EQ(Fields(CbRowText(read))[0], std::string("0"));
    }

    // ---- the frame end, in the draw stream: the tone map closes the frame in progress, so no Present is needed
    {
        FrameSplitter split;
        CHECK(!split.toneSeen);
        // every first tone map only starts the frame it ends
        CHECK(!split.NoteDraw(kStageToneMap, 1));
        CHECK(split.toneSeen);
        // the draws of the frame do not close it
        for (uint32_t ordinal = 2; ordinal < 500; ++ordinal)
            CHECK(!split.NoteDraw(kStageNone, ordinal));
        CHECK(!split.NoteDraw(kStageUi, 500));
        // the next tone map does
        CHECK(split.NoteDraw(kStageToneMap, 501));
        CHECK(!split.toneSeen); // and the frame after it is open again
        CHECK(!split.NoteDraw(kStageToneMap, 502)); // a second tone map right behind the split starts, it does not close
        // a frame too short to be a rendered one is never split: the split waits for the next tone map
        FrameSplitter shortFrame;
        CHECK(!shortFrame.NoteDraw(kStageToneMap, 1));
        CHECK(!shortFrame.NoteDraw(kStageToneMap, kMinDrawsPerFrame - 1));
        CHECK(shortFrame.toneSeen);
        CHECK(shortFrame.NoteDraw(kStageToneMap, kMinDrawsPerFrame));
        // a Present closes the frame too: the splitter starts over with it
        split.Reset();
        CHECK(!split.toneSeen);
    }

    // ---- when the capture starts: the two keys beside the frame count
    {
        ArmPlan plan = PlanArm(0, "off");
        CHECK(plan.trigger == Trigger::Immediate);
        CHECK(plan.key == kNoKey);
        plan = PlanArm(0, "");
        CHECK(plan.trigger == Trigger::Immediate);
        plan = PlanArm(90, "off");
        CHECK(plan.trigger == Trigger::Delay);
        CHECK_EQ(plan.delaySeconds, 90.0);
        plan = PlanArm(-5, "off");
        CHECK(plan.trigger == Trigger::Immediate); // a negative delay is no delay
        plan = PlanArm(99999, "off");
        CHECK(plan.trigger == Trigger::Delay);
        CHECK_EQ(plan.delaySeconds, double(kMaxDelaySeconds)); // clamped, so an ini typo cannot wait for a week
        // the key wins over the delay
        plan = PlanArm(90, "F10");
        CHECK(plan.trigger == Trigger::Key);
        CHECK_EQ(plan.key, uint32_t(0x79)); // VK_F10
        CHECK_EQ(plan.delaySeconds, 0.0);
    }

    // ---- the key names a person writes in the ini
    {
        CHECK_EQ(ParseKeyName("F10"), uint32_t(0x79));
        CHECK_EQ(ParseKeyName("f1"), uint32_t(0x70));
        CHECK_EQ(ParseKeyName(" F24	"), uint32_t(0x87));
        CHECK_EQ(ParseKeyName("F25"), kNoKey);  // no such key
        CHECK_EQ(ParseKeyName("F0"), kNoKey);
        CHECK_EQ(ParseKeyName("k"), uint32_t('K')); // letters read as the same key in either case
        CHECK_EQ(ParseKeyName("7"), uint32_t('7'));
        CHECK_EQ(ParseKeyName("off"), kNoKey);
        CHECK_EQ(ParseKeyName("F10x"), kNoKey);
        CHECK_EQ(ParseKeyName("space"), kNoKey);
        CHECK_EQ(ParseKeyName(""), kNoKey);
        CHECK_EQ(ParseKeyName("-"), kNoKey);
    }

    // ---- run folders: the ids the diagnostic writes and the pruning rule that only ever lists its own names
    {
        std::tm t {};
        t.tm_year = 126; // 2026
        t.tm_mon = 9;    // October
        t.tm_mday = 30;
        t.tm_hour = 14;
        t.tm_min = 8;
        t.tm_sec = 5;
        const std::string id = FormatRunId(t);
        CHECK_EQ(id, std::string("20261030-140805"));
        CHECK(IsRunId(id));
        CHECK(!IsRunId("plate-probe"));
        CHECK(!IsRunId("20261030-1408"));
        CHECK(!IsRunId("20261030_140805"));
        std::vector<std::string> names { "20261030-140805", "20261030-140804", "20261030-140803", "20261030-140802", "20261030-140801",
                                         "20261030-140800", "notes", "" };
        const std::vector<std::string> prune = RunsToPrune(names);
        CHECK_EQ(prune.size(), size_t(1)); // six run ids, five kept
        CHECK_EQ(prune[0], std::string("20261030-140800")); // "notes" is not a run id: never pruned
        names.resize(3);
        CHECK(RunsToPrune(names).empty());
        CHECK_EQ(kKeepRuns, size_t(5));
    }

    // ---- the fixture dump's preference: a shader that declares the sun mask, or the light draw's depth+light pair, is kept ahead of the
    // camera buffer every geometry pass declares, so a bound that fills up cannot hide the draw a lighting feature looks for
    {
        CHECK_EQ(FixtureBitsOf("g_SamplerShadow"), kFixtureMask);
        CHECK_EQ(FixtureBitsOf("g_SamplerDepth"), kFixtureDepth);
        CHECK_EQ(FixtureBitsOf("g_LightParam"), kFixtureLight);
        CHECK_EQ(FixtureBitsOf("g_CameraParameter"), kFixtureCamera);
        CHECK_EQ(FixtureBitsOf("g_SamplerViewPosition"), kFixtureViewPosition);
        CHECK_EQ(FixtureBitsOf("g_SamplerOcclusion"), kFixtureOcclusion);
        CHECK_EQ(FixtureBitsOf("g_AmbientParamArray"), kFixtureAmbient);
        // The engine's compiler writes the effect variables' type suffix into the reflection: the sun mask arrives as `g_SamplerShadow.T`
        // and its sampler state as `g_SamplerShadow.S`, so the dump has to look at the variable's name or it would keep nothing at all.
        CHECK_EQ(FixtureBitsOf("g_SamplerShadow.T"), kFixtureMask);
        CHECK_EQ(FixtureBitsOf("g_SamplerShadow.S"), kFixtureMask);
        CHECK_EQ(FixtureBitsOf("g_SamplerDepth.T"), kFixtureDepth);
        CHECK_EQ(FixtureBitsOf("g_SamplerViewPosition.T"), kFixtureViewPosition);
        CHECK_EQ(FixtureBitsOf("g_SamplerOcclusion.T"), kFixtureOcclusion);
        CHECK_EQ(FixtureBitsOf("g_AmbientParamArray"), kFixtureAmbient);
        CHECK(IsKeyFixtureShader(FixtureBitsOf("g_SamplerShadow.T") | FixtureBitsOf("g_SamplerDepth.T") |
                                 FixtureBitsOf("g_LightParam") | FixtureBitsOf("g_CameraParameter")));
        CHECK_EQ(FixtureBitsOf("g_SamplerGBuffer"), 0u); // a name the fork does not identify a draw by
        CHECK_EQ(FixtureBitsOf("g_SamplerShadowMask"), 0u);
        CHECK(IsKeyFixtureShader(kFixtureMask));                       // the sun mask: the light draw and the mask writers
        CHECK(IsKeyFixtureShader(kFixtureDepth | kFixtureLight));      // the light draw's own pair
        CHECK(IsKeyFixtureShader(kFixtureViewPosition | kFixtureCamera));
        CHECK(IsKeyFixtureShader(kFixtureOcclusion));
        CHECK(IsKeyFixtureShader(kFixtureAmbient));
        CHECK(!IsKeyFixtureShader(kFixtureCamera));                    // an ordinary geometry pass
        CHECK(!IsKeyFixtureShader(kFixtureLight));
        CHECK(!IsKeyFixtureShader(kFixtureCamera | kFixtureLight));
        CHECK(!IsKeyFixtureShader(kFixtureDepth));                     // the depth alone is a shadow or a depth pass
        CHECK(kMaxCommonFixtureShaders > 0 && kMaxCommonFixtureShaders < kMaxFixtureShaders);
        // The light draw's own shape keeps slots of its own: the ambient-occlusion passes are key shapes too and a session creates dozens of
        // them before the sun light draw arrives, so a shared bound was full by then and the dump never held the draw it was made for.
        CHECK(IsLightDrawFixture(kFixtureMask));
        CHECK(IsLightDrawFixture(kFixtureDepth | kFixtureLight));
        CHECK(IsLightDrawFixture(kFixtureMask | kFixtureDepth | kFixtureLight | kFixtureCamera));
        CHECK(!IsLightDrawFixture(kFixtureOcclusion)); // another lighting shader, not the light draw
        CHECK(!IsLightDrawFixture(kFixtureViewPosition | kFixtureCamera));
        CHECK(!IsLightDrawFixture(kFixtureDepth)); // the depth without the light parameters is a shadow or a depth pass
        CHECK(!IsLightDrawFixture(kFixtureLight));
        CHECK(!IsLightDrawFixture(0u));
        // the reserve never admits a shader the key rule would refuse
        for (uint32_t bits = 0; bits < 128; ++bits)
            CHECK(!IsLightDrawFixture(bits) || IsKeyFixtureShader(bits));
        CHECK(kMaxLightFixtures > 0);
        CHECK(kMaxLightFixtures < kMaxFixtureShaders); // a reserve, not a second bound of the same size
        // The admission itself: the bound filled with camera-only and ambient-occlusion shaders must still take the light draw's shape, which
        // is the failure one benchmark capture found (64 fixtures, 47 of them occlusion, and the sun light draw was dropped)
        const uint32_t lightShape = kFixtureMask | kFixtureDepth | kFixtureLight | kFixtureCamera;
        CHECK(AdmitFixture(lightShape, kMaxFixtureShaders, kMaxCommonFixtureShaders, 0, 0, 1000));
        CHECK(AdmitFixture(kFixtureDepth | kFixtureLight, kMaxFixtureShaders, kMaxCommonFixtureShaders, 0, 0, 1000));
        CHECK(AdmitFixture(kFixtureMask, kMaxFixtureShaders + kMaxLightFixtures - 1, kMaxCommonFixtureShaders, kMaxLightFixtures - 1, 0, 1000));
        CHECK(!AdmitFixture(kFixtureMask, kMaxFixtureShaders, kMaxCommonFixtureShaders, kMaxLightFixtures, 0, 1000)); // its own bound
        CHECK(!AdmitFixture(kFixtureMask, kMaxFixtureShaders + kMaxLightFixtures, 0, 0, 0, 1000)); // the total bound
        // a geometry pass: behind the key shapes, capped on its own
        CHECK(AdmitFixture(kFixtureCamera, 10, 0, 0, 0, 1000));
        CHECK(!AdmitFixture(kFixtureCamera, 10, kMaxCommonFixtureShaders, 0, 0, 1000));
        CHECK(!AdmitFixture(kFixtureCamera, kMaxFixtureShaders, 0, 0, 0, 1000)); // the shared bound is still a bound
        CHECK(AdmitFixture(kFixtureLight, kMaxFixtureShaders - 1, 0, 0, 0, 1000)); // a buffer only the lighting passes read
        // the byte budget, and a name the dump does not identify
        CHECK(!AdmitFixture(kFixtureMask, 0, 0, 0, kMaxFixtureBytes, 1));
        CHECK(AdmitFixture(kFixtureMask, 0, 0, 0, kMaxFixtureBytes - 1000, 1000));
        CHECK(!AdmitFixture(0u, 0, 0, 0, 0, 1000));
    }

    // ---- the caps: a frame is at most a few thousand rows, a capture at most a few frames
    CHECK(kMaxRowsPerFrame >= 2000); // a real frame of this game is about two thousand draws
    CHECK(kMinDrawsPerFrame >= 1 && kMinDrawsPerFrame < kMaxRowsPerFrame);
    CHECK(kMaxFrames == 16);
    CHECK(kDefaultFrames == 3);
    CHECK(kDrawsBytes >= (1ull << 20));
    CHECK(kLightParamBytes == 0x30 && kLightParamDirectionAt == 0x10 && kLightParamPositionAt == 0x00);
    CHECK(kCameraParamViewAt == 0x00 && kCameraParamMatrixBytes == 0x40); // the view matrix is the camera buffer's first member
    CHECK(kWaitDrawsPerPoll >= 1 && kSampleSeconds > 0.0);
    CHECK(kFormatVersion == 2); // the view matrix columns changed cb.csv

    return Finish("draw_diag_format");
}
