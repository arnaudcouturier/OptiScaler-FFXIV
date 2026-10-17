// S10-E3: the creation-time stage stamp of the boundary core, without a GPU.
//
// Bug (Session 2): with [HudLess] Mode off at launch the CreateShader hook never stamped any shader (the stamp was behind Armed()), so
// after Mode was switched on in the menu every draw hit an unstamped shader: the scene stages AND the UI stage were unknown and the
// boundary never fired. Core::ShaderCreated is the shipped creation decision (the hook calls it with Armed() as `registry`); these checks
// pin it down with a fake pixel shader object whose private data store is a fixed array (so the allocation count below is ours only):
//   * every catalog program (5 scene stages + 7 UI programs) is stamped with `registry = false` (not armed), and Core::ReadStamp (the
//     function the draw-time lookup uses) reads the stage and the ui flag back;
//   * a program outside the catalog gets no SetPrivateData at all;
//   * the epoch advances on every creation (pointer cache invalidation: a new object may reuse the address of a freed one);
//   * the creation path performs ZERO heap allocations, takes no registry and leaves the D1 counters and rows untouched;
//   * with `registry = true` (armed) the D1 registry runs on top and its per-program record replaces the stage record.
#define NOMINMAX
#include "check.h"

#include "ext/ffxiv/ui/UiBoundaryCore.h"

#include <cstdlib>
#include <cstring>
#include <new>

using namespace ext::ffxiv::ui;

// ---- allocation counting (ours only: the fake object below never allocates) ------------------------------------------------------
namespace
{
bool g_count = false;
long g_allocs = 0;
} // namespace

