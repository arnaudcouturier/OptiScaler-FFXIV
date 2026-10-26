@{
    Class       = 'cpu'
    Note        = 'resolver image side: PE header validation, dense image (section-mapped, zero-filled, reads only inside headers+sections), region classification (bss tail), live process image, .pdata enclosing-function lookup with chained and missing entries'
    Sources     = @('resolve_image_unwind.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
}
