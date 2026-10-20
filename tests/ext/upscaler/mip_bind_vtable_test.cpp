// [MIP-BIND] The vtable slots the bind-time mip bias detours use are pinned against d3d11.h. The product reads them from
// the vtable of the game's device and context; here the slot of each method is decoded from the compiler's own virtual
// call thunk (`mov rax,[rcx]; jmp [rax+N*8]`) of a pointer to the method, so the numbers come from the header the SDK
// ships, not from this file. The constants in MipBindEngine.h must say the same.
#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

static int g_failed = 0;

#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

// The vtable slot behind a pointer to a virtual member function (x64 MSVC). -1 when the thunk is not the expected shape.
template <class Method> static int SlotOf(Method method)
{
    static_assert(sizeof(Method) == sizeof(void*), "single-inheritance member pointer expected");
    const unsigned char* code = nullptr;
    std::memcpy(&code, &method, sizeof(code));

    for (int hops = 0; hops < 4; ++hops) // an incremental-link thunk in front of the real one
    {
        if (code[0] == 0xE9)
        {
            std::int32_t rel = 0;
            std::memcpy(&rel, code + 1, sizeof(rel));
            code = code + 5 + rel;
            continue;
        }
        break;
    }

    if (!(code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01)) // mov rax, [rcx]
        return -1;
    if (code[3] == 0xFF && code[4] == 0x60) // jmp [rax+disp8]
        return code[5] / 8;
    if (code[3] == 0xFF && code[4] == 0xA0) // jmp [rax+disp32]
    {
        std::int32_t disp = 0;
        std::memcpy(&disp, code + 5, sizeof(disp));
        return disp / 8;
    }
    if (code[3] == 0xFF && code[4] == 0x20) // jmp [rax]: slot 0
        return 0;
    return -1;
}

int main(int argc, char** argv)
{
    CHECK(SlotOf(&ID3D11Device::CreateSamplerState) == 23);

    CHECK(SlotOf(&ID3D11DeviceContext::PSSetSamplers) == 10);
    CHECK(SlotOf(&ID3D11DeviceContext::VSSetSamplers) == 26);
    CHECK(SlotOf(&ID3D11DeviceContext::GSSetSamplers) == 32);
    CHECK(SlotOf(&ID3D11DeviceContext::HSSetSamplers) == 61);
    CHECK(SlotOf(&ID3D11DeviceContext::DSSetSamplers) == 65);
    CHECK(SlotOf(&ID3D11DeviceContext::CSSetSamplers) == 70);

    CHECK(SlotOf(&ID3D11DeviceContext::PSGetSamplers) == 75);
    CHECK(SlotOf(&ID3D11DeviceContext::VSGetSamplers) == 85);
    CHECK(SlotOf(&ID3D11DeviceContext::GSGetSamplers) == 88);
    CHECK(SlotOf(&ID3D11DeviceContext::HSGetSamplers) == 99);
    CHECK(SlotOf(&ID3D11DeviceContext::DSGetSamplers) == 103);
    CHECK(SlotOf(&ID3D11DeviceContext::CSGetSamplers) == 108);

    // A method known to be elsewhere decodes to its own slot, so the helper cannot pass by returning a constant.
    CHECK(SlotOf(&ID3D11DeviceContext::PSSetShader) == 9);

    // The header's constants say the same (stage order PS, VS, GS, HS, DS, CS).
    if (argc > 1)
    {
        std::ifstream in(std::string(argv[1]) + "/OptiScaler/ext/upscaler/MipBindEngine.h", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(!text.empty());
        CHECK(text.find("kDeviceCreateSamplerStateSlot = 23;") != std::string::npos);
        CHECK(text.find("kContextSetSamplersSlots = { 10, 26, 32, 61, 65, 70 };") != std::string::npos);
        CHECK(text.find("kContextGetSamplersSlots = { 75, 85, 88, 99, 103, 108 };") != std::string::npos);
    }
    else
    {
        std::printf("FAIL: repository root missing\n");
        ++g_failed;
    }

    if (g_failed == 0)
        std::printf("mip_bind_vtable: all passed\n");
    return g_failed == 0 ? 0 : 1;
}
