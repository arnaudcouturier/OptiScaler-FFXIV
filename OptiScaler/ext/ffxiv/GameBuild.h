#pragma once

// FFXIV executable identity, RVAs, structure offsets, and instruction guards.
// Keep all game-build-dependent values in this file so a game patch can be rebased in one place.

#include <array>
#include <cstddef>
#include <cstdint>

namespace ext::ffxiv
{
struct NativeQualityBuild
{
    std::uint32_t settingsInstructionsRva = 0x3745d7;
    std::array<std::uint8_t, 37> settingsInstructions {
        0x4c, 0x8b, 0x05, 0x92, 0x9e, 0x59, 0x02, 0x45, 0x33, 0xe4, 0x41, 0x80, 0x78, 0x54, 0x02, 0x74, 0x06, 0x45,
        0x38, 0x60, 0x44, 0x74, 0x71, 0x45, 0x38, 0x60, 0x45, 0x74, 0x6b, 0x41, 0x0f, 0xb6, 0x40, 0x55, 0x83, 0xf8,
        0x06
    };

    std::uint32_t querySizeRva = 0x352350;
    std::array<std::uint8_t, 28> querySizeInstructions {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56, 0x48,
        0x83, 0xec, 0x20, 0x48, 0x8b, 0x05, 0x2c, 0xc1, 0x5b, 0x02
    };

    std::uint32_t rendererUpdateRva = 0x2da5b0;
    std::array<std::uint8_t, 18> rendererUpdateInstructions {
        0x4c, 0x8b, 0xdc, 0x57, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xc6, 0x16, 0x61, 0x02
    };

    std::uint32_t resizeCallbacksRva = 0x2369a0;
    std::array<std::uint8_t, 20> resizeCallbacksInstructions {
        0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83,
        0xec, 0x20
    };

    std::uint32_t settingsPointerRva = 0x290e470;
    std::uint32_t managerPointerRva = 0x290e498;
    std::uint32_t devicePointerRva = 0x2907d00;

    std::uint32_t settingsModeOffset = 0x54;
    std::uint32_t settingsQualityOffset = 0x44;
    std::uint32_t settingsDlaaOffset = 0x45;
    std::uint32_t settingsRendererOffset = 0x47;
    std::uint32_t settingsAdditionalOffset = 0x49;
    std::uint32_t settingsSelectorOffset = 0x55; // logged alongside the DLAA gate, never gated on
    std::uint32_t managerContextOffset = 0x4220;
    std::uint32_t contextFlagsOffset = 0x181;
    std::uint32_t contextCacheOffset = 0x150;
    std::uint32_t deviceCallbacksOffset = 0x30;
    std::uint32_t rendererSceneSizeOffset = 0x428;
};

struct CompanionBuild
{
    std::uint32_t batch16Rva = 0x6e56d0;
    std::uint32_t batch32Rva = 0x6e6270;
    std::array<std::uint8_t, 16> batchInstructions {
        0x88, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55
    };
    std::uint32_t enqueueRva = 0x240b80;
    std::array<std::uint8_t, 16> enqueueInstructions {
        0x48, 0x63, 0x41, 0x0c, 0x4c, 0x8b, 0xda, 0x4c, 0x8d, 0x04, 0x40, 0x8b, 0x41, 0x08, 0x42, 0x83
    };
    std::uint32_t bindStateRva = 0x22d750;
    std::array<std::uint8_t, 16> bindStateInstructions {
        0x40, 0x56, 0x41, 0x56, 0x48, 0x83, 0xec, 0x48, 0x80, 0x7a, 0x49, 0x00, 0x4c, 0x8b, 0xf2, 0x48
    };

    // The module global is supported by reference-chain evidence, not an instruction guard (medium confidence).
    std::uint32_t nameplateModulePointerRva = 0x2ab8650;
    std::uint32_t layoutSlotTableRva = 0x217c760;

