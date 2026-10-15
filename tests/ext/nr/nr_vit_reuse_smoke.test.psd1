@{
    Class       = 'cpu'
    Note        = 'ViT bottleneck reuse filter: which launches of one NR evaluation are dropped (DlssNrVitReuse.h)'
    Sources     = @('nr_vit_reuse_smoke.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
