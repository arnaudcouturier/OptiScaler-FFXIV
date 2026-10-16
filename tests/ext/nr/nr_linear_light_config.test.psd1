@{
    Class       = 'cpu'
    Note        = 'NR Linear light + composed (ReversibleMode 5): preset range accepts 0..5 and rejects 6 (real ext::NrConfig)'
    Sources     = @('nr_linear_light_config.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/OptiScaler/include', '{repo}/external/vulkan/include',
                    '{repo}/external/nvngx_dlss_sdk', '{repo}/external/xess/inc/xess', '{repo}/external/xess/inc/xell',
                    '{repo}/external/xess/inc/xess_fg', '{repo}/external/FidelityFX-SDK/ffx-api/include/ffx_api',
                    '{repo}/external/simpleini', '{repo}/external/unordered_dense/include', '{repo}/external/spdlog/include',
                    '{repo}/external/freetype', '{repo}/external/streamline', '{repo}/external/streamline1',
                    '{repo}/external/nvapi', '{repo}/external/nlohmann', '{repo}/external/fakenvapi',
                    '{repo}/external/magic_enum/include/magic_enum', '{repo}/external/AntiLag2-SDK',
                    '{repo}/external/latencyflex')
    Defines     = @('NOMINMAX')
    Args        = @()
}
