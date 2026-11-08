@{
    Class       = 'warp'
    Note        = 'The puddles'' chained order at the tone map, driven through the draw path''s own entries on D3D11 WARP (ext/ffxiv/engine/PuddleToneDraw.cpp): the gate the light shafts ask is off before a composite of the frame and on after one, the chain runs on the scene the shafts hand over (its output is the pass''s own output for that air-lit scene to the bit and differs from the pass''s output for the pre-shafts scene, so the hand-over reached the shader), the answer is stamped so the gate goes off again and the seam''s own call runs no second pass and no draw, a dry frame is refused with nothing stamped or counted, a frame whose composite carries no shape is answered with the pass''s own last output to the bit and counted as held rather than a run, and the fallback still runs a tone map the chain did not take - one draw, this module''s texture at t0 for that draw, the game''s own view back afterwards'
    Prepare     = 'prepare_puddle_tone.ps1'
    Sources     = @('puddle_tone_chain_warp.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/PuddleToneDraw.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/PuddleTonePass.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/engine/GlossPuddlesSwap.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler', '{out}')
    Args        = @('{repo}')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
