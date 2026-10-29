@{
    Class       = 'cpu'
    Note        = 'dlssnr.hlsl compiles for Vulkan with the repo dxc (-spirv -T cs_6_0 -E CSMain -O3 -D VK_MODE, the product recipe), so a Direct3D 12-only binding (the depth input of the depth-aware enlargement) stays outside the Vulkan build; with a negative control'
    Runner      = 'run_nr_vk_shader_compile.ps1'
    Sources     = @()
}
