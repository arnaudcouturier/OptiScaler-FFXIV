<#
.SYNOPSIS
  Diff stat of a revision against the upstream base, excluding fork-only paths.
.DESCRIPTION
  Base = merge-base of <Rev> and upstream/master (fallback fd6c2870).
  Target (docs/SEAMS.md section 1): <= 40 upstream files and <= 1000 changed lines.
#>
[CmdletBinding()]
param(
    [string]$Rev = 'HEAD',
    [int]$MaxFiles = 40,
    [int]$MaxLines = 1000
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $base = (git merge-base $Rev upstream/master 2>$null | Select-Object -First 1)
    if (-not $base) { $base = 'fd6c2870' }

    $forkOnly = @(
        '^OptiScaler/ext/', '^docs/', '^tools/', '^tests/ext/',
        '^\.github/README\.md$', '^\.github/workflows/ffxiv-'
    )

    $rows = @()
    foreach ($line in (git diff --numstat --no-renames $base $Rev)) {
        $p = $line -split "`t", 3
        if ($p.Count -lt 3) { continue }
        $path = $p[2]
        if ($forkOnly | Where-Object { $path -match $_ }) { continue }
        $add = if ($p[0] -eq '-') { 0 } else { [int]$p[0] }
        $del = if ($p[1] -eq '-') { 0 } else { [int]$p[1] }
        $rows += [pscustomobject]@{ Add = $add; Del = $del; Path = $path }
    }

    Write-Host ("footprint of {0} vs upstream base {1}" -f $Rev, $base.Substring(0, [Math]::Min(8, $base.Length)))
    foreach ($r in ($rows | Sort-Object { $_.Add + $_.Del } -Descending)) {
        Write-Host ("  +{0,-5} -{1,-5} {2}" -f $r.Add, $r.Del, $r.Path)
    }
    $add = ($rows | Measure-Object Add -Sum).Sum
    $del = ($rows | Measure-Object Del -Sum).Sum
    if (-not $add) { $add = 0 }
    if (-not $del) { $del = 0 }
    $lines = $add + $del
    $okF = if ($rows.Count -le $MaxFiles) { 'OK' } else { 'OVER' }
    $okL = if ($lines -le $MaxLines) { 'OK' } else { 'OVER' }
    Write-Host ("total: {0} files (target <= {1}) {2}; +{3} -{4} = {5} lines (target <= {6}) {7}" -f `
            $rows.Count, $MaxFiles, $okF, $add, $del, $lines, $MaxLines, $okL)
}
finally { Pop-Location }
