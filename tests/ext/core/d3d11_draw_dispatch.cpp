#include "ext/core/D3D11DrawDispatch.h"
#include <stdexcept>

namespace
{
using namespace ext::core::draw_dispatch;
struct Context { void** table; void* vtable[41]{}; Context() : table(vtable) {} };
int calls = 0;
UINT seenCount = 0, seenStart = 0;
INT seenBase = 0;
void STDMETHODCALLTYPE NativeIndexed(ID3D11DeviceContext*, UINT count, UINT start, INT base)
{ ++calls; seenCount = count; seenStart = start; seenBase = base; }
void STDMETHODCALLTYPE ThrowingIndexed(ID3D11DeviceContext*, UINT, UINT, INT)
{ throw std::runtime_error("dispatch test"); }
}

int main()
{
    Context context{};
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(&context);
    auto native = reinterpret_cast<void*>(&NativeIndexed);
    context.vtable[12] = native;
    calls = 0;
    Forward(ctx, 12, native, &NativeIndexed, Api::Indexed, 7u, 3u, -2);
    if (calls != 1 || seenCount != 7 || seenStart != 3 || seenBase != -2) return 1;

    auto replacement = reinterpret_cast<void*>(&ThrowingIndexed);
    context.vtable[12] = replacement;
    try { Forward(ctx, 12, native, &NativeIndexed, Api::Indexed, 9u, 4u, 1); return 2; }
    catch (const std::runtime_error&) {}
    if (IsFreshReentry(ctx, Api::Indexed)) return 3;

    bool entered = false;
    {
        FreshEntryScope fresh(ctx, Api::Indexed);
        if (!IsFreshReentry(ctx, Api::Indexed) || IsFreshReentry(ctx, Api::Draw)) return 4;
        LayerScope outer(ctx, Api::Indexed, &entered);
        if (!outer) return 5;
        LayerScope duplicate(ctx, Api::Indexed, &entered);
        if (duplicate) return 6;
        LayerScope otherApi(ctx, Api::Draw, &entered);
        if (!otherApi) return 7;
        Context other{};
        LayerScope otherContext(reinterpret_cast<ID3D11DeviceContext*>(&other), Api::Indexed, &entered);
        if (!otherContext) return 8;
    }
    if (IsFreshReentry(ctx, Api::Indexed)) return 9;
    context.vtable[12] = native;
    calls = 0;
    Forward(ctx, 12, native, &NativeIndexed, Api::Indexed, 5u, 2u, 0);
    if (calls != 1) return 10;
    return 0;
}
