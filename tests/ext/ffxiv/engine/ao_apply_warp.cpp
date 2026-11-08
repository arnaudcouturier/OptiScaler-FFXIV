// Ambient occlusion: the two things this module does to a game object - the stamp it writes on a shader and the one slot swap a reader draw gets
// (ext/ffxiv/engine/AoSwap.cpp) - on a real D3D11 device.
//
// The stamp is the whole of what the module remembers about a shader, so the failure that matters is the one a bounded table produced: an entry
// lost because the game created more shaders afterwards. This test stamps one shader with the slots its reflection named, creates five thousand
// more pixel shaders, and reads the first one's stamp back; a shader created and never stamped reads the empty word. The product's cost per
// reader draw is the swap, and the two things that would hurt in a session are that the game's own view does not come back and that a reference
// leaks or is released twice while hundreds of draws a frame run through it. So the test also binds a texture at a slot, swaps it the way the
// draw path does, and then checks the slot and the object's reference count after the guard goes out of scope - and again after an exception
// unwinds through the guard, which is what a throwing game draw does.
//
// AoSwap.cpp is compiled on its own here: it needs nothing but D3D11 and the two small types it serves, so the stamp and the swap with its
// restore are pinned without the module's config, log and pass around them.
//
// The slots a shader is stamped with are read from a TEXTURE binding, never from a sampler's: the last block parses the game's own bytecode for a
// capture's reader, whose samplers and textures carry the same names at different registers, and reads the stamp back.
#include "pch.h" // the test build's stand-in: Windows, D3D11 and the fork's logging macros

#include "check.h"

#include "ext/ffxiv/engine/AoApply.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace ext::ffxiv::engine::ao;

namespace
{
constexpr uint32_t kSlot = 1;
constexpr uint32_t kSize = 32;
constexpr uint32_t kLaterShaders = 5000; // more shaders than the game creates of one family in a session

// The game's own bytecode for one captured reader (the cascade draw that resolves the sun's screen-space mask): the effect compiler writes a
// texture and its sampler under one name, every sampler before every texture, so g_SamplerDepth.S is at s0 before g_SamplerDepth.T at t1 and
// g_SamplerGBuffer.S at s3 before g_SamplerGBuffer.T at t4. The blob is a test fixture, not a shader of the game this module serves.
const char* kReaderFixture = "/tests/ext/ffxiv/engine/fixtures/reader_0x1257B24B.dxbc";

// The smallest pixel shader the effect compiler will take: the object is all the stamp needs, and five thousand of them are what a session's
// material shaders look like to it.
constexpr char kShaderSource[] = "float4 main() : SV_Target0 { return 0.0; }";

ComPtr<ID3D11ShaderResourceView> MakeView(ID3D11Device* device, ID3D11Texture2D** texture)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = kSize;
    desc.Height = kSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> created;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &created)))
        return nullptr;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateShaderResourceView(created.Get(), nullptr, &view)))
        return nullptr;
    if (texture != nullptr)
        *texture = created.Detach();
    return view;
}

// What the slot holds right now, and the caller owns the reference.
ID3D11ShaderResourceView* BoundAt(ID3D11DeviceContext* context, uint32_t slot)
{
    ID3D11ShaderResourceView* view = nullptr;
    context->PSGetShaderResources(slot, 1, &view);
    return view;
}

// The object's reference count, asked the only way a COM object allows: one more reference, then two releases.
ULONG RefCount(IUnknown* object)
{
    object->AddRef();
    const ULONG count = object->Release();
    return count - 1;
}

void Bind(ID3D11DeviceContext* context, uint32_t slot, ID3D11ShaderResourceView* view)
{
    context->PSSetShaderResources(slot, 1, &view);
}
} // namespace

