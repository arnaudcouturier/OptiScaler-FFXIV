<#
.SYNOPSIS
  Builds the release-model probe host (tools\fg-probe\FgProbe.cpp) into x64\fg-probe.exe.
.DESCRIPTION
  The probe host is a separate executable on purpose: the measurement creates its own chains and releases a reference
  that may already be freed, which must never happen in the game's process. It is not part of OptiScaler.sln.

  Standalone cl.exe build with the vcvars64 environment (located with vswhere, or OPTISCALER_VCVARS). The FFX API
  headers come from the vendored FidelityFX SDK, the XeSS-FG headers from external\xess. Exit code is cl.exe's.

  Usage:  pwsh -NoProfile -File tools\fg-probe\build.ps1 [-Out x64\fg-probe.exe]
#>
[CmdletBinding()]
param(
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tools\fg-probe -> repo root
if (-not $Out) { $Out = Join-Path $root 'x64\fg-probe.exe' }
$Out = [IO.Path]::GetFullPath($Out)

$vcvars = $env:OPTISCALER_VCVARS
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $install = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 }
    $vcvars = if ($install) { Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat' }
    if (-not $vcvars -or -not (Test-Path $vcvars)) { $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat' }
}
if (-not (Test-Path $vcvars)) { Write-Error 'vcvars64.bat not found (set OPTISCALER_VCVARS)'; exit 2 }

$ffx = Join-Path $root 'external\FidelityFX-SDK-v2\Kits\FidelityFX'
$includes = @(
    (Join-Path $root 'OptiScaler'),
    (Join-Path $ffx 'api\include'),
    (Join-Path $ffx 'framegeneration\include'),
    (Join-Path $root 'external\xess\inc')
)
foreach ($i in $includes) {
    if (-not (Test-Path $i)) { Write-Error "include folder missing: $i"; exit 2 }
}

$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$source = Join-Path $PSScriptRoot 'FgProbe.cpp'
$inc = ($includes | ForEach-Object { '/I"{0}"' -f $_ }) -join ' '
$cl = @(
    '/nologo', '/std:c++20', '/EHsc', '/O2', '/W3', '/MD',
    '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
    $inc,
    ('"{0}"' -f $source),
    ('/Fe"{0}"' -f $Out),
    '/link', 'd3d12.lib', 'dxgi.lib', 'ole32.lib', 'user32.lib', 'advapi32.lib'
) -join ' '

Write-Host "[fg-probe] building $Out"
$cmd = 'call "{0}" >nul && cl.exe {1}' -f $vcvars, $cl
# The object file lands in the working directory, which keeps the /Fo quoting (a trailing backslash before a quote)
# out of the command line.
Push-Location $outDir
try { cmd /c $cmd } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Write-Host "[fg-probe] FAILED ($LASTEXITCODE)"; exit $LASTEXITCODE }
Write-Host "[fg-probe] OK $Out"
exit 0
