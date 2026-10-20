@{
    Class       = 'cpu'
    Note        = '[REL] Lock-order premise as a test: NR (ext/nr) and the frame generation module (ext/fg) never name the DX11 feature table, its pin or lock, or the DX11 NGX entry points in code (comments and string literals ignored), so the table lock stays a leaf and NR lock -> table lock is the only direction that can occur'
    Sources     = @('ngx_lock_order_pins_test.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('UNDEBUG')
}
