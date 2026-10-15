<#
.SYNOPSIS
  Builds OptiScaler.sln (Release x64 by default) with MSBuild. Log goes to build.log at the worktree root.
.DESCRIPTION
  Exit code is MSBuild's exit code. On failure a de-duplicated error summary is printed.
  For parallel agents use tools\build-locked.ps1, which serializes machine-wide builds.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [switch]$Rebuild,
    [int]$MaxErrors = 60
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $root 'build.log'

$msbuild = $env:OPTISCALER_MSBUILD
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    $msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
}
if (-not (Test-Path $msbuild)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    }
}
if (-not $msbuild -or -not (Test-Path $msbuild)) { Write-Error 'MSBuild.exe not found (set OPTISCALER_MSBUILD)'; exit 2 }

$target = if ($Rebuild) { '-t:Rebuild' } else { '-t:Build' }
Write-Host "[build] $root  ($Configuration|$Platform)"
$sw = [Diagnostics.Stopwatch]::StartNew()

Push-Location $root
try {
    & $msbuild OptiScaler.sln $target "-p:Configuration=$Configuration" "-p:Platform=$Platform" -m -nologo -v:m `
        "-flp:logfile=$log;verbosity=minimal;encoding=utf-8" | Out-Host
    $code = $LASTEXITCODE
}
finally { Pop-Location }

$sw.Stop()
if ($code -eq 0) {
    Write-Host ("[build] OK in {0:N0}s" -f $sw.Elapsed.TotalSeconds)
    exit 0
}

# Error summary: strip the node prefix and the [project] suffix, make paths worktree relative, dedupe.
$seen = [System.Collections.Generic.List[string]]::new()
$set = [System.Collections.Generic.HashSet[string]]::new()
if (Test-Path $log) {
    foreach ($l in Get-Content $log -Encoding UTF8) {
        if ($l -notmatch '(error|fatal error) [A-Za-z]+\d*\s*:') { continue }
        $t = $l.Trim() -replace '^\d+>', '' -replace '\s*\[[^\]]*\]\s*$', ''
        $t = $t.Replace($root + '\', '')
        if ($set.Add($t)) { $seen.Add($t) }
    }
}
Write-Host ("[build] FAILED (exit {0}) in {1:N0}s, {2} unique error(s), see {3}" -f $code, $sw.Elapsed.TotalSeconds, $seen.Count, $log)
$seen | Select-Object -First $MaxErrors | ForEach-Object { Write-Host ('  ' + $(if ($_.Length -gt 260) { $_.Substring(0, 260) } else { $_ })) }
if ($seen.Count -gt $MaxErrors) { Write-Host ("  ... {0} more" -f ($seen.Count - $MaxErrors)) }
exit $code
