@{
    Class       = 'cpu'
    Note        = 'status.json renderer: real ToJson output parses as strict JSON (full/slim/minimal), stays capped and clean, gates on state only, guards version text'
    Sources     = @('status_json.cpp', '{repo}/OptiScaler/ext/status/StatusJson.cpp')
    IncludeDirs = @('stubs', '{repo}/OptiScaler')
    Std         = 'c++20'
}
