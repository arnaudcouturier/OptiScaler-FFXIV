@{
    Class       = 'cpu'
    Note        = 'FG recorder record encoding: every session line parses as strict JSON, fields carry the call values with contiguous sequences, hostile bytes are escaped or replaced and long messages cut at 95 characters (a broken escaper fails the parse)'
    Sources     = @('fgrec_record.cpp')
    IncludeDirs = @('stubs', '.', '{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