    std::uint32_t rendererBatchListOffset = 0x580;
    std::uint32_t rendererBatchCountOffset = 0x588;
    std::uint32_t rendererIndexBufferOffset = 0x1e0;
    std::uint32_t rendererVertexAllocationOffset = 0x570;
    std::uint32_t rendererVertexStrideOffset = 0x500;
    std::uint32_t rendererVertexOffsetOffset = 0x504;
    std::uint32_t rendererQueuesOffset = 0x590;
    std::uint32_t rendererQueueCount = 16;
    std::uint32_t rendererQueueStride = 0x100;
    std::uint32_t moduleStatePointerOffset = 0x08;
    std::uint32_t stateQueuePointerOffset = 0x08;
    std::uint32_t resourceWrapperOffset = 0x68;
    std::uint32_t nativeContextOffset = 0x17b8;
    std::uint32_t drawStateFlagOffset = 0x49;
    std::uint32_t drawStateCommandBackPointerOffset = 0x20;

    std::uint32_t indexBufferCapacityOffset = 0x38;
    std::uint32_t indexBufferFlagsOffset = 0x40;
    std::uint32_t indexBufferCpuStartOffset = 0x60;
    std::uint32_t indexBufferCpuEndOffset = 0x68;
    std::uint32_t queueHeadOffset = 0x50;
    std::uint32_t queueRemainingOffset = 0x68;
    std::uint32_t queueBlocksOffset = 0x6c;
    std::uint32_t queueStridesOffset = 0x34;
    std::uint32_t queueNextBlockOffset = 0x3f0;
    std::uint32_t batchKindOffset = 0x00;
    std::uint32_t batchWrapperOffset = 0x10;
    std::uint32_t batchCountOffset = 0x20;
    std::uint32_t batchVerticesOffset = 0x28;
    std::uint32_t batchFormatOffset = 0x30;
    std::uint32_t batchExtensionOffset = 0x04;
    std::uint32_t entryPointerOffset = 0x08;
    std::uint32_t baseVertexOffset = 0x08;
    std::uint32_t startIndexOffset = 0x14;
    std::uint32_t indexCountOffset = 0x18;
    std::uint32_t drawCommandType = 6;
    std::uint32_t drawCommandTypeOffset = 0x00;
    std::uint32_t drawCommandSize = 0xb0;
    std::uint32_t nativeBatchEntrySize = 16;
    std::uint32_t nativeBatchHeaderSize = 64;
    std::uint32_t rendererVertexSlotStride = 0x10;
    std::uint32_t layoutSlotEntrySize = 8;
};

// The character preview renderer's render-target table (ext/ffxiv/chara). The table is reached through a tiny accessor
// function of the executable; the code pattern below finds it without an RVA, and the RVAs are the values of the
// current build, kept as documentation and as a cross-check of the scan (docs/RECONSTRUCTION-REFERENCE.md, CharaView).
//
// Accessor: mov rax,[rip+disp32] ; mov ecx,edx ; mov rax,[rax+rcx*8+0x2c8] ; ret
// The four displacement bytes are not compared (mask 0). Exactly one hit in .text is required.
struct CharaViewBuild
{
    std::array<std::uint8_t, 18> pattern {
        0x48, 0x8b, 0x05, 0x00, 0x00, 0x00, 0x00, 0x8b, 0xca, 0x48, 0x8b, 0x84, 0xc8, 0xc8, 0x02, 0x00, 0x00, 0xc3
    };
    std::array<std::uint8_t, 18> patternMask { 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    std::uint32_t ripInstructionLength = 7; // the load is 7 bytes: global = match + 7 + disp32

    std::uint32_t accessorRva = 0x26abd0;        // the accessor function (the pattern's single hit)
    std::uint32_t managerPointerRva = 0x290e490; // global holding the preview renderer's manager object

    std::uint32_t managerBytes = 0x400;       // bytes of the manager that are read
    std::uint32_t targetsOffset = 0x2c8;      // entries 0..12, 8 bytes each
    std::uint32_t transparentOffset = 0x378;  // entries 13..17
    std::uint32_t depthOffset0 = 0x360;       // entry 18
    std::uint32_t depthOffset1 = 0x368;       // entry 19
    std::uint32_t entryCount = 20;
    std::uint32_t entryBytes = 0x70;          // bytes of each entry object that are read
    std::uint32_t entryResourceOffset = 0x68; // native texture pointer
    std::uint32_t entryWidthOffset = 0x40;    // u32
    std::uint32_t entryHeightOffset = 0x44;   // u32
    std::uint32_t maxExtent = 0x4000;         // entries with a width or height of 0 or above this are ignored
    std::uint32_t maxTextBytes = 128u << 20;  // the .text scan is refused above this size
};

// The game's camera jitter object (ext/ffxiv/jitter, docs/RECONSTRUCTION-REFERENCE.md): the scene camera owns a 20-byte object
// { float jx, jy; float ndcX, ndcY; int counter } that the game's jitter generator fills (pixel jitter jx, jy in [-0.5, 0.5],
// the NDC translation the projection matrix gets, and a frame counter). Reached through three pointers:
//   G = [exe base + cameraGlobalRva] ; S = [G + cameraOffset] ; P = [S + jitterPointerOffset] ; object = P.
// Read only, by ext/ffxiv/jitter (the probe and the optional correction). Study: STUDY-jitter.md section 2.2 (RVAs 0x374050, 0x25f610,
// 0x2ebd90 of the supported build). If a game patch moves them, docs/FFXIV-PATCH-RUNBOOK.md says how to find them again.
struct JitterBuild
{
    std::uint32_t cameraGlobalRva = 0x290dba0;
    std::uint32_t cameraOffset = 0x3028;
    std::uint32_t jitterPointerOffset = 0x278;
    std::uint32_t jitterXOffset = 0x00;
    std::uint32_t jitterYOffset = 0x04;
    std::uint32_t ndcXOffset = 0x08;
    std::uint32_t ndcYOffset = 0x0c;
    std::uint32_t counterOffset = 0x10;
    std::uint32_t objectBytes = 0x14;
};

// The source-level jitter fix (ext/ffxiv/jitter, FfxivJitterSource*.h/.cpp): where the game's jitter generator is called and what it feeds.
// Every block below was read from the supported executable (ffxiv_dx11.exe, the October 20 build, SHA-256
// 46A52AB1DBE80B9A8D6C4D4531BF66693904200C5135FACACD053EF28A5DE14A) and is compared with the live bytes before either hook is installed
// (FfxivJitterSourceAnchors.h); tests/ext/ffxiv/jitter/jitter_source_anchors.cpp pins the table to the recorded bytes and checks that the
// addresses and offsets in it are the ones the bytes encode.
//
// The camera update (RVA 0x25f690) draws with   ndc = 2 * strength * jitter / size   where
//   strength = (float[settings+0x78] - 0.001f) * (ratio * ratio) + 0.001f,   ratio = (u16[renderer+0x700] / u16[renderer+0x704]) * float[settings+0x4c]
// and calls the generator (RVA 0x2ebd90: index % count + 1, outputs x - 0.5, y - 0.5) with the jitter object's two floats as outputs, the frame
// counter, and in DLSS mode (settings byte +0x54 == 2) an unbounded phase count (mov edx, -1). The NGX packet producer (RVA 0x374050 region)
// reads the same object through the camera getter (RVA 0x25f610), call site 0x37411d, return address 0x374122.
struct JitterSourceBuild
{
    std::uint32_t generatorRva = 0x2ebd90;
    std::array<std::uint8_t, 16> generatorInstructions {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x83, 0xec, 0x70
    };

