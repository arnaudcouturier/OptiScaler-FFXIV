# Writes <Out>\StateSkipWindow.h: the REAL LoadLibrary skip-window code of OptiScaler\State.h, cut from `static UINT
# GetOwner()` to the end of the class, so the tests exercise the production functions and their state (no copy to drift).
# The product header cannot be compiled on its own (it pulls in the whole renderer), so the slice is wrapped in a class of
# its own with the four includes it needs.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$src = [IO.File]::ReadAllText((Join-Path $Repo 'OptiScaler\State.h'))
$start = $src.IndexOf('    static UINT GetOwner()')
$end = $src.IndexOf('    State() = default;')
if ($start -lt 0 -or $end -le $start) { Write-Error 'skip-window markers not found in OptiScaler\State.h'; exit 1 }
$slice = $src.Substring($start, $end - $start)
foreach ($needle in @('GetOwner', 'DisableChecks', 'EnableChecks', 'SkipDllName', 'SkipDllChecks', '_skipDllName', '_lastOwner')) {
    if ($slice -notmatch [regex]::Escape($needle)) { Write-Error "slice lacks $needle"; exit 1 }
}
$head = "#pragma once`r`n#include <windows.h> // UINT`r`n#include <atomic>`r`n#include <map>`r`n#include <string>`r`n#include <utility>`r`n`r`nstruct State`r`n{`r`n"
[IO.File]::WriteAllText((Join-Path $Out 'StateSkipWindow.h'), $head + $slice + "};`r`n", [Text.Encoding]::ASCII)
