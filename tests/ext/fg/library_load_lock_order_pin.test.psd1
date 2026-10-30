@{
    Class    = 'cpu'
    Note     = 'Load hook lock order pins: no plugin hook mutex is held across the DLL load in the five plugin branches, and the catch-up for modules already in memory uses try_to_lock and requires owns_lock() so it never waits with the loader lock held'
    Sources  = @('library_load_lock_order_pin.cpp')
    Args     = @('{repo}')
    Defines  = @('NOMINMAX')
    Std      = 'c++20'
}