    std::uint32_t cameraGetterRva = 0x25f610;
    std::array<std::uint8_t, 8> cameraGetterInstructions { 0x48, 0x8b, 0x81, 0x78, 0x02, 0x00, 0x00, 0xc3 };

    std::uint32_t producerCallRva = 0x37411d; // call cameraGetter ; the NGX packet producer
    std::uint32_t producerReturnRva = 0x374122;
    std::array<std::uint8_t, 13> producerCallInstructions {
        0xe8, 0xee, 0xb4, 0xee, 0xff, 0x0f, 0x57, 0xc0, 0x41, 0xbd, 0xc4, 0x03, 0x02
    };

    std::uint32_t cameraHeadRva = 0x25f690; // prologue, settings global, mode byte
    std::array<std::uint8_t, 78> cameraHeadInstructions {
        0x40, 0x55, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x68, 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec, 0x98, 0x02, 0x00, 0x00,
        0x48, 0x8b, 0x05, 0xdf, 0xc5, 0x68, 0x02, 0x48, 0x33, 0xc4, 0x48, 0x89, 0x85, 0xd0, 0x00, 0x00, 0x00, 0x48,
        0x8b, 0x01, 0x48, 0x8b, 0xf9, 0x48, 0x89, 0x9c, 0x24, 0xb8, 0x02, 0x00, 0x00, 0xff, 0x50, 0x38, 0x48, 0x8b,
        0x05, 0xa5, 0xed, 0x6a, 0x02, 0x32, 0xd2, 0x44, 0x0f, 0xb6, 0x40, 0x54, 0x41, 0x80, 0xf8, 0x02, 0x74, 0x06,
        0x83, 0x78, 0x2c, 0x02, 0x75, 0x0f
    };

