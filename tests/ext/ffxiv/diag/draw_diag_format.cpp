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

// How often one text appears in another (a facts.txt line must appear once, not once per identifying draw).
size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

// A walk of one capture's frames on the runtime's own pure rules: the identify guard, the frame-over predicate, the G-buffer sampling match
// and what the end of a capture does with each copy that is still pending. It counts how often the dump would be written, and whether a
// G-buffer target that no draw ever sampled was written with it.
class DumpWalk
{
  public:
    enum class Kind
    {
        Plain,
        Gbuffer,
        Samples,
        Tone,
    };

    FrameSplitter split;
    bool presentMode = false; // a session whose bridge Present closes each frame: the tone map then only arms the splitter
    bool sampledGbuffer = true; // a later draw binds the five targets, which is the draw their copy waits for
    uint32_t wanted = 3;
    // The draws after the tone map: a live frame carries more than kMinDrawsPerFrame of them, and the arm frame's own tail is what the
    // capture counts as its first frame when the tone map dropped the rows before it.
    uint32_t tailAfterArm = kMinDrawsPerFrame + 8;
    uint32_t tailAfterTone = kMinDrawsPerFrame + 8;

    uint64_t frames = 0, sequence = 0;
    uint32_t framesCaptured = 0, drawsInFrame = 0, engineFrames = 0, dumpToneFrame = 0;
    bool dumpToneAtPresent = false, dumpWritten = false, finished = false;
    uint32_t writes = 0, lost = 0, endCopies = 0, endSkips = 0;
    bool copiedUnsampled = false;

    struct Item
    {
        std::string name;
        DumpRank rank = DumpRank::Rest;
        const void* pointer = nullptr;
        uint64_t sequence = 0;
        bool copied = false, sampled = false;
        std::string note;
    };
    std::vector<Item> items;

    static const void* Pointer(uint32_t id) { return reinterpret_cast<const void*>(static_cast<uintptr_t>(id)); }

    // One engine frame: ordinary draws, the five-target draw, the draw that samples the targets, the tone map, the frame's tail.
    void Frame()
    {
        for (uint32_t i = 0; i < kMinDrawsPerFrame; ++i)
            Draw(Kind::Plain);
        Draw(Kind::Gbuffer);
        if (sampledGbuffer)
            Draw(Kind::Samples);
        Draw(Kind::Tone);
        for (uint32_t i = 0; i < (engineFrames == 0 ? tailAfterArm : tailAfterTone); ++i)
            Draw(Kind::Plain);
        ++engineFrames;
        if (presentMode)
            Present();
    }

    void Run()
    {
        for (int guard = 0; guard < 32 && !finished; ++guard)
            Frame();
    }

    void Draw(Kind kind)
    {
        ++sequence;
        ++drawsInFrame;
        CopyPending();
        if (kind == Kind::Samples)
            CopySampled();
        MaybeWrite(true);
        if (DumpIdentifies(split.fresh))
        {
            if (kind == Kind::Gbuffer)
                for (uint32_t i = 0; i < 5; ++i)
                    Identify("gbuffer" + std::to_string(i), DumpRank::Gbuffer, Pointer(0x2000 + i));
            if (kind == Kind::Tone && dumpToneFrame == 0)
            {
                dumpToneFrame = uint32_t(frames + 1);
                Identify("scene", DumpRank::Scene, Pointer(0x5000));
                Identify("tonemap", DumpRank::Rest, Pointer(0x5001));
            }
        }
        const uint8_t bits = split.NoteDraw(kind == Kind::Tone ? kStageToneMap : kStageNone, drawsInFrame);
        if ((bits & kSplitArm) != 0)
            drawsInFrame = 0; // the partial frame before the first tone map is dropped
        if ((bits & kSplitClose) != 0)
        {
            CloseFrame(false);
            if (framesCaptured >= wanted)
                Finish();
        }
    }

    // DumpCopyPending: every rank but the G-buffer's is copied at a later draw.
    void CopyPending()
    {
        for (Item& item : items)
            if (DumpCopyDue(item.copied, !item.note.empty(), item.sequence, sequence) && !DumpWaitsForSampling(item.rank))
                item.copied = true;
    }

    // DumpCopySampled: the G-buffer targets are copied at a draw whose texture slots bind them (the row this model's draw binds).
    void CopySampled()
    {
        const void* bound[] { Pointer(0x2000), Pointer(0x2001), Pointer(0x2002), Pointer(0x2003), Pointer(0x2004) };
        for (Item& item : items)
        {
            if (!DumpWaitsForSampling(item.rank) || !DumpCopyDue(item.copied, !item.note.empty(), item.sequence, sequence))
                continue;
            if (DumpSampledSlot(bound, 5, item.pointer) < 0)
                continue;
            item.copied = true;
            item.sampled = true;
        }
    }

    void Identify(const std::string& name, DumpRank rank, const void* pointer)
    {
        bool held = false;
        size_t written = 0;
        for (const Item& item : items)
        {
            held = held || item.name == name;
            written += item.note.empty() ? 1 : 0;
        }
        if (!DumpAdds(true, dumpWritten, pointer, held, written))
            return;
        Item item;
        item.name = name;
        item.rank = rank;
        item.pointer = pointer;
        item.sequence = sequence;
        items.push_back(item);
    }

    // MaybeWriteDump: a bridge Present has no context, and the write refuses without one.
    void MaybeWrite(bool context)
    {
        if (dumpWritten || !DumpFrameOver(frames, dumpToneFrame, dumpToneAtPresent) || !context)
            return;
        Write();
    }

    void CloseFrame(bool present)
    {
        if (drawsInFrame >= kMinDrawsPerFrame)
            ++framesCaptured;
        ++frames;
        drawsInFrame = 0;
        if (present)
            split.Reset(); // only the bridge Present starts the splitter over; the tone map that closes a window keeps it armed
        if (present && dumpToneFrame != 0 && frames == dumpToneFrame)
            dumpToneAtPresent = true;
        MaybeWrite(!present);
    }

    void Present()
    {
        CloseFrame(true);
        if (framesCaptured >= wanted)
            Finish();
    }

    // FinishDump: the dump is written here with the game's context, or it is lost for the session.
    void Finish()
    {
        finished = true;
        if (dumpWritten)
            return;
        if (!DumpFrameOver(frames, dumpToneFrame, dumpToneAtPresent))
        {
            ++lost;
            return;
        }
        for (Item& item : items)
        {
            if (item.copied || !item.note.empty())
                continue;
            switch (DumpAtEnd(item.rank, item.sequence, sequence, dumpToneAtPresent))
            {
            case DumpEnd::Skip:
                item.note = "not sampled";
                ++endSkips;
                continue;
            case DumpEnd::Leave: continue;
            case DumpEnd::Copy:
                item.copied = true;
                ++endCopies;
                break;
            }
        }
        Write();
    }