int main(int argc, char** argv)
{
    // The repository root comes from the test recipe ('{repo}'), so the captured fixture is found whatever folder the test was built in.
    const std::string repo = argc > 1 ? argv[1] : ".";

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)) ||
        device == nullptr || context == nullptr)
    {
        std::printf("ao_apply_warp: no D3D11 WARP device\n");
        return 1;
    }

    ComPtr<ID3D11ShaderResourceView> game;
    ComPtr<ID3D11ShaderResourceView> ours;
    {
        ID3D11Texture2D* unused = nullptr;
        game = MakeView(device.Get(), &unused);
        if (unused != nullptr)
            unused->Release();
        ours = MakeView(device.Get(), nullptr);
    }
    if (!game || !ours)
    {
        std::printf("ao_apply_warp: the test's textures could not be created\n");
        return 1;
    }

    // ---- the slots a compiled reader declares: the TEXTURE's registers, not the samplers'
    //
    // This is the failure the module shipped with: every sampler is reflected before every texture and under the same name, so a scan that took
    // the first name it saw stamped s5 for the normal and s0 for the depth - and the pass then read whatever texture those slots held (G-buffer 3
    // for the normal), a buffer with no unit normal, which turned the whole term into 1.0.
    {
        std::ifstream file(repo + kReaderFixture, std::ios::binary);
        const std::string code { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        CHECK(!code.empty());

        ext::ffxiv::diag::ShaderBinding parsed[ext::ffxiv::diag::kMaxBindings];
        const size_t count = ext::ffxiv::diag::ParseBindings(code.data(), code.size(), parsed, ext::ffxiv::diag::kMaxBindings);
        CHECK(count > 0);

        const Inputs read = InputsOfBindings(parsed, count);
        CHECK_EQ(uint32_t(read.depth), 1u);  // g_SamplerDepth.T; its sampler is at s0
        CHECK_EQ(uint32_t(read.normal), 4u); // g_SamplerGBuffer.T; its sampler is at s3
        CHECK_EQ(uint32_t(read.camera), 1u);
        CHECK(read.Complete());

        // The stamp a reader draw reads back is that word, on a shader object made from the same bytecode.
        ComPtr<ID3D11PixelShader> reader;
        CHECK(SUCCEEDED(device->CreatePixelShader(code.data(), code.size(), nullptr, &reader)));
        const uint32_t word = PackInputs(read);
        CHECK(word != 0);
        CHECK(detail::StampShader(reader.Get(), word));
        const Inputs back = UnpackInputs(detail::ReadStamp(reader.Get()));
        CHECK_EQ(uint32_t(back.depth), 1u);
        CHECK_EQ(uint32_t(back.normal), 4u); // g_SamplerGBuffer.T at t4
        CHECK_EQ(uint32_t(back.camera), 1u);
        CHECK(back.Complete());
    }

    // ---- the stamp: the slots a shader declares live on that shader, and five thousand later shaders leave them alone
    //
    // This is the behaviour a bounded table of shaders could not give: the game creates every material shader of a session while it loads, each
    // of them declares the camera table, and the reader's entry was overwritten before the frame's first reader asked for it. The stamp is on the
    // object, so the only way to lose it is to lose the object.
    {
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(D3DCompile(kShaderSource, sizeof kShaderSource - 1, "ao_stamp", nullptr, nullptr, "main", "ps_5_0", flags, 0, &blob,
                              &errors)) ||
            blob == nullptr)
        {
            std::printf("ao_apply_warp: the test's pixel shader did not compile\n");
            return 1;
        }

        auto makeShader = [&](ComPtr<ID3D11PixelShader>& shader) {
            return SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader));
        };

        // The reader's own shader, stamped at creation with the slots its reflection declared (the session's shape: depth 0, normal 7, camera 13).
        ComPtr<ID3D11PixelShader> reader;
        CHECK(makeShader(reader));
        const uint32_t packed = PackInputs(Inputs { 0, 7, 13 });
        CHECK(packed != 0);
        CHECK(detail::StampShader(reader.Get(), packed));
        CHECK_EQ(detail::ReadStamp(reader.Get()), packed);
        CHECK(UnpackInputs(detail::ReadStamp(reader.Get())).Complete());

        // Five thousand more objects, each stamped with something else: a session's material shaders. Nothing here can reach the reader's word.
        {
            std::vector<ComPtr<ID3D11PixelShader>> later(kLaterShaders);
            for (uint32_t i = 0; i < kLaterShaders; ++i)
            {
                CHECK(makeShader(later[i]));
                CHECK(detail::StampShader(later[i].Get(), PackInputs(Inputs { uint8_t(i % 16), uint8_t((i / 16) % 16), uint8_t((i / 256) % 14) })));
            }
            CHECK_EQ(detail::ReadStamp(reader.Get()), packed);
            const Inputs back = UnpackInputs(detail::ReadStamp(reader.Get()));
            CHECK_EQ(uint32_t(back.depth), 0u);
            CHECK_EQ(uint32_t(back.normal), 7u);
            CHECK_EQ(uint32_t(back.camera), 13u);
            CHECK_EQ(detail::ReadStamp(later[kLaterShaders - 1].Get()),
                     PackInputs(Inputs { uint8_t((kLaterShaders - 1) % 16), uint8_t(((kLaterShaders - 1) / 16) % 16),
                                         uint8_t(((kLaterShaders - 1) / 256) % 14) }));
        }
        CHECK_EQ(detail::ReadStamp(reader.Get()), packed); // and after every one of them is gone

        // A shader this module never read answers with the empty word, which the draw path refuses instead of guessing slots. So does an object
        // that is not a shader this module stamped at all, and a null one.
        ComPtr<ID3D11PixelShader> untouched;
        CHECK(makeShader(untouched));
        CHECK_EQ(detail::ReadStamp(untouched.Get()), 0u);
        CHECK(!UnpackInputs(detail::ReadStamp(untouched.Get())).HasDepth());
        CHECK_EQ(detail::ReadStamp(nullptr), 0u);

        // Nothing is written for a word of 0: an empty stamp is an unstamped shader and not a third state.
        CHECK(!detail::StampShader(untouched.Get(), 0));
        CHECK_EQ(detail::ReadStamp(untouched.Get()), 0u);
        CHECK(!detail::StampShader(nullptr, packed));
    }

    // ---- the swap and the restore, with the game's own view back afterwards and no reference left behind
    {
        Bind(context.Get(), kSlot, game.Get()); // what the game's draw bound
        const ULONG before = RefCount(game.Get());

        ID3D11ShaderResourceView* replaced = detail::SwapSlot(context.Get(), kSlot, ours.Get());
        CHECK(replaced == game.Get()); // the game's own view, handed over with the reference the guard restores it with
        ID3D11ShaderResourceView* now = BoundAt(context.Get(), kSlot);
        CHECK(now == ours.Get()); // the draw would sample this module's texture
        if (now != nullptr)
            now->Release();
        CHECK_EQ(RefCount(game.Get()), before + 1u); // the reference the swap took is held, for the guard

        {
            BoundReader guard(context.Get(), kSlot, replaced);
            CHECK(guard.Bound());
            ID3D11ShaderResourceView* during = BoundAt(context.Get(), kSlot);
            CHECK(during == ours.Get()); // the guard binds nothing of its own: the swap is still in place
            if (during != nullptr)
                during->Release();
        }
        // The guard put the game's own view back and gave the reference back with it.
        ID3D11ShaderResourceView* after = BoundAt(context.Get(), kSlot);
        CHECK(after == game.Get());
        if (after != nullptr)
            after->Release();
        CHECK_EQ(RefCount(game.Get()), before);
    }

    // ---- an exception through the guard restores too, which is what a game draw that throws does
    {
        Bind(context.Get(), kSlot, game.Get());
        const ULONG before = RefCount(game.Get());
        bool threw = false;
        try
        {
            ID3D11ShaderResourceView* replaced = detail::SwapSlot(context.Get(), kSlot, ours.Get());
            BoundReader guard(context.Get(), kSlot, replaced);
            throw 7;
        }
        catch (int)
        {
            threw = true;
        }
        CHECK(threw);
        ID3D11ShaderResourceView* after = BoundAt(context.Get(), kSlot);
        CHECK(after == game.Get());
        if (after != nullptr)
            after->Release();
        CHECK_EQ(RefCount(game.Get()), before);
    }

    // ---- nothing bound under that name: no swap, and a guard with nothing to restore reports so
    {
        Bind(context.Get(), kSlot, nullptr);
        ID3D11ShaderResourceView* replaced = detail::SwapSlot(context.Get(), kSlot, ours.Get());
        CHECK(replaced == nullptr);
        ID3D11ShaderResourceView* now = BoundAt(context.Get(), kSlot);
        CHECK(now == nullptr); // the slot was left alone
        if (now != nullptr)
            now->Release();
        const BoundReader guard(context.Get(), kSlot, replaced);
        CHECK(!guard.Bound());

        // A null replacement is the same answer: there is nothing to swap in.
        Bind(context.Get(), kSlot, game.Get());
        CHECK(detail::SwapSlot(context.Get(), kSlot, nullptr) == nullptr);
        ID3D11ShaderResourceView* kept = BoundAt(context.Get(), kSlot);
        CHECK(kept == game.Get());
        if (kept != nullptr)
            kept->Release();
        Bind(context.Get(), kSlot, nullptr);
    }

    return Finish("ao_apply_warp");
}
