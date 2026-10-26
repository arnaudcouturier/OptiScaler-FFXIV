@{
    Class       = 'cpu'
    Note        = 'golden test on the real build-C executable (analysis\patch-20261016\ffxiv_dx11.exe, never committed): the entry the resolver generates equals GameBuild.h for every registered seam; prints resolve time and anchor margins; prints SKIP: when the exe is absent'
    Sources     = @('resolve_golden_exe.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/tools/ffxiv-resolve')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}/../analysis/patch-20261016/ffxiv_dx11.exe')
}
