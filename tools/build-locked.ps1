<#
.SYNOPSIS
  tools\build.ps1 serialized machine-wide with the named mutex Global\OptiScalerFFXIV_Build.
.DESCRIPTION
  Waits (and says so) while another build holds the mutex. The mutex is released on exit; if the
  holder crashed, Windows marks it abandoned and the next waiter takes over.
  All arguments are passed through to build.ps1.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [switch]$Rebuild,
    [int]$MaxErrors = 60
)

$ErrorActionPreference = 'Stop'
$mutex = New-Object System.Threading.Mutex($false, 'Global\OptiScalerFFXIV_Build')
$held = $false
$code = 1
try {
    try {
        if (-not $mutex.WaitOne(0)) {
            Write-Host "[build-locked] another build is running, waiting... ($(Get-Date -Format HH:mm:ss))"
            $t = [Diagnostics.Stopwatch]::StartNew()
            $mutex.WaitOne() | Out-Null
            Write-Host ("[build-locked] acquired after {0:N0}s" -f $t.Elapsed.TotalSeconds)
        }
    }
    catch [System.Threading.AbandonedMutexException] {
        Write-Host '[build-locked] previous holder died, taking over the lock'
    }
    $held = $true

    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Platform $Platform -Rebuild:$Rebuild -MaxErrors $MaxErrors
    $code = $LASTEXITCODE
}
finally {
    if ($held) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
exit $code
