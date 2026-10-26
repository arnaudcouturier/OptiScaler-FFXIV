@{
    Class       = 'cpu'
    Note        = 'builds tools/ffxiv-resolve (patch-day CLI) and runs it: error paths always; on the real exe --strict --compare-table --margin, entry equals the table (prints SKIP: when the exe is absent)'
    Sources     = @('{repo}/tools/ffxiv-resolve/main.cpp')
    Runner      = 'run_ffxiv_resolve_cli.ps1'
}
