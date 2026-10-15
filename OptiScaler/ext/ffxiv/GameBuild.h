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
    std::uint32_t settingsInstructionsRva = 0x374347;
    std::array<std::uint8_t, 37> settingsInstructions {
        0x4c, 0x8b, 0x05, 0x22, 0x51, 0x58, 0x02, 0x45, 0x33, 0xe4, 0x41, 0x80, 0x78, 0x54, 0x02, 0x74, 0x06, 0x45,
        0x38, 0x60, 0x44, 0x74, 0x71, 0x45, 0x38, 0x60, 0x45, 0x74, 0x6b, 0x41, 0x0f, 0xb6, 0x40, 0x55, 0x83, 0xf8,
        0x06
    };

    std::uint32_t querySizeRva = 0x3520c0;
    std::array<std::uint8_t, 28> querySizeInstructions {
        0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x41, 0x56, 0x48,
        0x83, 0xec, 0x20, 0x48, 0x8b, 0x05, 0xbc, 0x73, 0x5a, 0x02
    };

    std::uint32_t rendererUpdateRva = 0x2da290;
    std::array<std::uint8_t, 18> rendererUpdateInstructions {
        0x4c, 0x8b, 0xdc, 0x57, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xf6, 0xc9, 0x5f, 0x02
    };

    std::uint32_t resizeCallbacksRva = 0x236a40;
    std::array<std::uint8_t, 20> resizeCallbacksInstructions {
        0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83,
        0xec, 0x20
    };

    std::uint32_t settingsPointerRva = 0x28f9470;
    std::uint32_t managerPointerRva = 0x28f9498;
    std::uint32_t devicePointerRva = 0x28f2d00;

    std::uint32_t settingsModeOffset = 0x54;
    std::uint32_t settingsQualityOffset = 0x44;
    std::uint32_t settingsDlaaOffset = 0x45;
    std::uint32_t settingsRendererOffset = 0x47;
    std::uint32_t settingsAdditionalOffset = 0x49;
    std::uint32_t managerContextOffset = 0x4220;
    std::uint32_t contextFlagsOffset = 0x181;
    std::uint32_t contextCacheOffset = 0x150;
    std::uint32_t deviceCallbacksOffset = 0x30;
    std::uint32_t rendererSceneSizeOffset = 0x428;
};

struct CompanionBuild
{
    std::uint32_t batch16Rva = 0x6e5570;
    std::uint32_t batch32Rva = 0x6e6110;
    std::array<std::uint8_t, 16> batchInstructions {
        0x88, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55
    };
    std::uint32_t enqueueRva = 0x240bc0;
    std::array<std::uint8_t, 16> enqueueInstructions {
        0x48, 0x63, 0x41, 0x0c, 0x4c, 0x8b, 0xda, 0x4c, 0x8d, 0x04, 0x40, 0x8b, 0x41, 0x08, 0x42, 0x83
    };
    std::uint32_t bindStateRva = 0x22d7f0;
    std::array<std::uint8_t, 16> bindStateInstructions {
        0x40, 0x56, 0x41, 0x56, 0x48, 0x83, 0xec, 0x48, 0x80, 0x7a, 0x49, 0x00, 0x4c, 0x8b, 0xf2, 0x48
    };

    // The module global is supported by reference-chain evidence, not an instruction guard (medium confidence).
    std::uint32_t nameplateModulePointerRva = 0x2aa35c0;
    std::uint32_t layoutSlotTableRva = 0x217bc00;

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

struct GameBuild
{
    std::uint32_t timeDateStamp = 0x6ac3dc96;
    std::uint32_t sizeOfImage = 0x380a000;
    NativeQualityBuild nativeQuality {};
    CompanionBuild companion {};
};

inline constexpr GameBuild kCurrentBuild {};

const GameBuild* MatchBuild(std::uint32_t timeDateStamp, std::uint32_t sizeOfImage);
} // namespace ext::ffxiv