    void Write()
    {
        ++writes;
        dumpWritten = true;
        for (const Item& item : items)
            if (DumpWaitsForSampling(item.rank) && item.copied && !item.sampled)
                copiedUnsampled = true;
        items.clear();
    }
};

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
    row.srvs = "0:g_SamplerDepth:R32_FLOAT:1920x1080:1a2b3c4d5e6f|1:-:R16G16B16A16_FLOAT:?:0";
    row.cbs = "0:g_CommonParameter:64:0|2:g_LightParam:400:2a3b4c5d6e7f";
    row.rtv = "0:R16G16B16A16_FLOAT:3840x2160:3a4b5c6d7e8f|1:R8G8B8A8_UNORM:3840x2160:4a5b6c7d8e9f";
    row.dsv = "D24_UNORM_S8_UINT:3840x2160:5a6b7c8d9eaf";
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
        CHECK_EQ(plan.Line(4, kMaxRowsPerFrame + 7, 7), std::string("# frame 4 draws 12007 tone 1 ui1 0 upscale_after 0 omitted 7\n"));
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
        CHECK_EQ(header.size(), size_t(47)); // 15 + the view matrix's 16 + the view-projection's 16
        CHECK_EQ(header[0], std::string("frame"));
        CHECK_EQ(header[6], std::string("pos_x"));
        CHECK_EQ(header[9], std::string("dir_x"));
        CHECK_EQ(header[12], std::string("diffuse_r"));
        CHECK_EQ(header[15], std::string("view_00"));
        CHECK_EQ(header[30], std::string("view_33"));
        CHECK_EQ(header[31], std::string("vp_00"));
        CHECK_EQ(header[46], std::string("vp_33"));
        // a read with no camera buffer (a shader that does not declare it) leaves the two matrices' columns as "-"
        const std::vector<std::string> bare = Fields(CbRowText(read));
        CHECK_EQ(bare.size(), header.size());
        CHECK(!read.hasView);
        CHECK(!read.hasViewProjection);
        for (size_t i = 15; i < bare.size(); ++i)
            CHECK_EQ(bare[i], std::string("-"));
        // and one with it fills them row-major, in the order the game stores them, the view first and the view-projection after it
        for (int i = 0; i < 16; ++i)
        {
            read.view[i] = float(i) * 0.5f;
            read.viewProjection[i] = 100.0f + float(i) * 0.25f;
        }
        read.hasView = true;
        read.hasViewProjection = true;
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
        CHECK_EQ(fields[31], std::string("100"));
        CHECK_EQ(fields[46], std::string("103.75"));
        // a read without a bytecode CRC (a shader created before the hook) still names its buffer
        read.hasCrc = false;
        CHECK_EQ(Fields(CbRowText(read))[2], std::string("-"));
        // a read taken while the diagnostic was still waiting for its capture has frame 0
        read.frame = 0;
        CHECK_EQ(Fields(CbRowText(read))[0], std::string("0"));
    }

    // ---- the frame end, in the draw stream: ONE tone map per window, and the closing tone map opens the next one
    {
        FrameSplitter split;
        CHECK(!split.toneSeen);
        // The first tone map opens the first window and marks the partial frame the capture started in.
        CHECK_EQ(split.NoteDraw(kStageToneMap, 1), uint8_t(kSplitArm));
        CHECK(split.toneSeen);
        // The draws of the window do not close it.
        for (uint32_t ordinal = 1; ordinal < 500; ++ordinal)
            CHECK_EQ(split.NoteDraw(kStageNone, ordinal), uint8_t(0));
        CHECK_EQ(split.NoteDraw(kStageUi, 500), uint8_t(0));
        // The next tone map closes the window and arms the next one in the same call: a tone, a window, a tone, a window, a tone gives
        // TWO closes, which is what a live frame needs (a window that spans two frames was the measured bug: 8,824 and 15,079 draws).
        CHECK_EQ(split.NoteDraw(kStageToneMap, 501), uint8_t(kSplitClose));
        CHECK(split.toneSeen); // the closing tone map is the next window's boundary too
        CHECK_EQ(split.NoteDraw(kStageNone, 1), uint8_t(0));
        CHECK_EQ(split.NoteDraw(kStageToneMap, 7499), uint8_t(kSplitClose));
        CHECK_EQ(split.NoteDraw(kStageToneMap, 7500), uint8_t(kSplitClose));
        // A window too short to be a rendered one is never split: the split waits for the next tone map.
        FrameSplitter shortFrame;
        CHECK_EQ(shortFrame.NoteDraw(kStageToneMap, 1), uint8_t(kSplitArm));
        CHECK_EQ(shortFrame.NoteDraw(kStageToneMap, kMinDrawsPerFrame - 1), uint8_t(0));
        CHECK(shortFrame.toneSeen);
        CHECK_EQ(shortFrame.NoteDraw(kStageToneMap, kMinDrawsPerFrame), uint8_t(kSplitClose));
        // A bridge Present closes a frame at the engine's own frame end: the next window starts there, so its tone map only arms it and
        // the rows after the Present are not dropped as a partial frame.
        FrameSplitter bridged;
        CHECK_EQ(bridged.NoteDraw(kStageToneMap, 1), uint8_t(kSplitArm));
        CHECK_EQ(bridged.NoteDraw(kStageToneMap, 5000), uint8_t(kSplitClose));
        bridged.Reset();
        CHECK(!bridged.toneSeen);
        CHECK_EQ(bridged.NoteDraw(kStageToneMap, 5000), uint8_t(0));
        CHECK_EQ(bridged.NoteDraw(kStageToneMap, 5000), uint8_t(kSplitClose));
        // A Present that closed a frame before the first tone map also means there is no partial frame left to drop.
        FrameSplitter early;
        early.Reset();
        CHECK_EQ(early.NoteDraw(kStageToneMap, 40), uint8_t(0));
        CHECK_EQ(early.NoteDraw(kStageToneMap, 40), uint8_t(kSplitClose));
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
        CHECK_EQ(ParseKeyName(" F24	"), uint32_t(0x87)); // a stray space or tab around the name is not part of it
        CHECK_EQ(ParseKeyName(" F9"), uint32_t(0x78));    // the same for a leading space
        CHECK_EQ(ParseKeyName("F25"), kNoKey);  // no such key
        CHECK_EQ(ParseKeyName("F0"), kNoKey);
        CHECK_EQ(ParseKeyName("Fx"), kNoKey);   // no number: not a function key
        CHECK_EQ(ParseKeyName("F1x"), kNoKey);  // a character after the number is not a key name
        CHECK_EQ(ParseKeyName("F10x"), kNoKey);
        CHECK_EQ(ParseKeyName("ax"), kNoKey);   // two characters that are neither a function key nor one key
        // A name longer than "F24" is not a key, even when its digits spell one: "F001" is a typo, not F1. The whole name is matched.
        CHECK_EQ(ParseKeyName("F001"), kNoKey);
        CHECK_EQ(ParseKeyName("f0024"), kNoKey);
        CHECK_EQ(ParseKeyName("F10"), uint32_t(0x79));
        CHECK_EQ(ParseKeyName("F24"), uint32_t(0x87));
        CHECK_EQ(ParseKeyName("a"), uint32_t('A'));
        CHECK_EQ(ParseKeyName("k"), uint32_t('K')); // letters read as the same key in either case
        CHECK_EQ(ParseKeyName("7"), uint32_t('7'));
        CHECK_EQ(ParseKeyName("off"), kNoKey);
        CHECK_EQ(ParseKeyName("space"), kNoKey);
        CHECK_EQ(ParseKeyName(""), kNoKey);
        CHECK_EQ(ParseKeyName("-"), kNoKey);
        // Behind the key: a name that is not a key falls back to the delay, a key wins over it.
        CHECK(PlanArm(30, "F001").trigger == Trigger::Delay);
        CHECK(PlanArm(30, "F10").trigger == Trigger::Key);
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
        // a re-armed capture of the same second gets the session's capture index as a suffix
        CHECK_EQ(CaptureFolderName(t, 1), std::string("20261030-140805"));
        CHECK_EQ(CaptureFolderName(t, 2), std::string("20261030-140805-2"));
        CHECK(IsRunId(CaptureFolderName(t, 8)));
        CHECK(!IsRunId("20261030-140805-"));
        CHECK(!IsRunId("20261030-140805-2x"));
        CHECK(!IsRunId("20261030-140805-2-3"));
        std::vector<std::string> names { "20261030-140808", "20261030-140807", "20261030-140806", "20261030-140805", "20261030-140804",
                                         "20261030-140803", "20261030-140802", "20261030-140801", "20261030-140800", "notes", "" };
        const std::vector<std::string> prune = RunsToPrune(names);
        CHECK_EQ(prune.size(), size_t(1)); // nine run ids, eight kept
        CHECK_EQ(prune[0], std::string("20261030-140800")); // "notes" is not a run id: never pruned
        names.resize(3);
        CHECK(RunsToPrune(names).empty());
        CHECK_EQ(kKeepRuns, size_t(8)); // a session can start eight captures, none of them may prune its own
        // the same second's captures sort after the plain run and after each other
        std::vector<std::string> sameSecond { "20261030-140805-2", "20261030-140805", "20261030-140805-3" };
        const std::vector<std::string> kept = RunsToPrune(sameSecond, 2);
        CHECK_EQ(kept.size(), size_t(1));
        if (!kept.empty())
            CHECK_EQ(kept[0], std::string("20261030-140805"));
    }

    // ---- re-arming a key capture: one press starts one capture, and the press that started it must come back up first
    {
        CHECK_EQ(kMaxCapturesPerSession, 8u);
        CaptureArm arm;
        CHECK(!arm.Exhausted());
        CHECK(arm.Poll(true)); // the first press starts a capture
        CHECK_EQ(arm.captures, 1u);
        CHECK(!arm.Poll(true)); // the same press spends once, even if it is still held when the capture ends
        CHECK(!arm.Poll(true));
        CHECK(!arm.Poll(false)); // the release arms the next press
        CHECK(arm.Poll(true));
        CHECK_EQ(arm.captures, 2u);
        CHECK(!arm.Poll(true));
        // no key down, nothing starts
        CaptureArm idle;
        CHECK(!idle.Poll(false));
        CHECK_EQ(idle.captures, 0u);
        // the session is bounded
        CaptureArm many;
        many.captures = kMaxCapturesPerSession;
        CHECK(many.Exhausted());
        CHECK(!many.Poll(false));
        CHECK(!many.Poll(true));
        CHECK_EQ(many.captures, kMaxCapturesPerSession);
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
        // The sun mask answers to two names: the benchmark build's `g_SamplerShadow` and this game build's `g_SamplerShadowMask`, which is the
        // one its sun light draw declares (the capture's views list names the binding `g_SamplerShadowMask.T`).
        CHECK_EQ(FixtureBitsOf("g_SamplerShadowMask"), kFixtureMask);
        CHECK_EQ(FixtureBitsOf("g_SamplerShadowMask.T"), kFixtureMask);
        CHECK_EQ(FixtureBitsOf("g_SamplerShadowMask.S"), kFixtureMask);
        CHECK_EQ(FixtureBitsOf("g_SamplerShadowMaskExtra.T"), 0u); // a name that merely starts with it is another resource
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

    // ---- format 3: every resource entry carries the resource pointer, so a target and its readers can be joined
    {
        CHECK_EQ(PointerText(nullptr), std::string("0"));
        CHECK_EQ(PointerText(reinterpret_cast<const void*>(0x1a2b3c4d5e6fULL)), std::string("1a2b3c4d5e6f"));
        CHECK_EQ(PointerText(reinterpret_cast<const void*>(0x00000000ABCDEF01ULL)), std::string("abcdef01")); // no padding, no 0x
        CHECK_EQ(SrvEntryText(3, "g_SamplerHierarchicalZ.T", "R32_FLOAT", "2560x1440", reinterpret_cast<const void*>(0x1000)),
                 std::string("3:g_SamplerHierarchicalZ.T:R32_FLOAT:2560x1440:1000"));
        CHECK_EQ(SrvEntryText(1, "-", "R16G16B16A16_FLOAT", "?", nullptr), std::string("1:-:R16G16B16A16_FLOAT:?:0"));
        CHECK_EQ(CbEntryText(2, "g_LightParam", 400, reinterpret_cast<const void*>(0x2000)), std::string("2:g_LightParam:400:2000"));
        CHECK_EQ(TargetEntryText(0, "R16G16B16A16_FLOAT", "2560x1440", reinterpret_cast<const void*>(0x3000)),
                 std::string("0:R16G16B16A16_FLOAT:2560x1440:3000"));
        CHECK_EQ(DepthEntryText("D32_FLOAT", "2560x1440", nullptr), std::string("D32_FLOAT:2560x1440:0"));
    }

    // ---- views.txt: the SRV description of the textures the studies ask about, one line per name per capture
    {
        CHECK_EQ(ViewNameIndex("g_SamplerReflectionArray"), int32_t(0));
        CHECK_EQ(ViewNameIndex("g_SamplerReflectionArray.T"), int32_t(0)); // the engine's compiler appends the type suffix
        CHECK_EQ(ViewNameIndex("g_SamplerReflectionMap.T"), int32_t(1));
        CHECK_EQ(ViewNameIndex("g_SamplerHierarchicalZ"), int32_t(2));
        CHECK_EQ(ViewNameIndex("g_SamplerShadow.T"), int32_t(3));
        CHECK_EQ(ViewNameIndex("g_SamplerDepth"), int32_t(-1));
        CHECK_EQ(kViewNameCount, size_t(4));
        CHECK_EQ(SrvDimensionText(0), std::string("UNKNOWN"));
        CHECK_EQ(SrvDimensionText(1), std::string("BUFFER"));
        CHECK_EQ(SrvDimensionText(4), std::string("TEXTURE2D"));
        CHECK_EQ(SrvDimensionText(5), std::string("TEXTURE2DARRAY"));
        CHECK_EQ(SrvDimensionText(9), std::string("TEXTURECUBE"));
        CHECK_EQ(SrvDimensionText(10), std::string("TEXTURECUBEARRAY"));
        CHECK_EQ(SrvDimensionText(12), std::string("dim12")); // a value this build does not know
        ViewSample sample;
        sample.ordinal = 1260;
        sample.name = "g_SamplerReflectionArray";
        sample.viewDimension = 5;
        sample.viewMips = 1;
        sample.viewFormat = "R16G16B16A16_FLOAT";
        sample.arraySize = 256;
        sample.cubes = 0;
        sample.width = 256;
        sample.height = 256;
        sample.textureArray = 256;
        sample.textureMips = 1;
        sample.textureFormat = "R16G16B16A16_FLOAT";
        sample.pointer = reinterpret_cast<const void*>(0x1a2b3c4d5e6fULL);
        CHECK_EQ(ViewSampleText(sample),
                 std::string("g_SamplerReflectionArray,1260,TEXTURE2DARRAY,1,R16G16B16A16_FLOAT,256,0,256,256,256,1,R16G16B16A16_FLOAT,1a2b3c4d5e6f\n"));
        const std::vector<std::string> fields = Fields(ViewSampleText(sample));
        const std::vector<std::string> header = Fields(ViewsHeaderText());
        CHECK_EQ(fields.size(), header.size());
        CHECK_EQ(header[0], std::string("name"));
        CHECK_EQ(header[12], std::string("pointer"));
        CHECK(ViewsFileHeader().find("format 3") != std::string::npos);
        // A cube array reports its cube count and its array size in slices.
        sample.viewDimension = 10;
        sample.cubes = 4;
        sample.arraySize = 24;
        CHECK_EQ(Fields(ViewSampleText(sample))[6], std::string("4"));
        CHECK_EQ(Fields(ViewSampleText(sample))[5], std::string("24"));
    }

    // ---- cbdump.csv: one row per (sample, named buffer), the bytes in hex and the buffer pointer
    {
        CHECK_EQ(kCbDumpNameCount, size_t(18));
        CHECK_EQ(CbDumpNameIndex("g_WetnessParameter"), int32_t(0));
        CHECK_EQ(CbDumpNameIndex("g_AmbientParamArray"), int32_t(1));
        CHECK_EQ(CbDumpNameIndex("g_CameraParameter"), int32_t(5)); // the table an offline frame test rebuilds the pass's inputs from
        CHECK_EQ(CbDumpNameIndex("cFogParam"), int32_t(13));
        // The engine's flare family, appended after the buffers the earlier captures sampled: their values were never readable before.
        CHECK_EQ(CbDumpNameIndex("cHaloParam"), int32_t(14));
        CHECK_EQ(CbDumpNameIndex("cSunParam"), int32_t(15));
        CHECK_EQ(CbDumpNameIndex("cRadialBlurParam"), int32_t(16));
        CHECK_EQ(CbDumpNameIndex("g_ShadowBiasParameter"), int32_t(17)); // the engine shadow block's bias, read beside the atlas
        CHECK_EQ(CbDumpNameIndex("g_LightParam"), int32_t(-1)); // read into cb.csv, not this list
        CHECK_EQ(CbDumpNameIndex("g_WetnessParameter.T"), int32_t(0)); // compared by base name
        CHECK_EQ(kCbDumpHexMax, 16384u);
        CHECK_EQ(kCbDumpBytes, uint64_t(4) << 20);
        CHECK_EQ(HexBytes(nullptr, 0), std::string(""));
        const unsigned char small[] { 0x00, 0x0F, 0xA5, 0xFF };
        CHECK_EQ(HexBytes(small, sizeof small), std::string("000fa5ff"));
        CbDumpRow row;
        row.frame = 3;
        row.seconds = 12.5;
        row.psCrc = 0xADE72072u;
        row.hasCrc = true;
        row.name = "g_WetnessParameter";
        row.slot = 4;
        row.bytes = 32;
        row.pointer = reinterpret_cast<const void*>(0x1a2b3c4d5e6fULL);
        row.data.assign(small, small + sizeof small);
        const std::vector<std::string> header = Fields(CbDumpHeaderText());
        CHECK_EQ(header.size(), size_t(8));
        CHECK_EQ(header[0], std::string("frame"));
        CHECK_EQ(header[6], std::string("ptr"));
        CHECK_EQ(header[7], std::string("hex"));
        const std::vector<std::string> fields = Fields(CbDumpRowText(row));
        CHECK_EQ(fields.size(), header.size());
        CHECK_EQ(fields[0], std::string("3"));
        CHECK_EQ(fields[1], std::string("12.500"));
        CHECK_EQ(fields[2], std::string("0xADE72072"));
        CHECK_EQ(fields[3], std::string("g_WetnessParameter"));
        CHECK_EQ(fields[4], std::string("4"));
        CHECK_EQ(fields[5], std::string("32"));
        CHECK_EQ(fields[6], std::string("1a2b3c4d5e6f"));
        CHECK_EQ(fields[7], std::string("000fa5ff"));
        // a read of a shader whose reflection was not recorded, and a buffer that could not be read
        row.hasCrc = false;
        row.data.clear();
        CHECK_EQ(Fields(CbDumpRowText(row))[2], std::string("-"));
        CHECK_EQ(Fields(CbDumpRowText(row))[7], std::string(""));
        // the cap rule admits a row that fits and refuses one that does not
        CHECK(CbDumpFits(kCbDumpBytes - 10, 10));
        CHECK(!CbDumpFits(kCbDumpBytes - 9, 10));
    }

    // ---- the self-check trailer: the last line of each file says what the file holds, so a truncated copy is caught by the reader
    {
        CHECK_EQ(DrawsTrailerText(2104, 3, 7), std::string("# rows 2104 frames 3 omitted 7\n"));
        CHECK_EQ(DrawsTrailerText(0, 0, 0), std::string("# rows 0 frames 0 omitted 0\n"));
        CHECK_EQ(RowsTrailerText(3), std::string("# rows 3\n"));
        CHECK(DrawsTrailerText(1, 1, 0).front() == '#'); // a comment line, so a CSV reader skips it
        // The trailer counts the rows the file holds. A frame the byte cap dropped is not in the file, so it counts neither its rows nor
        // its frame line: counting rows the `kDrawsBytes` cap dropped made the reader report a false "cut off".
        FrameTally tally = TallyFrame(100, 3, true);
        CHECK(tally.frame);
        CHECK_EQ(tally.rows, uint64_t(97));
        CHECK_EQ(tally.omitted, uint64_t(3));
        tally = TallyFrame(100, 3, false);
        CHECK(!tally.frame);
        CHECK_EQ(tally.rows, uint64_t(0));
        CHECK_EQ(tally.omitted, uint64_t(0));
        uint64_t rows = 0, frames = 0, omitted = 0;
        for (const bool appended : { true, false, true })
        {
            const FrameTally note = TallyFrame(10, 1, appended);
            rows += note.rows;
            frames += note.frame ? 1 : 0;
            omitted += note.omitted;
        }
        CHECK_EQ(DrawsTrailerText(rows, frames, omitted), std::string("# rows 18 frames 2 omitted 2\n"));
    }

    // ---- the one-frame dump's rules: what identifies a pass, the key, the size class and the bounds
    {
        CHECK(IsWetnessPassShape(true, true, true, 1));
        CHECK(!IsWetnessPassShape(true, true, false, 1));
        CHECK(!IsWetnessPassShape(true, false, true, 1));
        CHECK(!IsWetnessPassShape(false, true, true, 1));
        CHECK(!IsWetnessPassShape(true, true, true, 0));
        CHECK(!IsWetnessPassShape(true, true, true, 2));
        CHECK(IsGbufferShape(5));
        CHECK(!IsGbufferShape(4) && !IsGbufferShape(6));
        CHECK(DumpEnabled("frame"));
        CHECK(!DumpEnabled("capture")); // the main key's value is not the dump's
        CHECK(!DumpEnabled("off"));
        CHECK(!DumpEnabled(""));
        CHECK(!DumpEnabled("FRAME")); // an ini value is matched exactly, like the main key
        CHECK(SameTextureSize(2560, 1440, 2560, 1440));
        CHECK(!SameTextureSize(2560, 1440, 1280, 720));
        CHECK(!SameTextureSize(0, 0, 0, 0));
        CHECK(IsFp16Format(10) && IsFp16Format(34) && IsFp16Format(54)); // R16G16B16A16_FLOAT, R16G16_FLOAT, R16_FLOAT
        CHECK(!IsFp16Format(28) && !IsFp16Format(0) && !IsFp16Format(11));
        CHECK_EQ(kMaxDumpTextures, size_t(20));
        CHECK_EQ(kMaxDumpBytes, uint64_t(256) << 20);
        // the priority order: the wetness target first, then the G-buffer, the lights, the SSR target, the scene, the view position, the rest
        CHECK(uint8_t(DumpRank::Wetness) < uint8_t(DumpRank::Gbuffer));
        CHECK(uint8_t(DumpRank::Gbuffer) < uint8_t(DumpRank::Light));
        CHECK(uint8_t(DumpRank::Light) < uint8_t(DumpRank::Ssr));
        CHECK(uint8_t(DumpRank::Ssr) < uint8_t(DumpRank::Scene));
        CHECK(uint8_t(DumpRank::Scene) < uint8_t(DumpRank::ViewPosition));
        CHECK(uint8_t(DumpRank::ViewPosition) < uint8_t(DumpRank::Rest));
        CHECK(uint8_t(DumpRank::Rest) < uint8_t(DumpRank::Atlas)); // the atlas is the first item a contended bound displaces
        CHECK(uint8_t(DumpRank::Atlas) < uint8_t(DumpRank::WaterLights)); // the water gate's pair is displaced before the atlas
        CHECK(uint8_t(DumpRank::WaterLights) < uint8_t(DumpRank::WaterGate)); // and the gate's other two files before the pair
        CHECK(!DumpYieldsTo(DumpRank::Atlas, DumpRank::WaterLights)); // the atlas keeps its place against a water-gate newcomer
        CHECK(!DumpYieldsTo(DumpRank::WaterLights, DumpRank::WaterGate)); // and the pair against the gate's other files
        CHECK(DumpYieldsTo(DumpRank::WaterGate, DumpRank::Atlas)); // the gate's files are what a contended bound displaces first
        CHECK(DumpYieldsTo(DumpRank::WaterLights, DumpRank::Atlas)); // then the pair, then the atlas, then everything above
        CHECK(DumpFits(0, 0, kMaxDumpBytes)); // exactly the bound fits
        CHECK(!DumpFits(0, 0, kMaxDumpBytes + 1));
        CHECK(!DumpFits(kMaxDumpTextures, 0, 1)); // the count bound
        CHECK(DumpFits(kMaxDumpTextures - 1, 0, 1));
        DumpFact fact;
        fact.name = "wetness";
        fact.format = "R16G16B16A16_FLOAT";
        fact.width = 2560;
        fact.height = 1440;
        fact.bytes = uint64_t(2560) * 1440 * 8;
        fact.rowPitch = 20480;
        fact.ordinal = 912;
        fact.pointer = reinterpret_cast<const void*>(0x1a2b3c4d5e6fULL);
        CHECK_EQ(DumpFactText(fact), std::string("wetness.bin,R16G16B16A16_FLOAT,2560x1440,29491200,20480,912,1a2b3c4d5e6f\n"));
        CHECK_EQ(Fields(DumpFactText(fact)).size(), size_t(7));
        CHECK(DumpFactsHeader().find("format 3") != std::string::npos);
        CHECK(DumpFactsHeader().find("vposition.bin") != std::string::npos);
        // The view-position buffer is identified by either name the engine gives it: the composite and the wetness pass declare
        // `g_SamplerViewPosition`, the water shaders `g_SamplerVPosition`, and both bind the same resource. A dump that knew only the first
        // name lost the target in a capture where only the water draws were named.
        CHECK_EQ(kViewPositionNameCount, size_t(2));
        CHECK(IsViewPositionName("g_SamplerViewPosition"));
        CHECK(IsViewPositionName("g_SamplerVPosition"));
        CHECK(!IsViewPositionName("g_SamplerVPositionDepth"));
        CHECK(!IsViewPositionName("g_SamplerVPosition.T")); // the caller strips the effect compiler's suffix before asking
        CHECK(!IsViewPositionName(""));
        CHECK_EQ(DumpSkipText("ssr", "multisampled"), std::string("# skipped ssr.bin: multisampled\n"));
        // The dump reads one whole window: nothing is identified while the splitter still waits for the tone map that opens it. The guard
        // is the splitter's `fresh`, not its `toneSeen`: a bridge Present clears the tone state every frame, and a guard on that would
        // identify nothing in a bridged session (and would take the partial frame's tone map).
        CHECK(!DumpIdentifies(true)); // the capture started mid-frame
        CHECK(DumpIdentifies(false));
        {
            FrameSplitter split;
            CHECK(!DumpIdentifies(split.fresh)); // the arming tone map belongs to the partial frame
            CHECK_EQ(split.NoteDraw(kStageToneMap, 1), uint8_t(kSplitArm));
            CHECK(DumpIdentifies(split.fresh)); // window 1's closing tone map is the dump's anchor
            split.Reset();
            CHECK(!split.toneSeen);
            CHECK(DumpIdentifies(split.fresh)); // a Present is a real frame boundary too
        }
        // A copy is only queued at a later draw, and "later" is the capture's own counter: window 1's closing tone map identifies at
        // ordinal 5000, window 2 starts at ordinal 1. The ordinal comparison never fires again; the counter keeps counting.
        CHECK(DumpCopyDue(false, false, 5000, 5001));
        CHECK(!DumpCopyDue(false, false, 5001, 5001)); // never at the draw that identified the texture
        CHECK(!DumpCopyDue(false, false, 5001, 5000));
        CHECK(!DumpCopyDue(true, false, 5000, 5001));  // already copied
        CHECK(!DumpCopyDue(false, true, 5000, 5001));  // a texture that was skipped keeps its one facts.txt line
        CHECK(DumpAfterTone(5000, 5001));
        CHECK(!DumpAfterTone(5000, 5000));
        CHECK(!DumpAfterTone(5000, 1)); // the next frame's ordinal is not "after" the tone map's own
        // The G-buffer waits for a later draw that samples it; the light buffers are identified at the draw that samples them, so they are
        // complete there and are copied at the next draw.
        CHECK(DumpWaitsForSampling(DumpRank::Gbuffer));
        CHECK(!DumpWaitsForSampling(DumpRank::Wetness));
        CHECK(!DumpWaitsForSampling(DumpRank::Light));
        CHECK(!DumpWaitsForSampling(DumpRank::Ssr));
        CHECK(!DumpWaitsForSampling(DumpRank::Scene));
        CHECK(!DumpWaitsForSampling(DumpRank::ViewPosition));
        CHECK(!DumpWaitsForSampling(DumpRank::Rest));
        CHECK(!DumpWaitsForSampling(DumpRank::Atlas)); // the atlas is read-only in the frame; any later draw's content is final

        // The shadow parameter's per-pointer cadence: one name, one instance per cascade, each its own row a window.
        CHECK_EQ(CbDumpPointersPerSecond("g_DirectionalShadowParameter"), 4u);
        CHECK_EQ(CbDumpPointersPerSecond("g_CameraParameter"), 1u);
        CHECK_EQ(CbDumpPointersPerSecond("g_WetnessParameter"), 1u);
        {
            CbDumpPointerCadence cadence {};
            const void* a = reinterpret_cast<const void*>(0x1000);
            const void* b = reinterpret_cast<const void*>(0x2000);
            CHECK(CbDumpPointerDue(cadence, a, 100, 10)); // an unread pointer is always due
            CHECK_EQ(CbDumpPointerRecord(cadence, a, 100), size_t(0));
            CHECK(!CbDumpPointerDue(cadence, a, 105, 10)); // inside its window: quiet
            CHECK(CbDumpPointerDue(cadence, a, 111, 10));  // past it: due again
            CHECK_EQ(CbDumpPointerRecord(cadence, b, 105), size_t(1)); // an empty slot, in order
            CHECK(CbDumpPointerIndex(cadence, a) >= 0 && CbDumpPointerIndex(cadence, b) >= 0);
            CHECK_EQ(CbDumpPointerIndex(cadence, reinterpret_cast<const void*>(0x3000)), int32_t(-1));
            for (size_t k = 0; k < 6; ++k) // the ring rounds over and never carries one pointer in two slots
                CbDumpPointerRecord(cadence, reinterpret_cast<const void*>(0x3000 + k), 200 + int64_t(k));
            CbDumpPointerRecord(cadence, reinterpret_cast<const void*>(0x9000), 300); // a full ring evicts the oldest read
            CHECK_EQ(CbDumpPointerIndex(cadence, reinterpret_cast<const void*>(0x9000)), int32_t(0));
            CHECK_EQ(CbDumpPointerIndex(cadence, a), int32_t(-1)); // slot 0 held `a`, the window's first read
            bool distinct = true;
            for (size_t i = 0; i < kCbDumpPointerRing; ++i)
                for (size_t j = i + 1; j < kCbDumpPointerRing; ++j)
                    if (cadence.who[i] != nullptr && cadence.who[i] == cadence.who[j])
                        distinct = false;
            CHECK(distinct);
            CbDumpPointerCadence unknown {};
            CHECK_EQ(CbDumpPointerRecord(unknown, nullptr, 1), size_t(0)); // a null buffer records nothing
            CHECK(unknown.who[0] == nullptr);
        }
        // A pass, from the draw's own texture slots: the five targets are identified at draw A (sequence 10). The draw right after it
        // (11) binds none of them, the draw at sequence 20 binds gbuffer0 in its second slot. Only the second one queues a copy, so a pass
        // whose match is broken copies at 11 (or never) and the checks below say so.
        const void* gbuffer0 = reinterpret_cast<const void*>(0x2000);
        const void* other = reinterpret_cast<const void*>(0x3000);
        const void* later[] { other, nullptr };    // a draw that reads something else
        const void* sampling[] { other, gbuffer0 }; // the draw that reads the target back
        const auto copyQueued = [gbuffer0](uint64_t now, const void* const* slots, size_t slots_)
        {
            return DumpCopyDue(false, false, 10, now) && DumpWaitsForSampling(DumpRank::Gbuffer) &&
                   DumpSampledSlot(slots, slots_, gbuffer0) >= 0;
        };
        CHECK(!copyQueued(11, later, 2));   // the draw right after the five-target draw is still inside the geometry pass
        CHECK(!copyQueued(20, later, 2));   // a later draw that binds another texture copies nothing
        CHECK(copyQueued(20, sampling, 2)); // the copy fires at the draw that samples the target
        CHECK_EQ(DumpSampledSlot(sampling, 2, gbuffer0), int32_t(1));
        CHECK_EQ(DumpSampledSlot(later, 2, gbuffer0), int32_t(-1));
        CHECK_EQ(DumpSampledSlot(later, 2, nullptr), int32_t(-1)); // a released item is never matched by a null slot
        CHECK(DumpCopyDue(false, false, 15, 16) && !DumpWaitsForSampling(DumpRank::Light)); // a light buffer at A+1
        // The staging copies are bounded at identification: a 4K FP16 target is 66,355,200 bytes, and the 256 MiB bound holds four.
        const uint64_t uhd = uint64_t(3840) * 2160 * 8;
        CHECK(DumpIdentFits(uhd * 3, uhd));
        CHECK(!DumpIdentFits(uhd * 4, uhd)); // a fifth does not fit
        CHECK(DumpIdentFits(kMaxDumpBytes - uhd, uhd)); // exactly the bound fits
        CHECK(!DumpIdentFits(kMaxDumpBytes - uhd + 1, uhd));
        // A 4K frame's whole set - the five G-buffer targets, the two light buffers, the SSR target, the scene, the view position, the tone map
        // and the copy - is twelve targets at the worst case of a 4K FP16 one (66,355,200 bytes): it fits a 1024 MiB budget and not the default
        // 256 MiB, which is why a session that wants the whole set of a 4K frame raises the key.
        CHECK(DumpFits(0, 0, uhd * 12, DumpBudgetBytes(1024)));
        CHECK(!DumpFits(0, 0, uhd * 12, DumpBudgetBytes(256))); // and not the default bound, which the session raises for its dump
        CHECK_EQ(DumpBudgetBytes(1024), uint64_t(1024) << 20);
        // The frame that identified the tone map is over when a later frame was closed, or when it was itself closed at a bridge Present:
        // a two-frame capture in a session with a bridge ends on exactly that Present, and its dump must not be lost there.
        CHECK(!DumpFrameOver(0, 0, false));
        CHECK(!DumpFrameOver(1, 2, true)); // the tone-map frame has not been closed yet
        CHECK(!DumpFrameOver(2, 2, false)); // a tone-map split: the frame's own close is not enough
        CHECK(DumpFrameOver(2, 2, true));
        CHECK(DumpFrameOver(3, 2, false));
        CHECK(DumpFrameOver(3, 2, true));
        // The end of a capture: a G-buffer target is skipped (no draw sampled it in the captured frame, so a copy now would hold a later
        // frame's content), a bridge Present has already run the identifying draw, and a tone-map split's closing draw has not.
        CHECK(DumpAtEnd(DumpRank::Gbuffer, 5000, 5000, true) == DumpEnd::Skip);
        CHECK(DumpAtEnd(DumpRank::Gbuffer, 5000, 6000, false) == DumpEnd::Skip);
        CHECK(DumpAtEnd(DumpRank::Scene, 5000, 5000, true) == DumpEnd::Copy);
        CHECK(DumpAtEnd(DumpRank::Scene, 5000, 5000, false) == DumpEnd::Leave);
        CHECK(DumpAtEnd(DumpRank::Scene, 5000, 5001, false) == DumpEnd::Copy);
        // The view position is identified at the draw that samples it, so it is complete there like a light buffer.
        CHECK(DumpAtEnd(DumpRank::ViewPosition, 5000, 5000, true) == DumpEnd::Copy);
        CHECK(DumpAtEnd(DumpRank::ViewPosition, 5000, 5000, false) == DumpEnd::Leave);
        CHECK(DumpAtEnd(DumpRank::ViewPosition, 5000, 5001, false) == DumpEnd::Copy);
    }

    // ---- the byte bound with priority: a texture identified later still fits when the holds of a lower priority give their place back,
    // so the wetness target is never displaced by the G-buffer and the light buffers merely because they came first
    {
        const uint64_t uhd = uint64_t(3840) * 2160 * 8; // a 4K FP16 target, 66,355,200 bytes
        // Four of them fit in 256 MiB and the fifth does not: the wetness target that arrives after four G-buffer targets takes the place
        // of one of them, and the earliest of the equal priorities is the one that gives way.
        DumpHold holds[4] { { DumpRank::Gbuffer, uhd, false },
                            { DumpRank::Gbuffer, uhd, false },
                            { DumpRank::Gbuffer, uhd, false },
                            { DumpRank::Gbuffer, uhd, false } };
        CHECK(!DumpIdentFits(uhd * 4, uhd)); // first come, first served would refuse the newcomer
        const uint64_t freed = DumpMakeRoom(holds, 4, DumpRank::Wetness, uhd, uhd * 4);
        CHECK_EQ(freed, uhd);
        CHECK(holds[0].released && !holds[1].released && !holds[3].released);
        CHECK(DumpIdentFits(uhd * 4 - freed, uhd));
        // The lowest priority goes first whatever the order of the set: two light buffers and two G-buffer targets fill a bound of three,
        // and the two lights are what the wetness target displaces while the G-buffer targets keep their places.
        DumpHold mixed[4] { { DumpRank::Gbuffer, uhd, false },
                            { DumpRank::Light, uhd, false },
                            { DumpRank::Gbuffer, uhd, false },
                            { DumpRank::Light, uhd, false } };
        CHECK_EQ(DumpMakeRoom(mixed, 4, DumpRank::Wetness, uhd, uhd * 4, uhd * 3), uhd * 2);
        CHECK(!mixed[0].released && !mixed[2].released);
        CHECK(mixed[1].released && mixed[3].released);
        // Nothing of an equal or a higher priority gives its place away: the caller refuses the newcomer instead.
        DumpHold wetness { DumpRank::Wetness, uhd, false };
        CHECK_EQ(DumpMakeRoom(&wetness, 1, DumpRank::Gbuffer, uhd, uhd, uhd), uint64_t(0));
        CHECK(!wetness.released);
        DumpHold equal { DumpRank::Gbuffer, uhd, false };
        CHECK_EQ(DumpMakeRoom(&equal, 1, DumpRank::Gbuffer, uhd, uhd, uhd), uint64_t(0)); // the first of two equals keeps its place
        CHECK(!equal.released);
        // And a set that fits gives nothing away at all.
        DumpHold fits { DumpRank::Light, uhd, false };
        CHECK_EQ(DumpMakeRoom(&fits, 1, DumpRank::Wetness, uhd, uhd, uhd * 2), uint64_t(0));
        CHECK(!fits.released);
        // A newcomer larger than the whole bound can never fit: nothing is released for it, so a single oversized texture does not wipe the
        // dump's holds.
        DumpHold large { DumpRank::Gbuffer, uhd, false };
        CHECK_EQ(DumpMakeRoom(&large, 1, DumpRank::Wetness, kMaxDumpBytes + 1, uhd, kMaxDumpBytes), uint64_t(0));
        CHECK(!large.released);
    }

    // ---- the dump's frame end, walked on the pure rules: a two-frame capture in a session with a bridge Present ends at the Present that
    // closes the tone map's own frame, so the dump has to be written there; three frames and both tone-map-split cases end at a later frame
    // or at the end of the capture. Nothing may be written twice, and a G-buffer target no draw sampled never with the dump.
    for (const bool presentMode : { false, true })
        for (const uint32_t wanted : { 2u, 3u })
            for (const bool sampled : { true, false })
            {
                DumpWalk walk;
                walk.presentMode = presentMode;
                walk.wanted = wanted;
                walk.sampledGbuffer = sampled;
                walk.Run();
                CHECK_EQ(walk.writes, 1u); // exactly one dump per capture
                CHECK_EQ(walk.lost, 0u);
                CHECK(!walk.copiedUnsampled);
                // The unfinished G-buffer targets are the end of a capture's business: skipped, one line each, never copied.
                CHECK_EQ(walk.endSkips, (!sampled && presentMode && wanted == 2) ? 5u : 0u);
                CHECK_EQ(walk.endCopies, 0u); // every other copy was taken before the dump
            }

    // ---- the same walk with the tone map as the last draw of the frame that ends the capture: a bridge Present has already run it, so the
    // scene and tone-map copies are taken at the end of the capture, which the tone-map split's closing draw may not do
    {
        DumpWalk walk;
        walk.presentMode = true;
        walk.wanted = 2;
        walk.tailAfterTone = 0; // the tone map closes the engine frame
        walk.Run();
        CHECK_EQ(walk.writes, 1u);
        CHECK_EQ(walk.lost, 0u);
        CHECK_EQ(walk.endCopies, 2u); // scene and tonemap, identified at the closing draw
        CHECK(!walk.copiedUnsampled);
        // The tone-map split writes the dump from the closing tone map's own draw hook, before that draw, so it leaves those copies to the
        // next draw: the same capture in a session without a bridge still writes exactly one dump.
        DumpWalk toneSplit;
        toneSplit.wanted = 2;
        toneSplit.tailAfterTone = 0;
        toneSplit.Run();
        CHECK_EQ(toneSplit.writes, 1u);
        CHECK_EQ(toneSplit.lost, 0u);
        CHECK_EQ(toneSplit.endCopies, 0u);
        CHECK(!toneSplit.copiedUnsampled);
    }

    // ---- a skipped texture is held once: the wetness pass, the light draws and the five-target draw repeat inside one captured frame, and
    // facts.txt must name a skipped texture once, not once per draw that identifies it again
    {
        struct Held
        {
            std::string name;
            std::string note;
        };
        std::vector<Held> held;
        const void* resource = reinterpret_cast<const void*>(0x2C);
        // One draw that identifies the wetness target, which this dump cannot take (a multisampled target).
        const auto identify = [&held, resource]
        {
            bool already = false;
            size_t written = 0;
            for (const Held& item : held)
            {
                already = already || item.name == "wetness";
                written += item.note.empty() ? 1 : 0;
            }
            if (!DumpAdds(true, false, resource, already, written))
                return;
            held.push_back(Held { "wetness", "multisampled" });
        };
        for (int draw = 0; draw < 40; ++draw)
            identify();
        CHECK_EQ(held.size(), size_t(1));
        std::string facts;
        for (const Held& item : held)
            facts += DumpSkipText(item.name, item.note);
        CHECK_EQ(Count(facts, "\n"), size_t(1)); // one line in the whole file
        CHECK_EQ(Count(facts, DumpSkipText("wetness", "multisampled")), size_t(1));
        // A skip does not use up one of the sixteen written places, and a full set of them refuses one more identification.
        const size_t full = kMaxDumpTextures;
        CHECK(!DumpAdds(true, false, resource, false, full));
        CHECK(DumpAdds(true, false, resource, false, full - 1));
        CHECK(!DumpAdds(false, false, resource, false, 0)); // the dump is off
        CHECK(!DumpAdds(true, true, resource, false, 0));   // it was written already
        CHECK(!DumpAdds(true, false, nullptr, false, 0));   // no resource behind the slot
        CHECK(!DumpAdds(true, false, resource, true, 0));   // the name is held already
    }

    // ---- the shaders kept from creation: which reflected names make a shader one of the shapes, and the bound it is kept under
    {
        // One bit per name, and the effect compiler's `.T`/`.S` suffix is not part of the comparison (a texture is only ever named
        // `g_SamplerGBuffer3.T` in the reflection).
        CHECK_EQ(CreationNameBitsOf("g_AmbientParamArray"), kKeepAmbientParam);
        CHECK_EQ(CreationNameBitsOf("g_WetnessParameter"), kKeepWetnessParam);
        CHECK_EQ(CreationNameBitsOf("g_SamplerGBuffer3.T"), kKeepGbuffer3);
        CHECK_EQ(CreationNameBitsOf("g_SamplerViewPosition.S"), kKeepViewPosition);
        CHECK_EQ(CreationNameBitsOf("g_SamplerHierarchicalZ"), kKeepHierarchicalZ);
        CHECK_EQ(CreationNameBitsOf("g_SamplerShadowMask"), kKeepShadowMask);
        CHECK_EQ(CreationNameBitsOf("g_SamplerShadowMask.T"), kKeepShadowMask);
        CHECK_EQ(CreationNameBitsOf("g_SamplerShadow"), 0u); // the other build's name: this list keeps what this game build creates
        CHECK_EQ(CreationNameBitsOf("cGlareParam"), kKeepGlareParam);
        CHECK_EQ(CreationNameBitsOf("cBrightPassParam"), kKeepBrightPassParam);
        CHECK_EQ(CreationNameBitsOf("cMergeWeight"), kKeepMergeWeight);
        CHECK_EQ(CreationNameBitsOf("cGaussianBlurParam"), kKeepGaussianBlurParam);
        CHECK_EQ(CreationNameBitsOf("cSoftFocusParam"), kKeepSoftFocusParam);
        // The engine's flare family: kept at creation for the same reason the glare chain is, so a capture says what the ring, the sun sprite
        // and the ghost discs were drawn with.
        CHECK_EQ(CreationNameBitsOf("cSunParam"), kKeepSunParam);
        CHECK_EQ(CreationNameBitsOf("cHaloParam"), kKeepHaloParam);
        CHECK_EQ(CreationNameBitsOf("cRadialBlurParam"), kKeepRadialBlurParam);
        CHECK_EQ(CreationNameBitsOf("cSunParameter"), 0u); // one character long, and a different buffer
        CHECK_EQ(CreationNameBitsOf("g_SamplerDiffuse.T"), 0u);
        CHECK_EQ(CreationNameBitsOf("g_AmbientParam"), 0u); // one character short, and a different buffer

        // The shapes: the ambient table alone, the hierarchical-Z alone, any glare parameter, or all three wetness names.
        CHECK(IsCreationFixture(kKeepAmbientParam));
        CHECK(IsCreationFixture(kKeepHierarchicalZ));
        // This game build's sun light draw declares g_SamplerShadowMask (beside g_LightParam) and is created while the game loads, so a
        // capture-only fixture list would never hold the shader the mask's meaning is read from.
        CHECK(IsCreationFixture(kKeepShadowMask));
        CHECK(IsCreationFixture(kKeepGlareParam));
        CHECK(IsCreationFixture(kKeepBrightPassParam));
        CHECK(IsCreationFixture(kKeepMergeWeight));
        CHECK(IsCreationFixture(kKeepGaussianBlurParam));
        CHECK(IsCreationFixture(kKeepSoftFocusParam));
        CHECK(IsCreationFixture(kKeepSunParam));
        CHECK(IsCreationFixture(kKeepHaloParam));
        CHECK(IsCreationFixture(kKeepRadialBlurParam));
        CHECK(!IsCreationFixture(0));
        CHECK(!IsCreationFixture(kKeepWetnessParam)); // the buffer alone is not the pass
        CHECK(!IsCreationFixture(kKeepWetnessParam | kKeepGbuffer3));
        CHECK(!IsCreationFixture(kKeepGbuffer3 | kKeepViewPosition));
        CHECK(IsCreationFixture(kCreationWetness));
        CHECK(IsCreationFixture(kKeepWetnessParam | kKeepGbuffer3 | kKeepViewPosition | kKeepGlareParam));

        // The bound: 32 shaders and 2 MiB, first come. A shape the list did not expect is never admitted, whatever room is left.
        CHECK_EQ(kMaxCreationFixtures, size_t(32));
        CHECK_EQ(kMaxCreationFixtureBytes, uint64_t(2) << 20);
        CHECK(AdmitCreationFixture(kKeepAmbientParam, 0, 0, 4096));
        CHECK(AdmitCreationFixture(kKeepAmbientParam, kMaxCreationFixtures - 1, 0, 4096));
        CHECK(!AdmitCreationFixture(kKeepAmbientParam, kMaxCreationFixtures, 0, 4096));
        CHECK(AdmitCreationFixture(kKeepAmbientParam, 0, kMaxCreationFixtureBytes - 4096, 4096));
        CHECK(!AdmitCreationFixture(kKeepAmbientParam, 0, kMaxCreationFixtureBytes - 4000, 4096));
        CHECK(!AdmitCreationFixture(0, 0, 0, 4096));
        CHECK(!AdmitCreationFixture(kCreationWetness, kMaxCreationFixtures, 0, 4096));

        // The byte scan before the reflection parse: one pass, and the names as the blob holds them (with the type suffix).
        const std::string wetnessPass = "DXBCg_WetnessParameterg_SamplerGBuffer3.Tg_SamplerViewPosition.S";
        CHECK(MentionsCreationFixtureName(wetnessPass.data(), wetnessPass.size()));
        const std::string glare = "DXBC" + std::string(24, '\0') + "cSoftFocusParam";
        CHECK(MentionsCreationFixtureName(glare.data(), glare.size()));
        for (const char* flare : { "cHaloParam", "cSunParam", "cRadialBlurParam" })
        {
            const std::string blob = "DXBC" + std::string(24, '\0') + flare;
            CHECK(MentionsCreationFixtureName(blob.data(), blob.size()));
        }
        CHECK(MentionsCreationFixtureName("g_AmbientParamArray", sizeof("g_AmbientParamArray") - 1)); // the name at the very end counts
        CHECK(MentionsCreationFixtureName("g_SamplerShadowMask.T", sizeof("g_SamplerShadowMask.T") - 1));
        const std::string plain = "DXBC" + std::string(24, '\0') + "g_SamplerDiffuse.T\0g_LightParam\0";
        CHECK(!MentionsCreationFixtureName(plain.data(), plain.size()));
        CHECK(!MentionsCreationFixtureName(nullptr, 0));
        CHECK(!MentionsCreationFixtureName("DXBC", 4));

        // The listing text: the origin column is what tells a shader kept at creation from one kept while the capture ran, and an old
        // format-3 file's two-column head stays readable (a reader takes a missing origin as `capture`).
        CHECK(Count(FixtureListHeader(), "crc,size,origin,name=bindPoint") == 1);
        CHECK(Count(FixtureListHeader(), "creation = kept when the shader was created") == 1);
        const std::vector<std::string> names { "g_AmbientParamArray=4", "g_SamplerLightDiffuse.T=2" };
        const std::string creationLine = FixtureListLine(0xADE72072u, 1234, true, names);
        CHECK_EQ(Count(creationLine, "\n"), size_t(1));
        CHECK(creationLine.rfind("ADE72072,1234,creation,g_AmbientParamArray=4,g_SamplerLightDiffuse.T=2\n", 0) == 0);
        const std::string captureLine = FixtureListLine(0x00112233u, 64, false, names);
        CHECK(captureLine.rfind("00112233,64,capture,", 0) == 0);
    }

    // ---- the dump's byte budget ([FFXIV] DrawDiagnosticsDumpBudgetMiB): 64 to 4096 MiB, 256 today's break-even
    {
        CHECK_EQ(kMinDumpBudgetMiB, 64);
        CHECK_EQ(kMaxDumpBudgetMiB, 4096);
        CHECK_EQ(kDefaultDumpBudgetMiB, 256);
        CHECK_EQ(DumpBudgetBytes(kDefaultDumpBudgetMiB), kMaxDumpBytes); // the default is today's bound
        CHECK_EQ(kMaxDumpBytes, uint64_t(256) << 20);
        CHECK_EQ(DumpBudgetBytes(1024), uint64_t(1024) << 20);
        CHECK_EQ(DumpBudgetBytes(1), uint64_t(64) << 20);      // clamped up
        CHECK_EQ(DumpBudgetBytes(100000), uint64_t(4096) << 20); // clamped down
        CHECK_EQ(DumpBudgetBytes(-5), uint64_t(64) << 20);

        // The rule with the budget as a parameter: the same texture fits a larger budget and not a smaller one, and the sixteen places are
        // a cap of their own.
        const uint64_t budget = DumpBudgetBytes(64);
        CHECK(DumpFits(0, budget - 3, 3, budget));
        CHECK(!DumpFits(0, budget - 2, 3, budget));
        CHECK(DumpFits(kMaxDumpTextures - 1, 0, 1, budget));
        CHECK(!DumpFits(kMaxDumpTextures, 0, 1, budget));
        CHECK(DumpIdentFits(budget - 1, 1, budget));
        CHECK(!DumpIdentFits(budget, 1, budget));

        // The priority eviction with that budget: a wetness target identified after the G-buffer and the light buffers releases the lowest
        // priority that frees enough, and nothing is released while only higher priorities are held.
        DumpHold holds[3];
        holds[0] = DumpHold { DumpRank::Gbuffer, 6, false };
        holds[1] = DumpHold { DumpRank::Light, 3, false };
        holds[2] = DumpHold { DumpRank::Ssr, 2, false };
        CHECK_EQ(DumpMakeRoom(holds, 3, DumpRank::Wetness, 4, 11, 10), uint64_t(5)); // the SSR target, then the light buffer
        CHECK(holds[1].released && holds[2].released && !holds[0].released);
        DumpHold tight[2];
        tight[0] = DumpHold { DumpRank::Wetness, 4, false };
        tight[1] = DumpHold { DumpRank::Rest, 1, false };
        CHECK_EQ(DumpMakeRoom(tight, 2, DumpRank::Rest, 4, 5, 5), uint64_t(0)); // a higher priority gives nothing back
        CHECK(!tight[0].released && !tight[1].released);
        CHECK_EQ(DumpMakeRoom(tight, 2, DumpRank::Rest, 6, 5, 5), uint64_t(0)); // larger than the whole budget
        CHECK(DumpFits(0, 0, 6, DumpBudgetBytes(4096)));
    }

    // ---- carrier of the engine pass overrides' counters: engine.txt, one line per field
    {
        EngineStatus off;
        const std::string offText = EngineStatusText(off);
        CHECK_EQ(Count(offText, "\n"), size_t(3)); // the format line, `module: off` and the sun's own line
        CHECK(offText.rfind(EngineStatusHeader(), 0) == 0);
        CHECK(offText.find("module: off\n") != std::string::npos);
        CHECK(offText.find("sun: -\n") != std::string::npos); // the shafts projected no sun this session
        CHECK(offText.find("runs") == std::string::npos);

        EngineStatus ran;
        ran.keyOn = true;
        ran.armed = true;
        ran.runs = 3;
        ran.declined = 2;
        ran.declines.emplace_back("the draw is a character-preview replay", 2);
        ran.haveWindow = true;
        ran.windowRuns = 1;
        ran.windowDeclined = 3;
        ran.windowDeclines.emplace_back("the ambient buffer is neither 16,384 nor 12,352 bytes", 3);
        ran.haveRun = true;
        ran.boundBytes = 16384;
        ran.targetWidth = 3840;
        ran.targetHeight = 2160;
        ran.indexed = 33000;
        ran.indexRefused = 2;
        ran.window[0] = 0.5f;
        ran.window[1] = 0.5f;
        ran.window[2] = 3838.0f;
        ran.window[3] = 2159.0f;
        const std::string text = EngineStatusText(ran);
        CHECK(text.find("\nmodule: on\n") != std::string::npos);
        CHECK(text.find("\narmed: yes\n") != std::string::npos);
        CHECK(text.find("\nidentity: no\n") != std::string::npos);
        CHECK(text.find("\nruns: 3\n") != std::string::npos);
        CHECK(text.find("\ndeclined: 2\n") != std::string::npos);
        CHECK(text.find("\ndecline: the draw is a character-preview replay = 2\n") != std::string::npos);
        // The shader index's own line: how many bytecodes it stores and how many new ones its cap refused, so a capture taken late in a long
        // session answers whether a late shader (a water material, a composite of a new zone) was ever indexed.
        CHECK(text.find("\nindex: 33000 bytecodes, 2 refused\n") != std::string::npos);
        // The sun's line: the light shafts module's own projection of the frame's sun and whether it was in the window, so a reader knows
        // at once whether an F9 could have the engine's flare in frame. It is written whatever this card's key says, because the flare is
        // the game's and the shafts card is a key of its own.
        CHECK(text.find("\nsun: -\n") != std::string::npos);
        ran.haveSun = true;
        ran.sunX = 1342.0f;
        ran.sunY = 480.0f;
        CHECK(EngineStatusText(ran).find("\nsun: 1342x480, in the window\n") != std::string::npos);
        ran.sunOutside = 0.24f;
        ran.sunX = 1479.7f;
        ran.sunY = -259.7f;
        CHECK(EngineStatusText(ran).find("\nsun: 1479.7x-259.7, 0.24 windows outside\n") != std::string::npos);
        ran.sunBehind = true;
        CHECK(EngineStatusText(ran).find("\nsun: behind the camera\n") != std::string::npos);
        ran.sunBehind = false;
        ran.sunOutside = 0.0f;
        ran.sunX = 1342.0f;
        ran.sunY = 480.0f;
        CHECK(text.find("\nwindow runs: 1\n") != std::string::npos);
        CHECK(text.find("\nwindow declined: 3\n") != std::string::npos);
        CHECK(text.find("\nwindow decline: the ambient buffer is neither 16,384 nor 12,352 bytes = 3\n") != std::string::npos);
        CHECK(text.find("\nlast bound bytes: 16384\n") != std::string::npos);
        CHECK(text.find("\nlast target: 3840x2160\n") != std::string::npos);
        CHECK(text.find("\nlast window: 0.5,0.5,3838x2159\n") != std::string::npos);

        // A module that ran nothing still says so, and each field has its line so a reader does not have to count them.
        EngineStatus idle;
        idle.keyOn = true;
        idle.identity = true;
        const std::string idleText = EngineStatusText(idle);
        CHECK(idleText.find("\nidentity: yes\n") != std::string::npos);
        CHECK(idleText.find("\nruns: 0\n") != std::string::npos);
        CHECK(idleText.find("\nlast bound bytes: -\n") != std::string::npos);
        CHECK(idleText.find("\nlast target: -\n") != std::string::npos);
        CHECK(idleText.find("\nlast window: -\n") != std::string::npos);
        CHECK(idleText.find("decline:") == std::string::npos);
        // No baseline, no window lines: a reader then knows the file cannot answer for one capture's frames.
        CHECK(idleText.find("window runs:") == std::string::npos);
        CHECK(idleText.find("window declined:") == std::string::npos);

        // The window lines sit with the session lines and before the last-run facts, so a reader that stops at the first `last` line still
        // has both groups.
        const size_t session = text.find("\ndeclined: 2\n");
        const size_t window = text.find("\nwindow runs: 1\n");
        const size_t last = text.find("\nlast bound bytes: 16384\n");
        CHECK(session != std::string::npos && window != std::string::npos && last != std::string::npos);
        CHECK(session < window && window < last);

        // A window with no decline still has its two count lines, so absence of a reason line means zero, not an old file.
        EngineStatus quiet;
        quiet.keyOn = true;
        quiet.haveWindow = true;
        const std::string quietText = EngineStatusText(quiet);
        CHECK(quietText.find("\nwindow runs: 0\n") != std::string::npos);
        CHECK(quietText.find("\nwindow declined: 0\n") != std::string::npos);
        CHECK(quietText.find("window decline:") == std::string::npos);

        // The difference rule of the window counts: the session counters minus the baseline, and a counter that went backwards counts from
        // zero so a rebuilt module state never reports a wrapped number.
        CHECK_EQ(WindowCounter(0, 0), 0u);
        CHECK_EQ(WindowCounter(5, 2), 3u);
        CHECK_EQ(WindowCounter(7, 7), 0u);
        CHECK_EQ(WindowCounter(2, 5), 2u);
        CHECK_EQ(WindowCounter(1, 0), 1u);
    }

    // ---- carrier of the screen-space reflections' counters: the second block of the same engine.txt
    {
        SsrStatus off;
        const std::string offText = SsrStatusText(off);
        CHECK(offText == "ssr module: off\n"); // the card is off: that is the whole answer

        SsrStatus ran;
        ran.keyOn = true;
        ran.armed = true;
        ran.runs = 2;
        ran.readers = 18;
        ran.declined = 1;
        ran.declines.emplace_back("the draw writes no scene target this module can copy", 1);
        ran.passedOn.emplace_back("the bound shader carries no reflection stamp: its bytecode was never read", 4);
        ran.haveWindow = true;
        ran.windowRuns = 1;
        ran.windowReaders = 18;
        ran.windowDeclined = 1;
        ran.windowDeclines.emplace_back("the draw writes no scene target this module can copy", 1);
        ran.windowPassedOn.emplace_back("the bound shader carries no reflection stamp: its bytecode was never read", 4);
        ran.lastWaterDraw =
            "stamp 0x01030702, map slot 1, read slot 1, view TEXTURE2D, array 1, format R16G16B16A16_FLOAT, 3840x2160, texture 1e6b8cdeee0";
        const std::string text = SsrStatusText(ran);
        CHECK(text.rfind("ssr module: on\n", 0) == 0);
        CHECK(text.find("\nssr armed: yes\n") != std::string::npos);
        CHECK(text.find("\nssr runs: 2\n") != std::string::npos);
        CHECK(text.find("\nssr readers: 18\n") != std::string::npos);
        CHECK(text.find("\nssr declined: 1\n") != std::string::npos);
        CHECK(text.find("\nssr decline: the draw writes no scene target this module can copy = 1\n") != std::string::npos);
        CHECK(text.find("\nssr passed on: the bound shader carries no reflection stamp: its bytecode was never read = 4\n") !=
              std::string::npos);
        // The stamp and the view of the last water draw the gate passed on: the capture's answer to which slot the module's stamp named.
        CHECK(text.find("\nssr last water draw: stamp 0x01030702, map slot 1, read slot 1, view TEXTURE2D, array 1, format "
                        "R16G16B16A16_FLOAT, 3840x2160, texture 1e6b8cdeee0\n") != std::string::npos);
        CHECK(text.find("\nssr window runs: 1\n") != std::string::npos);
        CHECK(text.find("\nssr window readers: 18\n") != std::string::npos);
        CHECK(text.find("\nssr window declined: 1\n") != std::string::npos);
        CHECK(text.find("\nssr window decline: the draw writes no scene target this module can copy = 1\n") != std::string::npos);
        CHECK(text.find("\nssr window passed on: the bound shader carries no reflection stamp: its bytecode was never read = 4\n") !=
              std::string::npos);
        // The two blocks are appended, not merged: the engine block keeps its own lines and its header, and a reader of the file reads the
        // reflections' block after it.
        const std::string file = EngineStatusText(EngineStatus {}) + SsrStatusText(ran);
        CHECK(file.rfind(EngineStatusHeader(), 0) == 0);
        CHECK(file.find("module: off\n") != std::string::npos && file.find("ssr module: on\n") != std::string::npos);

        // A module that ran nothing still has its count lines, and none of the reason lines: absence of a reason line means zero.
        SsrStatus armedOnly;
        armedOnly.keyOn = true;
        armedOnly.armed = true;
        // A reason a caller carried with a count of zero is not written: absence of a line means zero, and a line with 0 would read as one.
        armedOnly.passedOn.emplace_back("the card is off, at blend zero, or the module stood down", 0);
        const std::string armedText = SsrStatusText(armedOnly);
        CHECK(armedText.find("\nssr armed: yes\n") != std::string::npos);
        CHECK(armedText.find("\nssr runs: 0\n") != std::string::npos);
        CHECK(armedText.find("\nssr readers: 0\n") != std::string::npos);
        CHECK(armedText.find("ssr last water draw: -\n") != std::string::npos); // nothing was passed on after reading a slot
        CHECK(armedText.find("ssr passed on:") == std::string::npos);
        CHECK(armedText.find("ssr window") == std::string::npos); // no baseline, so the file cannot answer for one capture's frames
    }

    // ---- the light pass the constant-buffer sample is taken at: the draw whose two R11G11B10 targets are the frame's light buffers, written
    //      without blending. The rows are the captured frame's own (draws.csv): the sun's light draw binds exactly those two targets of one size
    //      with entry 0's blend off, while the wetness volumes write one R8 target with blending on and every local light accumulates into the same
    //      pair with blending on - and all of them declare g_LightParam too, which is why a sample at whichever draw bound the name first read
    //      another light's space for a whole session.
    {
        CHECK_EQ(kLightBufferFormat, 26u); // DXGI_FORMAT_R11G11B10_FLOAT
        CHECK_EQ(kLightBufferTargets, 2u);
        CHECK(IsLightPassShape(2, kLightBufferFormat, true, false)); // the sun's own light draw
        // the wetness volume: one R8_UNORM target, blended, which the shape refuses on three counts
        CHECK(!IsLightPassShape(1, 28, true, true));
        // a local light: the same pair, blended into
        CHECK(!IsLightPassShape(2, kLightBufferFormat, true, true));
        // another format, another count, two targets of different sizes, an empty draw
        CHECK(!IsLightPassShape(2, 10, true, false));
        CHECK(!IsLightPassShape(3, kLightBufferFormat, true, false));
        CHECK(!IsLightPassShape(2, kLightBufferFormat, false, false));
        CHECK(!IsLightPassShape(0, 0, false, false));
        // one pair of another size is still the light pass: the rule is the shape, not a resolution
        CHECK(IsLightPassShape(2, kLightBufferFormat, true, false));
    }

    // ---- the caps: a live frame of this game is about 7,500 draws, so a capture needs room above one frame
    CHECK_EQ(kMaxRowsPerFrame, 12000u); // one live frame, with room for a heavier one
    CHECK(kMinDrawsPerFrame >= 1 && kMinDrawsPerFrame < kMaxRowsPerFrame);
    CHECK(kMaxFrames == 16);
    CHECK(kDefaultFrames == 3);
    CHECK_EQ(kDrawsBytes, uint64_t(32) << 20); // 32 MiB (a format-3 row is about 150 to 200 B)
    CHECK(kLightParamBytes == 0x30 && kLightParamDirectionAt == 0x10 && kLightParamPositionAt == 0x00);
    CHECK(kCameraParamViewAt == 0x00 && kCameraParamViewProjectionAt == 0x60 && kCameraParamMatrixBytes == 0x40); // the view and the view-projection are the camera buffer's first two matrices
    CHECK(kWaitDrawsPerPoll >= 1 && kSampleSeconds > 0.0);
    CHECK(kFormatVersion == 3); // format 3 adds the resource pointer column and views.txt

    return Finish("draw_diag_format");
}
