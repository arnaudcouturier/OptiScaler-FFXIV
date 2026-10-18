@{
    Class       = 'cpu'
    Note        = 'CharaView preview NR plumbing rules: the SR and NR paths run independently (an SR latch or a closed SR gate does not stop NR and the reverse), the plumbing stays alive while either path may run or any GPU object exists, the dispatcher gate and maintenance forwarding stay on while NR holds resources (so turning PreviewNR off frees the models), draws are offered to the plumbing when only PreviewNR is on, NR bring-up does not wait for SR, the production entry needs a successful SR evaluate, consumer source priority'
    Sources     = @('chara_nr_plumbing.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
    Defines     = @('NOMINMAX')
    Std         = 'c++20'
}
