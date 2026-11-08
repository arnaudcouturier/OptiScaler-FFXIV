param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
# The chain-order harness links the light bounce's and the engine light module's passes: both shader sets, into the one build folder.
foreach ($script in @('prepare_gi.ps1', 'prepare_engine_light.ps1')) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot $script) -Repo $Repo -Out $Out
    if ($LASTEXITCODE -ne 0) { throw "$script failed." }
}