void* operator new(size_t n)
{
    if (g_count)
        ++g_allocs;
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace
{
// A pixel shader object with a private data store in a fixed array (no heap).
struct FakePs final : ID3D11PixelShader
{
    struct Entry
    {
        GUID guid {};
        UINT size = 0;
        unsigned char bytes[16] {};
        bool used = false;
    };
    Entry entries[4] {};
    int setCalls = 0;
    UINT lastSetSize = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** device) override { *device = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        for (const Entry& e : entries)
            if (e.used && IsEqualGUID(e.guid, guid))
            {
                const UINT have = *size;
                *size = e.size;
                if (data == nullptr)
                    return S_OK;
                if (have < e.size)
                    return DXGI_ERROR_MORE_DATA;
                std::memcpy(data, e.bytes, e.size);
                return S_OK;
            }
        *size = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        ++setCalls;
        lastSetSize = size;
        if (size > sizeof(Entry::bytes))
            return E_OUTOFMEMORY;
        for (Entry& e : entries)
            if (e.used && IsEqualGUID(e.guid, guid))
            {
                e.size = size;
                std::memcpy(e.bytes, data, size);
                return S_OK;
            }
        for (Entry& e : entries)
            if (!e.used)
            {
                e.used = true;
                e.guid = guid;
                e.size = size;
                std::memcpy(e.bytes, data, size);
                return S_OK;
            }
        return E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
};

constexpr uint32_t kSceneCrcs[5] { 0x85e777efu, 0xf8f57f0au, 0x27ebc404u, 0x1f264d17u, 0xf6e81a1bu }; // stages 1..5

void Run()
{
    auto* core = new Core; // never freed: the shaders below hold pointers into its stage records

    // ---- the whole catalog is stamped while NOT armed (registry = false) ------------------------------------------------------------
    for (uint8_t s = 0; s < 5; ++s)
    {
        FakePs ps;
        core->ShaderCreated(&ps, kSceneCrcs[s], 1500, false);
        CHECK_EQ(ps.setCalls, 1);
        CHECK_EQ(ps.lastSetSize, sizeof(ShaderRec*)); // 8 bytes: Lookup treats any other size as "no stamp"
        const ShaderRec* rec = Core::ReadStamp(&ps);  // what the draw-time lookup reads
        CHECK(rec != nullptr);
        if (rec)
        {
            CHECK_EQ(rec->stage, s + 1);
            CHECK(!rec->ui);
            CHECK(!rec->preHook);
        }
    }
    for (uint32_t crc : kUiCrcs)
    {
        FakePs ps;
        core->ShaderCreated(&ps, crc, 2048, false);
        CHECK_EQ(ps.setCalls, 1);
        const ShaderRec* rec = Core::ReadStamp(&ps);
        CHECK(rec != nullptr);
        if (rec)
        {
            CHECK_EQ(rec->stage, kUiStage);
            CHECK(rec->ui);
        }
    }
    // The records are shared per stage (one per stage, none per shader) and are not in the D1 registry.
    {
        FakePs a, b;
        core->ShaderCreated(&a, kUiCrcs[0], 2048, false);
        core->ShaderCreated(&b, kUiCrcs[3], 2048, false);
        CHECK(Core::ReadStamp(&a) == Core::ReadStamp(&b));
        CHECK_EQ(core->ShaderRows().size(), 0u);
        CHECK_EQ(core->Stats().shadersCreatedAfterHook, 0u);
        CHECK_EQ(core->PreHookShaders(), 0u);
    }

    // ---- outside the catalog: no stamp, but the epoch still advances ----------------------------------------------------------------
    {
        FakePs ps;
        const uint32_t before = core->Epoch();
        core->ShaderCreated(&ps, 0x12345678u, 300, false);
        CHECK_EQ(ps.setCalls, 0);
        CHECK(Core::ReadStamp(&ps) == nullptr);
        CHECK(core->Epoch() != before);
        const uint32_t mid = core->Epoch();
        FakePs stage;
        core->ShaderCreated(&stage, kSceneCrcs[0], 1500, false);
        CHECK(core->Epoch() != mid); // a catalog creation advances it too
    }

    // ---- the creation path allocates nothing and touches no registry ---------------------------------------------------------------
    {
        FakePs ps[16];
        g_allocs = 0;
        g_count = true;
        for (int i = 0; i < 4000; ++i)
        {
            FakePs& p = ps[i & 15];
            const uint32_t crc = (i % 3 == 0) ? kSceneCrcs[i % 5] : (i % 3 == 1) ? kUiCrcs[i % 7] : 0x9e3779b9u * uint32_t(i);
            core->ShaderCreated(&p, crc, 1500, false);
        }
        g_count = false;
        CHECK_EQ(g_allocs, 0);
        CHECK_EQ(core->ShaderRows().size(), 0u);
        CHECK_EQ(core->Stats().shadersCreatedAfterHook, 0u);
    }

    // ---- armed (registry = true): the D1 registry runs on top and its per-program record replaces the stage record ----------------------
    {
        FakePs stampOnly, armed;
        core->ShaderCreated(&stampOnly, kUiCrcs[1], 2048, false);
        core->ShaderCreated(&armed, kUiCrcs[1], 2048, true);
        CHECK_EQ(armed.setCalls, 2); // stage stamp, then the registry stamp over it
        const ShaderRec* stageRec = Core::ReadStamp(&stampOnly);
        const ShaderRec* programRec = Core::ReadStamp(&armed);
        CHECK(stageRec != nullptr && programRec != nullptr);
        CHECK(stageRec != programRec);
        if (programRec)
        {
            CHECK(programRec->hasCrc);
            CHECK_EQ(programRec->crc, kUiCrcs[1]);
            CHECK_EQ(programRec->stage, kUiStage);
            CHECK(programRec->ui);
            CHECK_EQ(programRec->instances, 1u);
        }
        CHECK_EQ(core->ShaderRows().size(), 1u);
        CHECK_EQ(core->Stats().shadersCreatedAfterHook, 1u);
    }
}
} // namespace

int main()
{
    Run();
    return Finish("ui_shader_stamp");
}
