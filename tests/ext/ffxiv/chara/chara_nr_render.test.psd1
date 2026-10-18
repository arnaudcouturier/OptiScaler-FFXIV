@{
    Class       = 'cpu'
    Note        = 'CharaView preview NR, the decisions of one render called as the dispatcher calls them: every module outcome other than an image (Preparing, Skipped, Failed, NotAvailable, no image, a failed alpha pass) keeps the super-resolution output and withdraws the older NR result, a produced image does the opposite, the effects run in the glue order and only when the rules allow them, and the functions agree with the logic the glue used to carry inline over a sweep of every input'
    Sources     = @('chara_nr_render.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
