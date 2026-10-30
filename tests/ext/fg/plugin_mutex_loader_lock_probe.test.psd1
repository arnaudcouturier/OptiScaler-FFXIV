@{
    Class    = 'local'
    Note     = 'Loader-lock inversion probe: an empty sl.dlss_g.dll loaded on one thread while another thread inside a DllMain holds the loader lock and re-enters the load hook. Passes once the plugin hook mutexes are not held across a load and the catch-up never waits for them; a hang is exit 3 from an out-of-process launcher with every thread stack and the loader lock owner'
    Requires = @('{repo}/x64/Release/a/OptiScaler.dll')
    Runner   = 'run_plugin_mutex_loader_lock_probe.ps1'
    Sources  = @('plugin_mutex_loader_lock_probe.cpp', 'lock_holder.cpp', 'sl_dlss_g_stub.cpp', 'hang_launcher.cpp')
}
