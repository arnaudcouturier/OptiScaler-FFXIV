@{
    Class       = 'cpu'
    Note        = 'HUD-less and recorder UX wiring pins: both paste-gate sites go through Core::PasteGate(_tint), the card prints the guard status line, keeps the coverage guard under its own sub-heading with the warning under its slider, logs each settled slider once, its Reset tooltip names the guard, the mark key is a Keybinds row (slot 19) with the "user mark N" line, and the recorder keeps 8 rotations of 16 MiB'
    Sources     = @('hudless_ux_wiring.cpp')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
