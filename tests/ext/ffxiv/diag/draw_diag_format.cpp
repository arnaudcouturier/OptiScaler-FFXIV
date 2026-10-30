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
        CHECK_EQ(header.size(), size_t(15));
        CHECK_EQ(header[0], std::string("frame"));
        CHECK_EQ(header[6], std::string("pos_x"));
        CHECK_EQ(header[9], std::string("dir_x"));
        CHECK_EQ(header[12], std::string("diffuse_r"));
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
        // a read without a bytecode CRC (a shader created before the hook) still names its buffer
        read.hasCrc = false;
        CHECK_EQ(Fields(CbRowText(read))[2], std::string("-"));
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

    // ---- the caps: a frame is at most a few thousand rows, a capture at most a few frames
    CHECK(kMaxRowsPerFrame >= 2000); // a real frame of this game is about two thousand draws
    CHECK(kMinDrawsPerFrame >= 1 && kMinDrawsPerFrame < kMaxRowsPerFrame);
    CHECK(kMaxFrames == 16);
    CHECK(kDefaultFrames == 3);
    CHECK(kDrawsBytes >= (1ull << 20));
    CHECK(kLightParamBytes == 0x30 && kLightParamDirectionAt == 0x10 && kLightParamPositionAt == 0x00);

    return Finish("draw_diag_format");
}
