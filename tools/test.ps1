<#
.SYNOPSIS
  Placeholder test runner. Discovers tests\ext and reports what it finds.
  The runner proper (CPU-only and WARP tests; hardware tests listed only) arrives with the test series.
#>
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'tests\ext'
$found = @()
if (Test-Path $dir) { $found = @(Get-ChildItem $dir -Recurse -File -Filter *.cpp) }
if ($found.Count -eq 0) {
    Write-Host '[test] tests\ext: no tests yet (nothing to run)'
    exit 0
}
Write-Host ("[test] tests\ext: {0} test source(s) found, runner not implemented yet:" -f $found.Count)
$found | ForEach-Object { Write-Host ('  ' + $_.FullName.Substring($root.Length + 1)) }
exit 0
