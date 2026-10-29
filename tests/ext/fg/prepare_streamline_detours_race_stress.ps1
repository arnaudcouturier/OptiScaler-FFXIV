# Test prepare step (tools\test.ps1 `Prepare`) for streamline_detours_race_stress: stages an isolated folder <Out>\stage with
#   - the product OptiScaler.dll built in this worktree (x64\Release\a\OptiScaler.dll),
#   - an OptiScaler.ini that turns the file log on (the test reads OptiScaler.log next to the DLL),
#   - a copy of the real Streamline plugins from the folder named by OPTISCALER_SL_PLUGIN_DIR (<Out>\stage\plugins),
# so the run reads and writes nothing outside <Out>.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$dll = Join-Path $Repo 'x64\Release\a\OptiScaler.dll'
if (-not (Test-Path $dll)) { Write-Error "no product build here: $dll"; exit 1 }
$plugins = $env:OPTISCALER_SL_PLUGIN_DIR
if (-not $plugins -or -not (Test-Path $plugins)) { Write-Error 'OPTISCALER_SL_PLUGIN_DIR is not set'; exit 1 }
$stage = Join-Path $Out 'stage'
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'plugins') | Out-Null
Copy-Item $dll (Join-Path $stage 'OptiScaler.dll')
Get-ChildItem $plugins -Filter 'sl.*.dll' | Copy-Item -Destination (Join-Path $stage 'plugins')
[IO.File]::WriteAllText((Join-Path $stage 'OptiScaler.ini'), "[Log]`r`nLogToFile=true`r`nLogLevel=0`r`n")
