<#
.SYNOPSIS
  Copies the build output to -Target and merges OptiScaler\ext\ini\*.ini fragments into OptiScaler.ini.
.DESCRIPTION
  Fragment file name (without .ini) = ini section name, e.g. ext\ini\DLSSG.ini goes into [DLSSG].
  A fragment holds only the key/comment lines (a leading "[Section]" line is tolerated and dropped).
  The keys are inserted at the end of the matching section (before the rule line that precedes the next
  section header); a missing section is created at the end of the file. The shipped file ends up with one
  header per section. The upstream OptiScaler.ini template is never edited in git.
  The forwarder DLL is copied next to the OptiScaler DLL if it exists in the build output.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Target,
    [string]$Source,
    [string]$ForwarderName = 'nvngx.dll_dlssnr.dll',
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $root "x64\$Configuration\a" }
if (-not (Test-Path $Source)) { Write-Error "Build output not found: $Source (run tools\build-locked.ps1 first)"; exit 1 }

New-Item -ItemType Directory -Force -Path $Target | Out-Null
Copy-Item -Path (Join-Path $Source '*') -Destination $Target -Recurse -Force
Write-Host "[package] copied $Source -> $Target"

# forwarder
$fwd = Get-ChildItem -Path (Join-Path $root "x64\$Configuration") -Filter $ForwarderName -File -ErrorAction SilentlyContinue | Select-Object -First 1
if ($fwd) {
    Copy-Item $fwd.FullName -Destination $Target -Force
    Write-Host "[package] forwarder $($fwd.Name)"
}
else {
    Write-Host "[package] no forwarder ($ForwarderName) in the build output, skipped"
}

# ini fragments
$iniPath = Join-Path $Target 'OptiScaler.ini'
$fragDir = Join-Path $root 'OptiScaler\ext\ini'
$frags = @()
if (Test-Path $fragDir) { $frags = @(Get-ChildItem $fragDir -Filter *.ini -File | Sort-Object Name) }
if ($frags.Count -eq 0) { Write-Host '[package] no ini fragments'; exit 0 }
if (-not (Test-Path $iniPath)) { Write-Error "OptiScaler.ini missing in $Target"; exit 1 }

$utf8 = New-Object System.Text.UTF8Encoding($false)
$raw = [System.IO.File]::ReadAllText($iniPath)
$nl = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }
$lines = [System.Collections.Generic.List[string]]::new()
foreach ($l in ($raw -split "\r?\n")) { $lines.Add($l) }
if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }

foreach ($f in $frags) {
    $section = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
    $body = [System.Collections.Generic.List[string]]::new()
    foreach ($l in ([System.IO.File]::ReadAllText($f.FullName) -split "\r?\n")) { $body.Add($l) }
    while ($body.Count -gt 0 -and $body[0].Trim() -eq "[$section]") { $body.RemoveAt(0) }
    while ($body.Count -gt 0 -and $body[$body.Count - 1].Trim() -eq '') { $body.RemoveAt($body.Count - 1) }
    if ($body.Count -eq 0) { continue }

    # locate the section header (case-insensitive, like SimpleIni)
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -ieq "[$section]") { $start = $i; break }
    }
    if ($start -lt 0) {
        $lines.Add('')
        $lines.Add('; -------------------------------------------------------')
        $lines.Add("[$section]")
        $lines.Add('; -------------------------------------------------------')
        foreach ($b in $body) { $lines.Add($b) }
        Write-Host "[package] [$section] created ($($body.Count) lines)"
        continue
    }

    # end of section = next header, minus the rule line and blank lines that precede it
    $next = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[[^\]]+\]\s*$') { $next = $i; break }
    }
    $ins = $next
    if ($next -lt $lines.Count -and $ins - 1 -gt $start -and $lines[$ins - 1] -match '^\s*;\s*-{5,}\s*$') { $ins-- }
    while ($ins - 1 -gt $start -and $lines[$ins - 1].Trim() -eq '') { $ins-- }
    $block = [System.Collections.Generic.List[string]]::new()
    $block.Add('')
    foreach ($b in $body) { $block.Add($b) }
    $lines.InsertRange($ins, $block)
    Write-Host "[package] [$section] merged ($($body.Count) lines)"
}

[System.IO.File]::WriteAllText($iniPath, (($lines -join $nl) + $nl), $utf8)
