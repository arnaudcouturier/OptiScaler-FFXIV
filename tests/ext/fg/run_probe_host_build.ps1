# Compile-only runner for the release-model probe host (tools/fg-probe/FgProbe.cpp).
#
# The host is a standalone cl.exe build outside OptiScaler.sln, so nothing in the product build notices a header change
# that breaks it; the next probe run would be the first to find out. This recipe builds it into the test's own folder and
# fails on a compiler error. It does not run the host: the host creates frame-generation chains and releases references on
# purpose, which is a measurement somebody starts by hand, never part of the gate.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }
$exe = Join-Path $Out 'fg-probe.exe'

$text = & pwsh -NoProfile -File (Join-Path $Repo 'tools/fg-probe/build.ps1') -Out $exe 2>&1 | Out-String
$code = $LASTEXITCODE
Write-Host $text
if ($code -ne 0) {
    Write-Host "[fg-probe] the host does not compile ($code)"
    exit 1
}
if (-not (Test-Path $exe)) {
    Write-Host "[fg-probe] the build reported success but produced no exe: $exe"
    exit 1
}
Write-Host '[fg-probe] the release-model host compiles'
exit 0
