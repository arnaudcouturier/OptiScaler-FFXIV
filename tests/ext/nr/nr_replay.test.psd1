@{
    Class       = 'local'
    Note        = 'NR frame replay of a captured set (OPTISCALER_NR_REPLAY_DIR) on D3D12 WARP with this build''s compiled shaders, bit-exact against the blessed images; OPTISCALER_NR_REPLAY_BLESS=all|kinds with OPTISCALER_NR_REPLAY_BLESS_OK=1 re-blesses (without the confirmation the run exits 3 and writes nothing); needs the product build''s generated shader headers'
    Requires    = @('OPTISCALER_NR_REPLAY_DIR',
                    '{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_Shader.h',
                    '{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_GuideMatch_Shader.h',
                    '{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_Spatial_Shader.h',
                    '{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_Spatial_Guides_Shader.h',
                    '{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_ClassicSharpen_Shader.h')
    Sources     = @('nr_replay.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/OptiScaler/x64/Release/ext_gen')
    Libs        = @('d3d12.lib', 'dxgi.lib', 'version.lib')
    Defines     = @('NOMINMAX')
    Args        = @('{env:OPTISCALER_NR_REPLAY_DIR}', '{repo}')
}
