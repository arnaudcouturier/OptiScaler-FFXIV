# Writes <Out>\Config.h: the REAL CustomOptional template of OptiScaler\Config.h (cut out between its first line and
# UnboundKey, so the test exercises the production class, volatile values included) plus mfg_cfg_stubs\ConfigRest.h.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$src = [IO.File]::ReadAllText((Join-Path $Repo 'OptiScaler\Config.h'))
$start = $src.IndexOf('enum HasDefaultValue')
$end = $src.IndexOf('constexpr inline int UnboundKey')
if ($start -lt 0 -or $end -le $start) { Write-Error 'CustomOptional markers not found in OptiScaler\Config.h'; exit 1 }
$slice = $src.Substring($start, $end - $start)
if ($slice -notmatch 'set_volatile_value' -or $slice -notmatch 'value_for_config') { Write-Error 'slice lacks the volatile API'; exit 1 }
$rest = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'mfg_cfg_stubs\ConfigRest.h'))
$head = "#pragma once`r`n#include <concepts>`r`n#include <cstdint>`r`n#include <optional>`r`n#include <string>`r`n#include <utility>`r`n"
[IO.File]::WriteAllText((Join-Path $Out 'Config.h'), $head + $slice + $rest, [Text.Encoding]::ASCII)
