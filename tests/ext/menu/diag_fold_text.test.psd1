@{
    Class       = 'cpu'
    Note        = 'Diagnostics cards (HUD-less diagnostics, ReShade add-on probe) keep their details in a closed plain TreeNode drawn by the card function (TreePop balanced, no return); the HUD-less and HDR cards have a SectionTitle equal to the picker name and no fold opens by default (source text pins, negative controls)'
    Sources     = @('diag_fold_text.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Args        = @('{repo}')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
