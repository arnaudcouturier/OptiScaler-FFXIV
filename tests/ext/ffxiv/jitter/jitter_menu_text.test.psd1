@{
    Class       = 'cpu'
    Note        = 'Game jitter menu card: both [FFXIV] jitter keys are written from their own checkbox, the card is registered after Output Scaling with the FFXIV visibility rule and decides nothing (source text pins, comments and string contents stripped)'
    Sources     = @('jitter_menu_text.cpp')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
