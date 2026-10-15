# nvngx.dll_dlssnr.dll

Neural Rendering's snippet (`nvngx_dlssnr.dll`) resolves the module that owns its caller's return address and refuses
anything whose path does not contain `nvngx.dll` -- the driver core being `_nvngx.dll`. It returns
`FAIL_PlatformError` before it inspects a single argument. OptiScaler installs as `dxgi.dll` or `winmm.dll`, so it
fails that test like anything else would.

This library exists to be named correctly. It does nothing else: it forwards create, evaluate and release, so the
calls into the snippet originate from a module the snippet accepts. The caller check is inside NVIDIA's code, which is
why the name `nvngx.dll_dlssnr.dll` cannot be changed, and why OptiScaler's `nvngx.dll` spoof must leave paths
containing `nvngx.dll_` alone (the `Kernel_Hooks` carve-out, S2b).

No binary is committed. It is the second project of `OptiScaler.sln` (`dlssnr_forwarder.vcxproj`, v143, static CRT)
and builds into `x64\<Configuration>\a\`, next to `OptiScaler.dll`; the packaging scripts ship it from there. Nothing
in `OptiScaler.dll` links against it: it is loaded by file name at run time (beside OptiScaler first, then beside the
game executable) and its exports (`dlssnr_call_*`, `dlssnr_vk_*`, `dlssnr_d3d11_*`, `dlssnr_query_scaling_ratio`) are an
ABI between the two.

One detail is load-bearing and not obvious. The forwarder must not `return snippetFn(...)` -- that is a tail call,
the compiler emits a `jmp`, this module's frame disappears, and the snippet then resolves its caller to whoever called
the forwarder. The result goes into a `volatile` local first.
