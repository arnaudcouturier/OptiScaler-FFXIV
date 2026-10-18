@{
    Class       = 'cpu'
    Note        = 'CharaView probe files: RGBA/BGRA to RGB + alpha conversion (channel order, row pitch, failures), dump plan with exact PNG sizes, dump folder naming (-2/-3), all-or-nothing output budget in front of the writes, state.txt text, UTF-8 path conversion'
    Sources     = @('chara_probe_files.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++latest'
}
