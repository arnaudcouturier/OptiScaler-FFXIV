// The per-stage image dump's pure rules (diag/StageDumpFormat.h): what a host test can drive without a device. The runtime that copies,
// maps and writes is driven by stage_dump_warp; the text pins of the stages' hook sites are stage_dump_pins.
#include "check.h"

#include "ext/ffxiv/diag/DrawDiagFormat.h"
#include "ext/ffxiv/diag/StageDump.h"
#include "ext/ffxiv/diag/StageDumpFormat.h"

#include <set>
#include <string>

using namespace ext::ffxiv::diag;

namespace
{
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}
} // namespace

int main()
{
    // ---- the registry: every stage once, the chain in the order its stages run ---------------------------------------------------------------
    {
        CHECK_EQ(kStageCount, size_t(13)); // 13: look-l2's atmosphere is stage-12 (its enumerator and table line are copied here byte for byte so the merge is clean), the night grade (L1b) is APPENDED as stage-13
        std::set<std::string> names;
        for (size_t i = 0; i < kStageCount; ++i)
        {
            const StageInfo& info = kStageTable[i];
            CHECK(info.name != nullptr && info.name[0] != '\0');
            CHECK(names.insert(info.name).second); // no two stages share a file stem
            CHECK_EQ(StageNumber(StageId(i)), uint32_t(i + 1));
        }
        // The chain order is the order the stages hand the picture on, which is NOT the order the brief listed them in: the light shafts
        // run first, the puddles' tone stage second, then the march, the light bounce and depth of field.
        CHECK(uint8_t(StageId::Shafts) < uint8_t(StageId::PuddleTone));
        CHECK(uint8_t(StageId::PuddleTone) < uint8_t(StageId::Godrays));
        CHECK(uint8_t(StageId::Godrays) < uint8_t(StageId::Gi));
        CHECK(uint8_t(StageId::Gi) < uint8_t(StageId::Dof));
        // Both late stages run between the light bounce and depth of field but are numbered LAST (appended), so the numbers the lab's bridge parses (09 gi, 10 dof, 11 flare)
        // never move. The atmosphere's enumerator is look-l2's (no stage behind it on this branch); the night grade is stage-13: at the merge L2's own
        // `Atmosphere + 1 == Count` check reads `NightGrade + 1 == Count` (this one).
        CHECK(uint8_t(StageId::Atmosphere) == uint8_t(StageId::Flare) + 1);
        CHECK(uint8_t(StageId::NightGrade) == uint8_t(StageId::Atmosphere) + 1 && uint8_t(StageId::NightGrade) + 1 == uint8_t(StageId::Count));
        CHECK_EQ(StageNumber(StageId::Dof), uint32_t(10));
        CHECK_EQ(StageNumber(StageId::Flare), uint32_t(11));
        CHECK_EQ(StageNumber(StageId::Atmosphere), uint32_t(12));
        CHECK_EQ(StageNumber(StageId::NightGrade), uint32_t(13));
        CHECK(std::string(StageInfoOf(StageId::Atmosphere).name) == "atmosphere" && std::string(StageInfoOf(StageId::NightGrade).name) == "night-grade");
        for (const StageId id : { StageId::ContactShadows, StageId::ShadowLift, StageId::Ao, StageId::Gloss, StageId::Ssr })
            CHECK(StageInfoOf(id).group == StageGroup::PreLight && StageInfoOf(id).images);
        for (const StageId id : { StageId::Shafts, StageId::PuddleTone, StageId::Godrays, StageId::Gi, StageId::Dof, StageId::Atmosphere, StageId::NightGrade })
            CHECK(StageInfoOf(id).group == StageGroup::Chain && StageInfoOf(id).images);
        CHECK(StageInfoOf(StageId::Flare).group == StageGroup::Chain && !StageInfoOf(StageId::Flare).images); // patches constants, no image
        CHECK_EQ(StageBitOf(StageId::Ao), kStagePreBit);
        CHECK_EQ(StageBitOf(StageId::Dof), kStageChainBit);
        CHECK(kStagePreBit != kStageChainBit && (kStagePreBit & kStageChainBit) == 0);
    }

    // ---- the file names: stage-<NN>-<name>-in|out ---------------------------------------------------------------------------------------------
    {
        CHECK(StageImageName(StageId::Shafts, StagePort::Out) == "stage-06-shafts-out");
        CHECK(StageImageName(StageId::Shafts, StagePort::In) == "stage-06-shafts-in");
        CHECK(StageImageName(StageId::ContactShadows, StagePort::In) == "stage-01-contact-shadows-in");
        CHECK(StageImageName(StageId::Dof, StagePort::Out) == "stage-10-dof-out");
        CHECK(StageImageName(StageId::Atmosphere, StagePort::In) == "stage-12-atmosphere-in");
        CHECK(StageImageName(StageId::Atmosphere, StagePort::Out) == "stage-12-atmosphere-out");
        CHECK(StageImageName(StageId::NightGrade, StagePort::In) == "stage-13-night-grade-in");
        CHECK(StageImageName(StageId::NightGrade, StagePort::Out) == "stage-13-night-grade-out");
        std::set<std::string> files;
        for (size_t i = 0; i < kStageCount; ++i)
            for (const StagePort port : { StagePort::In, StagePort::Out })
                CHECK(files.insert(StageImageName(StageId(i), port)).second);
        CHECK_EQ(files.size(), kStageCount * 2);
    }

    // ---- the dump rank: the stages' pictures take the tone map's own rank, above the atlas and everything below it --------------------------------
    {
        CHECK_EQ(uint8_t(DumpRank::Stage), uint8_t(DumpRank::Rest)); // no existing rank moves: the pinned numbers of the atlas.. fog post stay
        CHECK_EQ(uint8_t(DumpRank::Atlas), uint8_t(7));
        CHECK(uint8_t(DumpRank::Stage) < uint8_t(DumpRank::Atlas));
        // A contended bound gives the atlas, the sun mask, the water files and the material table away to a stage image, and never the engine's own
        // wetness / G-buffer / light / reflection / scene / view-position / tone-map targets nor the fog post's three files (the LUT and the sky with
        // the scene, the depth with the view positions: the replay lab cannot judge the god rays or the depth of field without them).
        for (const DumpRank lower : { DumpRank::Atlas, DumpRank::SunMask, DumpRank::WaterLights, DumpRank::WaterGate, DumpRank::ShaderTypes })
            CHECK(DumpYieldsTo(lower, DumpRank::Stage));
        for (const DumpRank higher : { DumpRank::Wetness, DumpRank::Gbuffer, DumpRank::Light, DumpRank::Ssr, DumpRank::Scene,
                                       DumpRank::ViewPosition, DumpRank::Rest, DumpRank::FogLut, DumpRank::FogSky, DumpRank::Depth })
            CHECK(!DumpYieldsTo(higher, DumpRank::Stage));
        CHECK(!DumpYieldsTo(DumpRank::Stage, DumpRank::Stage)); // a stage image keeps its first-come place against another stage image
        CHECK(!DumpYieldsTo(DumpRank::Stage, DumpRank::Rest));  // ... and against the tone map's own copy (the same rank: first come)
        CHECK(!DumpWaitsForSampling(DumpRank::Stage));          // complete when the stage returns: copied at once, never waiting for a sampler
        // The make-room rule releases the atlas for a stage image that does not fit beside it.
        DumpHold holds[3] { { DumpRank::Rest, 40, false }, { DumpRank::Atlas, 30, false }, { DumpRank::ShaderTypes, 5, false } };
        const uint64_t freed = DumpMakeRoom(holds, 3, DumpRank::Stage, 50, 75, 100);
        CHECK(freed == 35 && holds[2].released && holds[1].released && !holds[0].released);
        CHECK_EQ(kMaxDumpTextures, size_t(24)); // the dump's own twenty-four places are untouched: the stage images have places of their own
    }

    // ---- which draws belong to the capture frame ---------------------------------------------------------------------------------------------
    {
        StageOpenState s;
        CHECK_EQ(StageOpenMask(s), 0u); // nothing armed
        s.dumpActive = true;
        s.capturing = true;
        CHECK_EQ(StageOpenMask(s), 0u); // the splitter still waits: the capture started in a partial frame
        s.splitFresh = false;
        CHECK_EQ(StageOpenMask(s), kStagePreBit); // the capture frame's pre-light draws
        s.toneFrameSet = true;
        CHECK_EQ(StageOpenMask(s), kStageChainBit); // its tone map has been seen: the chain runs now, the pre-light stages are the NEXT frame's
        s.toneFrameOver = true;
        CHECK_EQ(StageOpenMask(s), 0u); // the frame is over (a later close, or the bridge Present): the next frame's chain is not this capture's
        s.toneFrameOver = false;
        s.dumpWritten = true;
        CHECK_EQ(StageOpenMask(s), 0u); // the dump was written
        s.dumpWritten = false;
        s.capturing = false;
        CHECK_EQ(StageOpenMask(s), 0u); // the capture ended
        s.capturing = true;
        s.dumpActive = false;
        CHECK_EQ(StageOpenMask(s), 0u); // DrawDiagnosticsDump is not "frame"
    }

    // ---- the crop: the stage's window, floored at the origin, clamped to the texture --------------------------------------------------------
    {
        StageBox box = StageCrop(0.5f, 0.5f, 1920.0f, 1080.0f, 1920, 1080);
        CHECK(box.valid && !box.whole && box.left == 0 && box.top == 0 && box.right == 1920 && box.bottom == 1080); // the half pixel floors to 0
        box = StageCrop(160.0f, 90.0f, 1280.0f, 720.0f, 1920, 1080);
        CHECK(box.valid && box.left == 160 && box.top == 90 && box.right == 1440 && box.bottom == 810); // the origin is used, not dropped
        CHECK_EQ(box.Width(), 1280u);
        CHECK_EQ(box.Height(), 720u);
        box = StageCrop(160.5f, 90.5f, 1280.0f, 720.0f, 1920, 1080);
        CHECK(box.left == 160 && box.top == 90 && box.right == 1440 && box.bottom == 810); // a half-pixel origin floors
        box = StageCrop(800.0f, 400.0f, 1920.0f, 1080.0f, 1920, 1080);
        CHECK(box.valid && box.right == 1920 && box.bottom == 1080 && box.left == 800); // clamped to the texture, not wrapped
        box = StageCrop(1920.0f, 0.0f, 100.0f, 100.0f, 1920, 1080);
        CHECK(!box.valid); // the window starts outside the texture
        box = StageCrop(0.0f, 0.0f, 0.0f, 0.0f, 1920, 1080);
        CHECK(box.valid && box.whole && box.right == 1920 && box.bottom == 1080); // a stage without a window takes the whole texture
        box = StageCrop(-8.0f, -8.0f, 100.0f, 100.0f, 1920, 1080);
        CHECK(box.valid && box.left == 0 && box.top == 0 && box.right == 100); // a negative origin is 0
        box = StageCrop(0.0f, 0.0f, 100.0f, 100.0f, 0, 1080);
        CHECK(!box.valid); // no texture
        box = StageCrop(0.0f, 0.0f, 1280.0f, 720.0f, 1280, 720);
        CHECK(box.valid && box.right == 1280 && box.bottom == 720); // a window as large as the texture is a plain full copy
    }

    // ---- the budget and the places ----------------------------------------------------------------------------------------------------------
    {
        CHECK(StageCountFits(0) && StageCountFits(kMaxStageImages - 1));
        CHECK(!StageCountFits(kMaxStageImages));
        // every image-making stage's in and out (the flare card makes none): 12 stages x 2 = 24 places, exactly full; the shared places are not raised
        size_t imageStages = 0;
        for (size_t i = 0; i < kStageCount; ++i)
            imageStages += kStageTable[i].images ? 1 : 0;
        CHECK_EQ(imageStages, size_t(12));
        CHECK(kMaxStageImages >= imageStages * 2);
        {
            // The reflections' A/B picture: the draw it was made at, the shader, the window, the box and the texture, in one line.
            const StageBox box = StageCrop(2.5f, 1.0f, 4.0f, 3.0f, 8, 6);
            const std::string note = AbImageNote("ssr-ab-engine", 4321, 0x60F6192Cu, "R16G16B16A16_FLOAT", 2.5f, 1.0f, 4.0f, 3.0f, box, 8, 6);
            CHECK(note == "# ssr-ab-image ssr-ab-engine frame_position=4321 ps_crc=0x60F6192C format=R16G16B16A16_FLOAT window=2.5,1,4,3 box=2,1,6,4 texture=8x6\n");
            const StageBox whole = StageCrop(0.0f, 0.0f, 0.0f, 0.0f, 8, 6);
            CHECK(AbImageNote("ssr-ab-before", 1, 0u, "R8_UNORM", 0.0f, 0.0f, 0.0f, 0.0f, whole, 8, 6).find(" window_unknown=1\n") != std::string::npos);
        }
        CHECK(DumpIdentFits(100, 50, 200) && !DumpIdentFits(100, 101, 200)); // the stage images ride the dump's held-bytes rule
        CHECK(DumpIdentFits(0, uint64_t(16588800), kMaxDumpBytes));           // one 1080p FP16 RGBA picture fits the default budget
    }

    // ---- the outcomes of a frame ---------------------------------------------------------------------------------------------------------------
    {
        CHECK(StageMerge(StageOutcome::None, StageOutcome::Declined) == StageOutcome::Declined);
        CHECK(StageMerge(StageOutcome::Declined, StageOutcome::Ran) == StageOutcome::Ran);
        CHECK(StageMerge(StageOutcome::Ran, StageOutcome::Declined) == StageOutcome::Ran); // a later "answered already" never overwrites "ran"
        CHECK(StageMerge(StageOutcome::Held, StageOutcome::Declined) == StageOutcome::Held);
        CHECK(StageMerge(StageOutcome::Held, StageOutcome::Ran) == StageOutcome::Ran);
        CHECK(StageMerge(StageOutcome::Combined, StageOutcome::Declined) == StageOutcome::Combined);
        CHECK(StageMerge(StageOutcome::Declined, StageOutcome::Combined) == StageOutcome::Combined);

        StageRecord record;
        StageNote(record, StageOutcome::Declined, "the gate said no", 40);
        StageNote(record, StageOutcome::Declined, "the size did not match", 90);
        CHECK(record.outcome == StageOutcome::Declined && record.declined == 2 && record.reason == "the size did not match" && record.ordinal == 40);
        StageNote(record, StageOutcome::Ran, "", 120);
        StageNote(record, StageOutcome::Declined, "already answered this frame", 130);
        CHECK(record.outcome == StageOutcome::Ran && record.ran == 1 && record.declined == 3 && record.reason.empty());
        CHECK_EQ(record.ordinal, 40u); // the frame position is the stage's first report
        StageNote(record, StageOutcome::None, "", 5);
        CHECK_EQ(record.ran + record.declined + record.held + record.combined, 4u); // None counts nothing
    }

    // ---- facts.txt's stage line: every stage gets one, with its own words ------------------------------------------------------------------------
    {
        StageRecord ran;
        StageNote(ran, StageOutcome::Ran, "", 5123);
        ran.images[0] = ran.images[1] = true;
        const std::string ranLine = StageStatusLine(StageId::Shafts, ran, true, "intensity=1.00 radius=0.50");
        CHECK(ranLine == "# stage 06 shafts: ran | frame position 5123 | images in+out | values intensity=1.00 radius=0.50\n");

        StageRecord declined;
        StageNote(declined, StageOutcome::Declined, "scene format not served", 5130);
        const std::string declinedLine = StageStatusLine(StageId::Godrays, declined, true, "");
        CHECK(declinedLine == "# stage 08 godrays: declined: scene format not served | frame position 5130 | images none\n");
        StageNote(declined, StageOutcome::Declined, "scene format not served", 5131);
        CHECK(Has(StageStatusLine(StageId::Godrays, declined, true, ""), "(2 times)"));

        StageRecord held;
        StageNote(held, StageOutcome::Held, "", 77);
        CHECK(Has(StageStatusLine(StageId::PuddleTone, held, true, ""), "held: the stage's last whole answer (an earlier frame's picture)"));

        StageRecord combined;
        StageNote(combined, StageOutcome::Combined, "lift 0.30, softness 0.10 ridden on the contact-shadow mask copy", 12);
        CHECK(Has(StageStatusLine(StageId::ShadowLift, combined, true, ""), "combined: lift 0.30, softness 0.10 ridden on the contact-shadow mask copy"));

        const StageRecord untouched;
        CHECK(Has(StageStatusLine(StageId::Gi, untouched, true, ""), "not reached this frame"));
        CHECK(Has(StageStatusLine(StageId::Gi, untouched, false, ""), "not armed"));
        CHECK(!Has(StageStatusLine(StageId::Gi, untouched, false, ""), "frame position")); // nothing was reached: no position
        CHECK(Has(StageStatusLine(StageId::Dof, untouched, true, "strength=0.5"), "values strength=0.5"));
        CHECK(Has(StageStatusLine(StageId::Flare, untouched, true, ""), "none (this stage makes no image)"));

        // A '#' line is a comment to every parser of facts.txt; no stage line may begin like a skipped file ("# skipped") by accident.
        for (size_t i = 0; i < kStageCount; ++i)
        {
            const std::string line = StageStatusLine(StageId(i), untouched, true, "");
            CHECK(line[0] == '#' && line.back() == '\n' && Count(line, "\n") == 1);
            CHECK(!Has(line, "# skipped"));
        }
    }

    // ---- the image comments ------------------------------------------------------------------------------------------------------------------
    {
        const StageBox box = StageCrop(160.0f, 90.0f, 1280.0f, 720.0f, 1920, 1080);
        const std::string note =
            StageImageNote("stage-08-godrays-out", StageId::Godrays, StagePort::Out, "R16G16B16A16_FLOAT", 160, 90, 1280, 720, box, 1920, 1080);
        CHECK(note == "# stage-image stage-08-godrays-out stage=godrays port=out view_format=R16G16B16A16_FLOAT window=160,90,1280,720 "
                      "box=160,90,1440,810 texture=1920x1080\n");
        CHECK(!Has(note, "format 3")); // the parsers read "format <N>" from comment lines as the dump's version
        const StageBox whole = StageCrop(0, 0, 0, 0, 640, 360);
        CHECK(Has(StageImageNote("stage-01-contact-shadows-in", StageId::ContactShadows, StagePort::In, "R8_UNORM", 0, 0, 0, 0, whole, 640, 360),
                  "window_unknown=1"));
        CHECK(StageReferenceNote("stage-09-gi-in", "stage-08-godrays-out") == "# stage-image stage-09-gi-in same_as=stage-08-godrays-out\n");
        CHECK(StageSkipNote("stage-09-gi-out", "over the dump's bound") == "# skipped stage-09-gi-out.bin: over the dump's bound\n");
        CHECK(Has(StageFactsLegend(), "stage-<NN>-<name>-in.bin") && Has(StageFactsLegend(), "not reached this frame"));
    }


    // ---- the capture frame's measured numbers: the late text, the white point and fade, the sun, the tint, the depth note -----------------------------------
    {
        StageFrameValues white;
        white.haveWhite = true;
        white.white = 0.838f;
        white.fade = 1.0f;
        CHECK(FormatFrameValues(white) == "white=0.838 fade=1.00");
        StageFrameValues shafts = white;
        shafts.haveSun = true;
        shafts.sunX = 1023.9f;
        shafts.sunY = 217.0f;
        shafts.haveTint = true;
        shafts.tint[0] = 1.96f;
        shafts.tint[1] = 1.49f;
        shafts.tint[2] = 1.25f;
        CHECK(FormatFrameValues(shafts) == "white=0.838 fade=1.00 sunPx=1023.9,217.0 tint=1.96,1.49,1.25");
        StageFrameValues held = shafts;
        held.fade = 0.5f;
        CHECK(Has(FormatFrameValues(held), "fade=0.50"));
        StageFrameValues sunOnly;
        sunOnly.haveSun = true;
        sunOnly.sunX = 12.34f;
        sunOnly.sunY = -5.0f;
        CHECK(FormatFrameValues(sunOnly) == "sunPx=12.3,-5.0"); // a missing part is left out, never invented
        CHECK(FormatFrameValues(StageFrameValues {}).empty());

        // The line: the snapshot of the keys, then the late numbers, one space, nothing doubled.
        CHECK(StageValuesWithLate("focus=auto strength=0.80", "focusM=8.43 engineDof=0") == "focus=auto strength=0.80 focusM=8.43 engineDof=0");
        CHECK(StageValuesWithLate("focus=auto", "") == "focus=auto");
        CHECK(StageValuesWithLate("", "white=0.838 fade=1.00") == "white=0.838 fade=1.00");
        CHECK(StageValuesWithLate("", "").empty());
        StageRecord ran;
        ran.outcome = StageOutcome::Ran;
        ran.ran = 1;
        ran.ordinal = 5123;
        const std::string line = StageStatusLine(StageId::Dof, ran, true, StageValuesWithLate("mode=distance focus=auto", "focusM=8.43 start=20.00"));
        CHECK(Has(line, "| values mode=distance focus=auto focusM=8.43 start=20.00\n"));
        CHECK(Count(line, "\n") == 1 && line[0] == '#'); // still one comment line, the format unchanged

        // The depth file's comment: the same attributes the stage images carry, so the lab's reader takes both with one pattern.
        const StageBox box = StageCrop(0.5f, 0.5f, 1920.0f, 1080.0f, 3840, 2160);
        CHECK(box.valid && box.left == 0 && box.top == 0 && box.right == 1920 && box.bottom == 1080);
        const std::string note = DepthImageNote("depth", "R24_UNORM_X8_TYPELESS", 0.5f, 0.5f, 1920.0f, 1080.0f, box, 3840, 2160);
        CHECK(note == "# depth-image depth view_format=R24_UNORM_X8_TYPELESS window=0.5,0.5,1920,1080 box=0,0,1920,1080 texture=3840x2160\n");
        CHECK(!Has(note, "format 3"));
        const StageBox unknown = StageCrop(0, 0, 0, 0, 640, 360);
        CHECK(Has(DepthImageNote("depth", "R24_UNORM_X8_TYPELESS", 0, 0, 0, 0, unknown, 640, 360), "window_unknown=1"));
    }

    return Finish("stage_dump_rules");
}
