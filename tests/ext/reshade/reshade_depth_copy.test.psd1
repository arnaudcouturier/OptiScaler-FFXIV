@{
    Class       = 'warp'
    Note        = 'Automatic ReShade depth, D3D11 half on WARP: view format table, request checks, crop and nearest remap against two CPU references (6 source families, 12 cases), the game context state, bypass scope, publish order on recreate / Release / device change, refusals, faults, the product shader driven directly (clamp, largest edge)'
    Prepare     = 'prepare_depth_copy.ps1'
    Sources     = @('reshade_depth_copy.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeDepthCopy.cpp', '{repo}/OptiScaler/ext/reshade/ReShadeD3D11.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}/ext_gen')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX', 'UNDEBUG')
    Std         = 'c++latest'
}