    // renderer global, jitter object, phase count, scene size, DLSS branch, device output width, generator arguments. 120 bytes: 0x25f721 up to the
    // strength block at 0x25f799 with no gap, so the last 8 bytes (lea rcx,[rsp+0x30] at 0x25f791, mov rdx,rbx at 0x25f796 = the generator's x output
    // is the jitter object P, the pointer the world tie compares) are guarded too.
    std::uint32_t phasesRva = 0x25f721;
    std::array<std::uint8_t, 120> phasesInstructions {
        0x48, 0x8b, 0x05, 0x68, 0xed, 0x6a, 0x02, 0x0f, 0x57, 0xf6, 0x48, 0x8b, 0x9f, 0x78, 0x02, 0x00, 0x00, 0xba,
        0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x88, 0x28, 0x04, 0x00, 0x00, 0x48, 0x8b, 0xf1, 0x8b, 0xc1, 0x48, 0xc1,
        0xee, 0x20, 0xf3, 0x48, 0x0f, 0x2a, 0xf0, 0x41, 0x80, 0xf8, 0x02, 0x74, 0x33, 0x48, 0x8b, 0x05, 0xa7, 0x85,
        0x6a, 0x02, 0x0f, 0x57, 0xc0, 0xf3, 0x0f, 0x10, 0x0d, 0x3c, 0xe7, 0xeb, 0x01, 0x8b, 0x88, 0x8c, 0x00, 0x00,
        0x00, 0xf3, 0x48, 0x0f, 0x2a, 0xc1, 0xf3, 0x0f, 0x5e, 0xc6, 0xe8, 0x88, 0x65, 0xb8, 0x01, 0xf3, 0x0f, 0x59,
        0x05, 0xbc, 0xe6, 0xee, 0x01, 0xf3, 0x48, 0x0f, 0x2c, 0xd0, 0x44, 0x8b, 0x4b, 0x10, 0x4c, 0x8d, 0x43, 0x04,
        0x89, 0x54, 0x24, 0x20, 0x48, 0x8d, 0x4c, 0x24, 0x30, 0x48, 0x8b, 0xd3
    };

    std::uint32_t strengthRva = 0x25f799; // call generator, extents, settings amount and scale, strength, doubling, NDC division
    std::uint32_t generatorCallReturnRva = 0x25f79e;
    std::array<std::uint8_t, 157> strengthInstructions {
        0xe8, 0xf2, 0xc5, 0x08, 0x00, 0x48, 0x8b, 0x0d, 0xeb, 0xec, 0x6a, 0x02, 0x4c, 0x8d, 0x45, 0xd0, 0x48, 0x8b,
        0x03, 0x48, 0x8d, 0x57, 0x50, 0x48, 0x89, 0x44, 0x24, 0x38, 0x0f, 0xb7, 0x81, 0x00, 0x07, 0x00, 0x00, 0x66,
        0x0f, 0x6e, 0xc8, 0x0f, 0xb7, 0x81, 0x04, 0x07, 0x00, 0x00, 0x48, 0x8b, 0x8f, 0x78, 0x02, 0x00, 0x00, 0x0f,
        0x5b, 0xc9, 0x66, 0x0f, 0x6e, 0xc0, 0x48, 0x8b, 0x05, 0x94, 0xec, 0x6a, 0x02, 0x0f, 0x5b, 0xc0, 0xf3, 0x0f,
        0x10, 0x50, 0x78, 0xf3, 0x0f, 0x5c, 0x15, 0x78, 0xe5, 0xeb, 0x01, 0xf3, 0x0f, 0x5e, 0xc8, 0x0f, 0x57, 0xc0,
        0xf3, 0x0f, 0x59, 0x48, 0x4c, 0xf3, 0x48, 0x0f, 0x2a, 0xc6, 0xf3, 0x0f, 0x59, 0xc9, 0xf3, 0x0f, 0x59, 0xd1,
        0xf3, 0x0f, 0x58, 0x15, 0x57, 0xe5, 0xeb, 0x01, 0xf3, 0x0f, 0x58, 0xd2, 0x0f, 0x28, 0xca, 0xf3, 0x0f, 0x59,
        0x54, 0x24, 0x3c, 0xf3, 0x0f, 0x59, 0x4c, 0x24, 0x38, 0x0f, 0x57, 0x15, 0x69, 0xe8, 0xeb, 0x01, 0xf3, 0x0f,
        0x5e, 0xd0, 0x0f, 0x28, 0x05, 0xae, 0xe7, 0xeb, 0x01, 0xf3, 0x0f, 0x5e, 0xce
    };

