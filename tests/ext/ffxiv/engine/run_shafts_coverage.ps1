<#
.SYNOPSIS
  Runs the light-shaft coverage tool's own checks for tools\test.ps1.
.DESCRIPTION
  The tool is Python and its cases are fabricated captures and logs, so this recipe has no compile step: the runner runs the checks with the
  interpreter a session's replay uses and passes the exit code through. The checks build their fixtures in a temporary folder and delete them.
#>
param(
    [string]$Repo,
    [string]$Out
)

$ErrorActionPreference = 'Stop'
$checks = Join-Path $Repo 'tools\shafts-coverage-selftest.py'
if (-not (Test-Path $checks)) { Write-Host "no $checks"; exit 2 }
& python -I -B $checks
exit $LASTEXITCODE
