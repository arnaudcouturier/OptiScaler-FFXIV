@{
    Class       = 'cpu'
    Note        = 'The simulation-tick anchors on the two real executables (the build the table describes and the 2024 benchmark build, both read only): the five seams resolve from the bytes alone with the values both builds are known to have, both interior anchors of the tick agree, the three counter offsets have the layout the install checks, every anchor passes the offline margin rule, the static Framework vtable still holds the tick in slot 4, and on the table build three decoys behave as the design needs (one interior block broken keeps the tick usable, a foreign E9 over the entry is not EntryPatched, both blocks broken turns the feature off); prints SKIP: when an executable is absent'
    Sources     = @('resolve_latency_anchors.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/tools/ffxiv-resolve')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}/../analysis/patch-20261016/ffxiv_dx11.exe', '{repo}/../tools-downloads/ffxiv-benchmark/game/ffxiv_dx11.exe')
}
