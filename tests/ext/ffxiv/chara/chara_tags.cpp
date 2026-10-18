#include "check.h"
#include "ext/ffxiv/chara/CharaTags.h"

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
} // namespace

int main()
{
    TestLists();
    TestRoleBits();
    TestPrivateData();
    TestShaderInfo();
    return TEST_RESULT("chara_tags");
}
