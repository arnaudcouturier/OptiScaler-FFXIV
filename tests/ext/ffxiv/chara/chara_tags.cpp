#include "check.h"
// FfxivHdrPreview.h is used for its real UI tag (it is not self-contained: the headers it relies on are listed here)
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include "ext/ffxiv/chara/CharaTags.h"
#include "ext/hdr/FfxivHdrPreview.h"

#include <cstring>
#include <set>
#include <vector>

using namespace ext::ffxiv::chara;

namespace
{
// The UI shader set of ext/hdr/FfxivHdrPreview.h (decimal there), restated in hex.
constexpr uint32_t kUiCrcs[] = { 0xE00FBAFDu, 0x6CA9BB69u, 0x712EB335u, 0xB0D428D9u, 0xE66DAE4Eu, 0xE95DEEC4u, 0xF2F6BDE8u };

// A pixel shader with just enough behaviour to carry private data.
struct FakeShader final : ID3D11PixelShader
{
    struct Item
    {
        GUID guid;
        std::vector<uint8_t> data;
    };
    std::vector<Item> items;
    ULONG refs = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device**) override {}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        for (const auto& i : items)
        {
            if (std::memcmp(&i.guid, &guid, sizeof(GUID)) != 0)
                continue;
            if (data == nullptr)
            {
                *size = static_cast<UINT>(i.data.size());
                return S_OK;
            }
            if (*size < i.data.size())
                return DXGI_ERROR_MORE_DATA;
            std::memcpy(data, i.data.data(), i.data.size());
            *size = static_cast<UINT>(i.data.size());
            return S_OK;
        }
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        for (auto& i : items)
        {
            if (std::memcmp(&i.guid, &guid, sizeof(GUID)) == 0)
            {
                i.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
                return S_OK;
            }
        }
        items.push_back({ guid, std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size) });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
};

void TestLists()
{
    CHECK(kGeometryCrcs.size() == 7);
    const uint32_t expected[] = { 0x68029DFCu, 0x05626DF1u, 0x478C1594u, 0x67AD01CAu, 0xA657BFE5u, 0xCC2C6A71u, 0xD2AEC0AAu };
    for (size_t i = 0; i < 7; ++i)
        CHECK(kGeometryCrcs[i] == expected[i]);
    CHECK(kFinisherCrc == 0xFDB2F4FCu);
    CHECK(kEquipDisplayCrc == 0x7B40E31Au);

    // no CRC is in two lists, nothing repeats inside the geometry list
    std::multiset<uint32_t> all;
    for (const auto c : kGeometryCrcs)
        all.insert(c);
    all.insert(kFinisherCrc);
    all.insert(kEquipDisplayCrc);
    for (const auto c : kUiCrcs)
        all.insert(c);
    for (const auto c : all)
        CHECK(all.count(c) == 1);
    CHECK(all.size() == 7 + 1 + 1 + 7);
}

void TestRoleBits()
{
    for (const auto c : kGeometryCrcs)
        CHECK(RoleOfCrc(c) == RoleGeometry);
    CHECK(RoleOfCrc(kFinisherCrc) == RoleFinisher);
    CHECK(RoleOfCrc(kEquipDisplayCrc) == RoleEquipDisplay);
    // the UI shaders are tagged by their own module; the CRC alone gives no chara role bit
    for (const auto c : kUiCrcs)
        CHECK(RoleOfCrc(c) == RoleNone);
    CHECK(RoleOfCrc(0) == RoleNone);
    CHECK(RoleOfCrc(0x12345678u) == RoleNone);

    // the four bits are distinct single bits
    const uint32_t bits[] = { RoleGeometry, RoleFinisher, RoleEquipDisplay, RoleUi };
    uint32_t seen = 0;
    for (const auto b : bits)
    {
        CHECK(b != 0 && (b & (b - 1)) == 0 && (seen & b) == 0);
        seen |= b;
    }
}

void TestPrivateData()
{
    FakeShader shader;
    CHECK(RoleOf(&shader) == RoleNone);       // untagged
    CHECK(RoleOf(nullptr) == RoleNone);
    CHECK(!SetRoles(&shader, RoleNone));      // nothing to write
    CHECK(shader.items.empty());
    CHECK(!SetRoles(nullptr, RoleFinisher));

    CHECK(SetRoles(&shader, RoleEquipDisplay));
    CHECK(RoleOf(&shader) == RoleEquipDisplay);
    CHECK(SetRoles(&shader, RoleGeometry | RoleUi)); // overwrites
    CHECK(RoleOf(&shader) == (RoleGeometry | RoleUi));
    CHECK(shader.items.size() == 1);

    // a value of the wrong size is not a role mask
    FakeShader odd;
    const uint8_t one = 1;
    odd.SetPrivateData(kRoleTag, 1, &one);
    CHECK(RoleOf(&odd) == RoleNone);

    // the tag GUID is not one of the GUIDs other modules use on shaders
    CHECK(kRoleTag.Data1 == 0x6f0c9a35);
}