    std::uint32_t deviceSizeRva = 0xd3f73; // another reader of the device global: output height (+0x90) and width (+0x8c) as a pair
    std::array<std::uint8_t, 20> deviceSizeInstructions {
        0x48, 0x8b, 0x0d, 0x86, 0x3d, 0x83, 0x02, 0x44, 0x8b, 0x81, 0x90, 0x00, 0x00, 0x00, 0x8b, 0x91, 0x8c, 0x00, 0x00, 0x00
    };

    // The globals the blocks above load (verified through their rip-relative displacement bytes), and the offsets read from them.
    std::uint32_t settingsPointerRva = 0x290e470;
    std::uint32_t rendererPointerRva = 0x290e490;
    std::uint32_t devicePointerRva = 0x2907d00;
    std::uint32_t settingsScaleOffset = 0x4c;   // float
    std::uint32_t settingsModeOffset = 0x54;    // byte, 2 = DLSS mode
    std::uint32_t settingsAmountOffset = 0x78;  // float
    std::uint32_t rendererSceneSizeOffset = 0x428; // u32 width, u32 height of the scene (render) size
    std::uint32_t rendererExtentOffset = 0x700;    // u16 extent0 at +0, u16 extent2 at +4
    std::uint32_t deviceOutputSizeOffset = 0x8c;   // u32 output width, u32 output height

    // The two constants the source-fix core assumes (FfxivJitterSourceCore.h reads them from here at run time). Constants, never resolved: the
    // seams jitterSource.strengthFloor and jitterSource.ndcSignMask are the live check (ValueEquals of exactly these bits), and the gate
    // (ext/ffxiv/resolve/Gate.h) refuses the whole fix on any build where one of them is not Usable, i.e. where the executable's value
    // differs from what the core was written against.
    std::uint32_t strengthFloorBits = 0x3a83126f; // float 0.001f, the constant the strength expression subtracts and adds
    std::uint32_t ndcSignMaskBits = 0x80000000;   // the sign mask `xorps` applies to the NDC y (the generator output's y is negated)
};

struct GameBuild
{
    std::uint32_t timeDateStamp = 0x6ad10bfd;
    std::uint32_t sizeOfImage = 0x381f000;
    NativeQualityBuild nativeQuality {};
    CompanionBuild companion {};
    CharaViewBuild charaView {};
    JitterBuild jitter {};
    JitterSourceBuild jitterSource {};
};

inline constexpr GameBuild kCurrentBuild {};

const GameBuild* MatchBuild(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage);

// Walks the CURRENT process's own module headers (GetModuleHandleW(nullptr)) and checks them
// against MatchBuild: the "is this the matched FFXIV build" in-process identity gate. Shared by
// every site that needs this before ext::game::Current() may have run --
// ext::ffxiv::quality::FfxivNativeQualityHooks::MatchedBuildOnce and
// ext::ffxiv::companion::Native::Install both used to re-walk the DOS/NT headers themselves and
// now call this instead. ext::game::SelectGame (ext/game/Game.cpp) has its own equivalent walk;
// it is pre-existing, already working, and intentionally left alone here -- a future cleanup
// could have it call this too for consistency.
const GameBuild* MatchCurrentProcess();
} // namespace ext::ffxiv
