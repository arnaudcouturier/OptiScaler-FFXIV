@{
    Class       = 'cpu'
    Note        = 'Consumer conversion pinned on the sources, per function (the feature each consumer asks for, the key / stamp / size arguments of the selector, the env wiring of the startup, no MAX_PATH): the only readers of the compile-time table / exact-build match are the listed files, every converted consumer reads ActiveBuild() and asks the gate for its own feature, the startup is called once at the top of the first device hook (never DllMain), companion::Install is gated, Group 1 on the exe name, the constants that stay compile-time are Constant field rows'
    Sources     = @('gate_consumer_pins.cpp')
    Defines     = @('NOMINMAX')
    Args        = @('{repo}')
    Std         = 'c++20'
}
