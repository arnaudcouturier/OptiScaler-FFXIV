@{
    Class       = 'local'
    Gpu         = $true
    Note        = 'Real Vulkan device (not WARP): production SPIR-V (OptiScaler/x64/Release/ext_gen/DlssNr_Shader_Vk.h.bin, written by the product build of this worktree) encode/resolve'
    Requires    = @('{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_Shader_Vk.h.bin')
    Sources     = @('nr_vulkan_shader_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/external/vulkan/include')
    Libs        = @('{repo}/OptiScaler/library/vulkan/vulkan-1.lib')
    Args        = @('{repo}/OptiScaler/x64/Release/ext_gen/DlssNr_Shader_Vk.h.bin')
}
