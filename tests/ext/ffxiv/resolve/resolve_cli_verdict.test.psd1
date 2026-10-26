@{
    Class       = 'cpu'
    Note        = 'patch-day CLI verdicts (tools/ffxiv-resolve/Verdict.h) on synthetic results: --strict and --compare-table fail on any DISAGREE, single-anchor and unresolved seams; entry comment stripping'
    Sources     = @('resolve_cli_verdict.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/tools/ffxiv-resolve')
    Defines     = @('NOMINMAX')
}