// Every pixel shader carries its CRC and size, independent of the role bits.
void TestShaderInfo()
{
    FakeShader shader;
    CHECK(!GetShaderInfo(&shader).known);          // created before the hooks
    CHECK(!GetShaderInfo(nullptr).known);
    CHECK(!SetShaderInfo(nullptr, 1, 2));

    CHECK(SetShaderInfo(&shader, 0xFDB2F4FCu, 4321));
    const ShaderInfo info = GetShaderInfo(&shader);
    CHECK(info.known && info.crc == 0xFDB2F4FCu && info.size == 4321);

    // independent of the role mask, and the two do not disturb each other
    CHECK(SetRoles(&shader, RoleFinisher));
    CHECK(RoleOf(&shader) == RoleFinisher);
    CHECK(GetShaderInfo(&shader).crc == 0xFDB2F4FCu);
    CHECK(SetShaderInfo(&shader, 7, 8));
    CHECK(GetShaderInfo(&shader).crc == 7 && GetShaderInfo(&shader).size == 8 && RoleOf(&shader) == RoleFinisher);
    CHECK(shader.items.size() == 2);

    // a value of the wrong size is not shader info
    FakeShader odd;
    const uint8_t one = 1;
    odd.SetPrivateData(kShaderInfoTag, 1, &one);
    CHECK(!GetShaderInfo(&odd).known);
    CHECK(std::memcmp(&kShaderInfoTag, &kRoleTag, sizeof(GUID)) != 0);
}
// The creation hook: the UI module tags the shader first, then the preview layer reads that tag. The UI set is the real one
// (ext::hdr::FfxivHdrPreview::Tag), not a restated list, so this fails if the two modules drift apart or the order is swapped.
void TestCreationOrder()
{
    const GUID& uiTag = ext::hdr::FfxivHdrPreview::tag;

    // Every UI CRC the real module accepts, and only those: the restated list and the real one agree, and no chara CRC is a UI CRC.
    {
        std::set<uint32_t> accepted;
        std::vector<uint32_t> probes(std::begin(kUiCrcs), std::end(kUiCrcs));
        for (const auto c : kGeometryCrcs)
            probes.push_back(c);
        probes.push_back(kFinisherCrc);
        probes.push_back(kEquipDisplayCrc);
        probes.push_back(0);
        probes.push_back(0x12345678u);
        for (const uint32_t crc : probes)
        {
            FakeShader s;
            if (ext::hdr::FfxivHdrPreview::Tag(&s, crc))
                accepted.insert(crc);
        }
        CHECK(accepted == std::set<uint32_t>(std::begin(kUiCrcs), std::end(kUiCrcs)));
    }

    // UI shader, tagged by the UI module first: the preview layer adds RoleUi, records CRC and size
    for (const uint32_t crc : kUiCrcs)
    {
        FakeShader s;
        CHECK(ext::hdr::FfxivHdrPreview::Tag(&s, crc));
        const TagOutcome out = TagCreatedShader(&s, crc, 1234, uiTag);
        CHECK(out.written && out.roles == RoleUi);
        CHECK(RoleOf(&s) == RoleUi);
        CHECK(GetShaderInfo(&s).known && GetShaderInfo(&s).crc == crc && GetShaderInfo(&s).size == 1234);
    }

    // the wrong order: the preview layer looks first, so the UI shader is not recognised (this is the dependency the hook order serves)
    {
        FakeShader s;
        const TagOutcome out = TagCreatedShader(&s, kUiCrcs[0], 99, uiTag);
        CHECK(!out.written && out.roles == RoleNone && RoleOf(&s) == RoleNone);
        CHECK(GetShaderInfo(&s).known); // the info is recorded for every shader anyway
    }

    // a shader that is both a preview shader and carries the UI tag has both roles
    {
        FakeShader s;
        const uint32_t one = 1;
        s.SetPrivateData(uiTag, sizeof(one), &one);
        const TagOutcome out = TagCreatedShader(&s, kFinisherCrc, 4321, uiTag);
        CHECK(out.written && out.roles == (RoleFinisher | RoleUi));
        CHECK(RoleOf(&s) == (RoleFinisher | RoleUi));
    }

    // roles by CRC alone, a shader without any role writes nothing but the info, a zero UI tag is no tag, a null shader is safe
    for (const auto c : kGeometryCrcs)
    {
        FakeShader s;
        CHECK(TagCreatedShader(&s, c, 10, uiTag).roles == RoleGeometry && RoleOf(&s) == RoleGeometry);
    }
    {
        FakeShader s;
        CHECK(TagCreatedShader(&s, kEquipDisplayCrc, 10, uiTag).roles == RoleEquipDisplay);
        FakeShader plain;
        const TagOutcome out = TagCreatedShader(&plain, 0xDEADBEEFu, 77, uiTag);
        CHECK(!out.written && out.roles == RoleNone);
        CHECK(plain.items.size() == 1); // only the CRC and size item
        FakeShader zero;
        const uint32_t none = 0;
        zero.SetPrivateData(uiTag, sizeof(none), &none);
        CHECK(!TagCreatedShader(&zero, 0xDEADBEEFu, 1, uiTag).written);
        const uint8_t odd = 1;
        FakeShader wrongSize;
        wrongSize.SetPrivateData(uiTag, 1, &odd);
        CHECK(!TagCreatedShader(&wrongSize, 0xDEADBEEFu, 1, uiTag).written);
        CHECK(!TagCreatedShader(nullptr, kFinisherCrc, 1, uiTag).written && TagCreatedShader(nullptr, kFinisherCrc, 1, uiTag).roles == RoleNone);
    }
}
} // namespace

int main()
{
    TestLists();
    TestRoleBits();
    TestPrivateData();
    TestShaderInfo();
    TestCreationOrder();
    return TEST_RESULT("chara_tags");
}
