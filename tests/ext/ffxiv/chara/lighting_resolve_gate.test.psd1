@{
    Class       = 'cpu'
    Note        = 'Lighting layer hook transactions (ext/ffxiv/lighting/FfxivResolveGate.h): the run-time attach and detach of the ResolveSubresource detour are paired (attached once, detached once, nothing to detach after the main detach took it), the readiness check runs under the lock, and a transaction of one thread never overlaps a transaction of another (install, main detach, run-time attach and detach)'
    Sources     = @('lighting_resolve_gate.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
