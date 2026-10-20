@{
    Class   = 'cpu'
    Note    = 'Two same-named nvngx_dlssnr.dll files in different folders load as two modules when loaded by full path (the NR runtime picker relies on it); a lookup by name returns the first'
    Runner  = 'run_nr_same_name_modules.ps1'
    Sources = @('nr_same_name_modules.cpp', 'nr_same_name_dll.cpp')
}
