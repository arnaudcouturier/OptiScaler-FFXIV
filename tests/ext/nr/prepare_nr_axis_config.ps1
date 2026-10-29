# Writes <Out>\Config.h: the REAL CustomOptional template of OptiScaler\Config.h (cut out between `enum HasDefaultValue` and UnboundKey, so the
# test exercises the production class) plus the REAL Scaler enum (NrConfig.h declares a member of it), cut out of the same file.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$src = [IO.File]::ReadAllText((Join-Path $Repo 'OptiScaler\Config.h'))
$start = $src.IndexOf('enum HasDefaultValue')
$end = $src.IndexOf('constexpr inline int UnboundKey')
if ($start -lt 0 -or $end -le $start) { Write-Error 'CustomOptional markers not found in OptiScaler\Config.h'; exit 1 }
$slice = $src.Substring($start, $end - $start)
$scalerStart = $src.IndexOf('enum class Scaler')
$scalerEnd = if ($scalerStart -ge 0) { $src.IndexOf('};', $scalerStart) } else { -1 }
if ($scalerStart -lt 0 -or $scalerEnd -le $scalerStart) { Write-Error 'Scaler enum not found in OptiScaler\Config.h'; exit 1 }
$scaler = $src.Substring($scalerStart, $scalerEnd + 2 - $scalerStart)
if ($scaler -notmatch 'Lanczos3' -or $scaler -notmatch 'Count') { Write-Error 'Scaler enum slice is not the expected one'; exit 1 }
$head = "#pragma once`r`n#include <concepts>`r`n#include <cstdint>`r`n#include <optional>`r`n#include <string>`r`n#include <utility>`r`n"
[IO.File]::WriteAllText((Join-Path $Out 'Config.h'), $head + $slice + "constexpr inline int UnboundKey = -1;`r`n" + $scaler + "`r`n", [Text.Encoding]::ASCII)
