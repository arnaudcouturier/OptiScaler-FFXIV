@{
    Class       = 'cpu'
    Note        = 'P2a quality group against build B, B-read rows only: the settings block (37 recorded bytes) resolves to B 0x374347 / global 0x28f9470 with B offsets; querySize / rendererUpdate / resizeCallbacks anchors are prefix-compatible with the recorded B bytes and every B-to-C byte change is a masked byte; wrong-build control refused by TargetIn(Bss)'
    Sources     = @('resolve_quality_b_fixture.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
