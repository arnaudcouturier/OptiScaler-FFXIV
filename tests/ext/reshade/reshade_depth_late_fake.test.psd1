@{
    Class   = 'cpu'
    Note    = 'Automatic ReShade depth: late registration and event subscription against a stand-in ReShade DLL with the production sources (a ReShade that loads later is found, once; exactly the six depth events are subscribed, each once, begin last, nothing else; the planner interval, the option gate and the 60 attempts bound; a refusal is final; a fault inside the ReShade registration is a failed attempt; an event that cannot be subscribed is remembered and begin is then never subscribed; sixteen threads make one attempt; the registered callbacks reach the binder, are not gated by the enabled flag and stand down while shutting down)'
    Sources = @('reshade_depth_late_fake.cpp', 'fake_reshade.cpp')
    Runner  = 'run_reshade_depth_late_fake.ps1'
    Std     = 'c++latest'
}
