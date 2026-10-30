@{
    Class       = 'cpu'
    Note        = 'The simulation-tick lease rules on ordinary memory: the install checks (ready, not ready, a vtable outside read-only image data, a slot another owner holds, a counter layout that does not fit), the layout rule on both real builds and three broken pairs, the compare-exchange install and the restore that refuses a slot a second owner took back, the pass-through decision, and the retry plan (first attempt at once, every 120 presents, ten minutes, eight refusals)'
    Sources     = @('tick_lease.cpp')
    IncludeDirs = @('{repo}/OptiScaler')
}
