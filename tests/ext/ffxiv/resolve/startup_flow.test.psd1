@{
    Class       = 'cpu'
    Note        = 'Startup decision (StartupDecision.h) with a fake environment: not the game / rollback key open nothing, the published build is the resolved one, the table is offered unless FfxivForceUnknownBuild, a resolver failure falls back to the table on the exact known build and publishes zeros elsewhere, log and publish failures decide nothing, the keys map to their own flags, the exe path is read without the MAX_PATH cut'
    Sources     = @('startup_flow.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
