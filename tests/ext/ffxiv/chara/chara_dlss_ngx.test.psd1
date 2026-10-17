@{
    Class       = 'cpu'
    Note        = 'CharaView isolated DLSS NGX composition: create flags 0x42 (0x4A with a reversed depth buffer), render and 2x output sizes, MaxPerf, the evaluate parameter set (resources, jitter without sign flip, motion scale, subrect, reset) recorded through a fake parameter object'
    Sources     = @('chara_dlss_ngx.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
