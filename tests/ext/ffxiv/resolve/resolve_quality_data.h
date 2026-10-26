#pragma once
// Data of the quality-group tests: the recorded bytes of build C at every site the quality anchors read (copied from the executable by a
// script, equal to ffxiv_dx11.exe (build C, analysis/patch-20261016) at those RVAs; the executable itself is never committed) and the pinned pattern
// texts. The pinned texts are independent copies of the patterns in AnchorsQuality.h: a change there must be made here too, on purpose.
#include <cstdint>

namespace qt
{
struct Site
{
    const char* name;
    std::uint32_t rva;
    const char* hex;
};

inline constexpr Site kSites[] = {
    { "chara.accessor", 0x26abd0,
        "488b05b9386a028bca488b84c8c8020000c3" },
    { "context.prelude+settingsBlock", 0x374590,
        "405741564883ec38f68181010000014c8bf2488bf9751be86404000084c07512488bcfe8a802000032c04883c438415e5fc348896c245849"
        "8bd6488bcf4c89642468e8390900004c8b05929e59024533e44180785402740645386044747145386045746b410fb6405583f806" },
    // the second flags anchor (0x37479c, inside contextResolver's chained range 0x37473d..0x3747ac): movzx eax,[rdi+FLAGS] ; mov r15,[rsp+0x30] ;
    // test al,0x30 ; jne. Recorded from the executable, 16 bytes.
    { "contextFlagsLoad", 0x37479c,
        "0fb687810100004c8b7c2430a830755d" },
    { "cacheCompare", 0x374f10,
        "48894c2408564881ec900000008b8150010000488bf2488bd13906750f8b81540100003946040f845b030000" },
    { "querySize", 0x352350,
        "48895c2408488974241048897c241841564883ec20488b052cc15b02498bf9498bf04c8bf2488b98204200004885db7441488b05e8c05b02"
        "807854027534488bd14533c0488bcbe8f421020084c07422488b83d4000000498906488b83dc000000488906488b83e4000000488907b001" },
    { "rendererUpdate.entry", 0x2da5b0,
        "4c8bdc574881ecd0000000488b05c61661024833c44889442460f30f1081100700000f57d2f30f59052338e7010f28d949895b10488bd949"
        "896b180f57ed4d8973e84c8b3507d76202" },
    { "rendererUpdate.caller", 0x2b9e69,
        "488b0d20466502410f28cae837070200488b0d78d07e024885c97405e806850800" },
    { "resizeBlock", 0x2dab0d,
        "498b4e30e88abef5ff488b055339630280785402740680784400740f8078450074090fb78302070000eb070fb78304070000668983000700"
        "00" },
    { "sizeBlock", 0x2dab9a,
        "488b0d5fd162028b818c000000398328040000750e8b819000000039832c0400007435" },
    { "resizeEntry", 0x2369a0,
        "48895c241048896c24184889742420574883ec20488d5908488bf1488bcbff15846ced018b463840b501488b7e3085c074354c897424308b"
        "f04533f60f1f4000488b074885c07410488b4f08ffd084c0400fb6ed410f44ee4883c7104883ee0175de4c8b742430488bcbff15406ced01"
        "488b5c2438400fb6c5488b6c2440488b7424484883c4205fc3" },
    { "managerCtor", 0x3537c8,
        "48898720420000488d0d7aebffff488b05b3ac5b0248898830070000488bcfe844800000488bcfe8dc810000" },
    { "cameraAccessor", 0x795b90,
        "488b0509801702488b8028300000c3" },
    { "cameraProducer", 0x3740c9,
        "488b05d09a5902488b3db9a35902488b1d223c5902488b8828300000488b8668010000498906488b867801000048898600010000488b8750"
        "06000048898608010000488b4770488986100100004c89be18010000e8eeb4eeff" },
    { "cameraGetter", 0x25f610,
        "488b8178020000c3" },
};

// ---- pinned pattern texts ----
inline constexpr const char* kPinSettingsBlock =
    "4C 8B 05 ?? ?? ?? ?? 45 33 E4 41 80 78 ?? 02 74 06 45 38 60 ?? 74 71 45 "
    "38 60 ?? 74 6B 41 0F B6 40 ?? 83 F8 06";
inline constexpr const char* kPinQuerySizeEntry =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8B 05 "
    "?? ?? ?? ?? 49 8B F9 49 8B F0 4C 8B F2 48 8B 98 ?? ?? ?? ?? 48 85 DB 74 "
    "41 48 8B 05 ?? ?? ?? ?? 80 78 ?? 02 75 34 48 8B D1 45 33 C0 48 8B CB E8 "
    "?? ?? ?? ?? 84 C0 74 22 48 8B 83 D4 00 00 00 49 89 06 48 8B 83 DC 00 00 "
    "00 48 89 06 48 8B 83 E4 00 00 00 48 89 07 B0 01";
inline constexpr const char* kPinQuerySizeBody =
    "49 8B F9 49 8B F0 4C 8B F2 48 8B 98 ?? ?? ?? ?? 48 85 DB 74 41 48 8B 05 "
    "?? ?? ?? ?? 80 78 ?? 02 75 34 48 8B D1 45 33 C0 48 8B CB E8 ?? ?? ?? ?? "
    "84 C0 74 22 48 8B 83 D4 00 00 00 49 89 06 48 8B 83 DC 00 00 00 48 89 06 "
    "48 8B 83 E4 00 00 00 48 89 07 B0 01";
inline constexpr const char* kPinRendererUpdateEntry =
    "4C 8B DC 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 "
    "24 60 F3 0F 10 81 10 07 00 00 0F 57 D2 F3 0F 59 05 ?? ?? ?? ?? 0F 28 D9 "
    "49 89 5B 10 48 8B D9 49 89 6B 18 0F 57 ED 4D 89 73 E8 4C 8B 35 ?? ?? ?? "
    "??";
inline constexpr const char* kPinRendererUpdateBody =
    "F3 0F 10 81 10 07 00 00 0F 57 D2 F3 0F 59 05 ?? ?? ?? ?? 0F 28 D9 49 89 "
    "5B 10 48 8B D9 49 89 6B 18 0F 57 ED 4D 89 73 E8 4C 8B 35 ?? ?? ?? ??";
inline constexpr const char* kPinRendererUpdateCaller =
    "48 8B 0D ?? ?? ?? ?? 41 0F 28 CA E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 "
    "85 C9 74 05 E8 ?? ?? ?? ??";
inline constexpr const char* kPinResizeBlock =
    "49 8B 4E ?? E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 80 78 ?? 02 74 06 80 78 "
    "?? 00 74 0F 80 78 ?? 00 74 09 0F B7 83 02 07 00 00 EB 07 0F B7 83 04 07 "
    "00 00 66 89 83 00 07 00 00";
inline constexpr const char* kPinSizeBlock =
    "48 8B 0D ?? ?? ?? ?? 8B 81 8C 00 00 00 39 83 ?? ?? ?? ?? 75 0E 8B 81 90 "
    "00 00 00 39 83 2C 04 00 00 74 35";
inline constexpr const char* kPinResizeEntry =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D 59 08 "
    "48 8B F1 48 8B CB FF 15 ?? ?? ?? ?? 8B 46 38 40 B5 01 48 8B 7E 30 85 C0 "
    "74 35 4C 89 74 24 30 8B F0 45 33 F6 0F 1F 40 00 48 8B 07 48 85 C0 74 10 "
    "48 8B 4F 08 FF D0 84 C0 40 0F B6 ED 41 0F 44 EE 48 83 C7 10 48 83 EE 01 "
    "75 DE 4C 8B 74 24 30 48 8B CB FF 15 ?? ?? ?? ?? 48 8B 5C 24 38 40 0F B6 "
    "C5 48 8B 6C 24 40 48 8B 74 24 48 48 83 C4 20 5F C3";
inline constexpr const char* kPinManagerCtor =
    "48 89 87 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 89 88 "
    "30 07 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ??";
inline constexpr const char* kPinCallContext =
    "49 8B D6 48 8B CF 4C 89 64 24 68 E8 ?? ?? ?? ??";
inline constexpr const char* kPinCacheCompare =
    "48 89 4C 24 08 56 48 81 EC 90 00 00 00 8B 81 ?? ?? ?? ?? 48 8B F2 48 8B "
    "D1 39 06 75 0F 8B 81 54 01 00 00 39 46 04 0F 84 ?? ?? ?? ??";
inline constexpr const char* kPinCameraAccessor =
    "48 8B 05 ?? ?? ?? ?? 48 8B 80 28 30 00 00 C3";
inline constexpr const char* kPinCameraProducer =
    "48 8B 05 ?? ?? ?? ?? 48 8B 3D ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 8B 88 "
    "?? ?? ?? ?? 48 8B 86 68 01 00 00 49 89 06 48 8B 86 78 01 00 00 48 89 86 "
    "00 01 00 00 48 8B 87 50 06 00 00 48 89 86 08 01 00 00 48 8B 47 70 48 89 "
    "86 10 01 00 00 4C 89 BE 18 01 00 00 E8 ?? ?? ?? ??";
inline constexpr const char* kPinCameraGetter =
    "48 8B 81 ?? ?? ?? ?? C3";
inline constexpr const char* kPinCharaTargets =
    "48 8B 05 ?? ?? ?? ?? 8B CA 48 8B 84 C8 ?? ?? ?? ?? C3";
// guard windows (verified at a fixed offset from the seam; they stop short of the prologue)
inline constexpr const char* kPinQueryManagerWindow = "48 8B 05 ?? ?? ?? ?? 49 8B F9 49 8B F0 4C 8B F2 48 8B 98 ?? ?? ?? ?? 48 85 DB";
inline constexpr const char* kPinQuerySettingsWindow = "48 8B 05 ?? ?? ?? ?? 80 78 ?? 02 75 34";
inline constexpr const char* kPinQueryCallWindow = "48 8B D1 45 33 C0 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 22";
inline constexpr const char* kPinContextFlagsWindow = "F6 81 ?? ?? ?? ?? 01 4C 8B F2 48 8B F9 75 ??";
inline constexpr const char* kPinContextFlagsLoad = "0F B6 87 ?? ?? ?? ?? 4C 8B 7C 24 30 A8 30 75 ??";
inline constexpr const char* kPinCacheWindow = "8B 81 50 01 00 00 48 8B F2 48 8B D1 39 06 75 0F 8B 81 54 01 00 00 39 46 04";
} // namespace qt
