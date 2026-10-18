#include "check.h"

#include "ext/ffxiv/chara/CharaContextState.h"

#include <d3d11_1.h>

using namespace ext::ffxiv::chara;

int main()
{
    // the constants are the SDK's
    static_assert(kDeviceCreateSingleThreaded == D3D11_CREATE_DEVICE_SINGLETHREADED);
    static_assert(kContextStateSingleThreaded == D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED);

    CHECK(ContextStateFlags(0) == 0);
    CHECK(ContextStateFlags(D3D11_CREATE_DEVICE_SINGLETHREADED) == D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED);
    // other creation flags do not matter
    CHECK(ContextStateFlags(D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_BGRA_SUPPORT) == 0);
    CHECK(ContextStateFlags(D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT) ==
          D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED);
    return TEST_RESULT("chara_context_state");
}
