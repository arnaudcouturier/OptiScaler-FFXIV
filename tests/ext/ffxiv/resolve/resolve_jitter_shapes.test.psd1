@{
    Class       = 'cpu'
    Note        = 'jitter-source strict shapes: the hook-site registry as data (location + shape + failClosed + bindings), the literal mask map, and every non-wildcard byte of every shape flipped on an unknown build (shape not usable, Guard zero, VerifyAnchors refuses through its block, the shape exercised on its own)'
    Sources     = @('resolve_jitter_shapes.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
