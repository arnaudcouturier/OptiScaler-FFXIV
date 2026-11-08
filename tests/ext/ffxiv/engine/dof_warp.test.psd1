@{
    Class       = 'warp'
    Note        = 'Depth of field Route B''s pass on D3D11 WARP with the real shaders and the real Pass class on synthetic buffers: the auto focus (the view depth under the screen centre) and the manual key both reconstruct through the march''s fixture camera, the near and far disc blur''s PSF matches the CPU reference per pixel at two step counts, two apertures and both focus modes, a focused column stays byte-exact at every setting, on a noise scene the focus band (raw CoC within 0.12) stays byte for byte on both sides while the near plane''s CoC is held at 0.25 (the near amount never passes 0.25 x strength) and the far plane beyond the band keeps its uncapped blur, the merge equal to scene + (target - scene) x amount per pixel, a Strength-0 run answers one CopyResource with no draws, the pack''s min(col,10) cap keeps a highlight from smearing through the taps, the merge stays inside the sources'' own range, the game''s bindings come back after a run, and a binding the pass cannot read returns null with a reason'
    Prepare     = 'prepare_dof.ps1'
    Sources     = @('dof_warp.cpp', '{repo}/OptiScaler/ext/ffxiv/engine/DofPass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/DofCamera.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
