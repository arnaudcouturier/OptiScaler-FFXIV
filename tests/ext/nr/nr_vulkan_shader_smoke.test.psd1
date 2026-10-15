@{
    Class       = 'manual'
    Note        = 'Real Vulkan device (not WARP): production SPIR-V (OptiScaler\x64\Release\ext_gen\DlssNr_Shader_Vk.h.bin, written by the product build) encode/resolve; build line is in the source header; args = the .bin path'
    Sources     = @('nr_vulkan_shader_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
