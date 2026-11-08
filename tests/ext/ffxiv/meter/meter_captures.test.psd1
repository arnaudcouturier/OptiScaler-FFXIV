@{
    Class       = 'local'
    Note        = 'Effect meter acceptance on the user''s own live captures (2026-11-07 16:41-16:44 play session, draw-diag folders 20261107-164135, -164337-2 and -164414-3): the real MeterPass on D3D11 WARP measures every stage image pair the capture frames left (contact shadows, occlusion, reflections, light bounce, depth of field, shafts, night grade where they ran), the GPU share / mean / coverage land within 1 % of the CPU diff of the very same images and the GPU equals the CPU tap emulation. Needs the capture folders on this machine; no game, no GPU'
    Prepare     = 'prepare_meter.ps1'
    Sources     = @('meter_captures.cpp', '{repo}/OptiScaler/ext/ffxiv/meter/MeterPass.cpp')
    IncludeDirs = @('../engine/stubs', '../engine', '{repo}/OptiScaler', '{out}')
    Libs        = @('d3d11.lib', 'dxgi.lib')
    Defines     = @('NOMINMAX')
    Args        = @('C:/Games/Steam/steamapps/common/FINAL FANTASY XIV Online/game/OptiScaler/draw-diag')
    Requires    = @('C:/Games/Steam/steamapps/common/FINAL FANTASY XIV Online/game/OptiScaler/draw-diag/20261107-164414-3/dump/facts.txt', 'C:/Games/Steam/steamapps/common/FINAL FANTASY XIV Online/game/OptiScaler/draw-diag/20261107-164337-2/dump/facts.txt', 'C:/Games/Steam/steamapps/common/FINAL FANTASY XIV Online/game/OptiScaler/draw-diag/20261107-164135/dump/facts.txt')
    Std         = 'c++latest'
}
