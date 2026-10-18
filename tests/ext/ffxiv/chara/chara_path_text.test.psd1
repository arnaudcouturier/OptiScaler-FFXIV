@{
    Class       = 'cpu'
    Note        = 'ext::PathText: a path with non-ANSI characters (Cyrillic, Japanese, a lone surrogate, empty) becomes UTF-8 text without throwing, the replacement for path::string() in log lines'
    Sources     = @('chara_path_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
