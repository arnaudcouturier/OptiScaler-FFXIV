@{
    Class       = 'cpu'
    Note        = 'Window mode menu card: the [Window] Mode key is written only from its combo with a known name, the active mode is shown from the published copy, the card is registered after the DLSSG settings with the FFXIV visibility rule and decides nothing (source text pins, comments and string contents stripped)'
    Sources     = @('window_menu_text.cpp')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
