@{
    Class   = 'cpu'
    Note    = 'tools\package-release.ps1 privacy guard: a packed PE holding a drive:\Users\ path (ASCII or UTF-16LE) stops the script with exit 5, third-party libraries only warn, Licenses\ entries go through the block-list and the scan; forwarder links with /PDBALTPATH'
    Runner  = 'run_package_release_scan.ps1'
    Sources = @()
}
