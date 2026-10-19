@{
    Class       = 'cpu'
    Note        = 'CharaView DLSS glue, source-text guard: the lines of CharaDlss.cpp and CharaNr.cpp that pass the facts, the colour and the composed-from-NR flag between the pure decision functions and the effects (no recipe links those files), comments stripped, with a negative control for each guarded edit'
    Sources     = @('chara_dlss_glue_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
