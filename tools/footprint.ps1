<#
.SYNOPSIS
  Diff stat of a revision against the upstream base, excluding fork-only paths.
.DESCRIPTION
  Base = merge-base of <Rev> and upstream/master (fallback fd6c2870).
  Target (docs/SEAMS.md section 1.3): <= 600 lines of persistent surface. [UP] commits
  (upstream-candidate fixes) are counted separately; the file count is reported only.
#>
[CmdletBinding()]
param(
    [string]$Rev = 'HEAD',
    [int]$MaxLines = 600
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
    # Lines contributed by [UP] commits (upstream-candidate fixes), upstream paths only.
    $up = 0
    foreach ($c in (git log --format=%H --grep='^\[UP\]' "$base..$Rev")) {
        foreach ($line in (git show --numstat --format= --no-renames $c)) {
            $p = $line -split "`t", 3
            if ($p.Count -lt 3) { continue }
            if ($forkOnly | Where-Object { $p[2] -match $_ }) { continue }
            if ($p[0] -ne '-') { $up += [int]$p[0] }
            if ($p[1] -ne '-') { $up += [int]$p[1] }
        }
    }
    $persistent = [Math]::Max(0, $lines - $up)
    $ok = if ($persistent -le $MaxLines) { 'OK' } else { 'OVER' }
    Write-Host ("total: {0} upstream files; +{1} -{2} = {3} lines" -f $rows.Count, $add, $del, $lines)
    Write-Host ("  [UP] commits: {0} lines; persistent surface: {1} lines (target <= {2}) {3}" -f $up, $persistent, $MaxLines, $ok)
}
finally { Pop-Location }
