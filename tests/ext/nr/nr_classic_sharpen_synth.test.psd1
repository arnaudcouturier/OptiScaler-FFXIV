@{
    Class       = 'warp'
    Note        = 'Classic sharpening shader on D3D12 WARP from staged source: bit-exact double replay, strength 0 copies, alpha kept, flat field untouched, negatives clamp to zero, strength orders the edit'
    Prepare     = 'prepare_shaders.ps1'
    Sources     = @('nr_classic_sharpen_synth.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'version.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{out}')
}
