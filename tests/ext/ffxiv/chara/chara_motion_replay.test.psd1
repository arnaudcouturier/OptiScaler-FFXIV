@{
    Class       = 'warp'
    Note        = 'CharaView signed-motion replay on D3D11 WARP: the real CharaMotion.cpp replays a five-target G-buffer draw, the signed vector reaches the private float target unclamped, the game targets and every pipeline state stay untouched; UAV, open query, unknown shader, other table entry, own draw and size mismatch are skipped; a wider-shape draw gives the pass its camera and finishes it'
    Sources     = @('chara_motion_replay.cpp', '{repo}/OptiScaler/ext/ffxiv/chara/CharaMotion.cpp',
                    '{repo}/OptiScaler/ext/ffxiv/chara/CharaCamera.cpp',
                    '{repo}/OptiScaler/ext/core/ExtConfig.cpp', '{repo}/OptiScaler/ext/hdr/HdrConfig.cpp',
                    '{repo}/OptiScaler/ext/hudless/HudlessConfig.cpp', '{repo}/OptiScaler/ext/hudless/HudlessState.cpp')
    IncludeDirs = @('stubs', '{repo}/tests/ext/hdr/stubs', '{repo}/OptiScaler', '{repo}/external/simpleini')
    Libs        = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
