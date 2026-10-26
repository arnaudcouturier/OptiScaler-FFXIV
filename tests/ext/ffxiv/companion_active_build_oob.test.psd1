@{
    Class       = 'cpu'
    Note        = 'Companion bounds: a resolved batch header offset outside the 64-byte header refuses every packet (no read outside the array)'
    Sources     = @('companion_active_build_test.cpp')
    Args        = @('oob')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
