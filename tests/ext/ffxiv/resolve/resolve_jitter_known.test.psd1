@{
    Class       = 'cpu'
    Note        = 'jitter-source shapes on a KNOWN build (P2c fix): a shape that does not match is never replaced by the table guard prefix (failClosed): the review PoC rows and every non-wildcard byte of every shape are refused through the real ResolveImage and VerifyAnchors; control: without failClosed the PoC rows install'
    Sources     = @('resolve_jitter_known.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
