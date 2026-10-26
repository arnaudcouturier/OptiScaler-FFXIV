@{
    Class       = 'cpu'
    Note        = 'ffxiv-resolve --emit-gamebuild core (tools/ffxiv-resolve/EmitGameBuild.h) on the committed GameBuild.h: idempotence, round trip of perturbed RVAs/offsets/guard bytes/stamp, struct scoping, shape and array lengths, header-does-not-fit errors, refuse and --allow-partial marks on a real resolve of a synthetic image, build B as a second build, field partition'
    Sources     = @('resolve_emit_gamebuild.cpp')
    IncludeDirs = @('{repo}/OptiScaler', '{repo}/tools/ffxiv-resolve')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}')
}
