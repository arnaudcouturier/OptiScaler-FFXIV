# Writes <Out>\Config.h for update_check: the REAL CustomOptional template of OptiScaler\Config.h (cut out between its first
# line and NV_PRESET_LATEST, so the test exercises the production class, volatile values included, and UnboundKey comes
# with it) plus a one-member Config whose CheckForUpdate line is copied from the real Config.h, so a changed default there
# is seen by the test. SimpleIni comes from external\simpleini (on the include path of the recipe).
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$src = [IO.File]::ReadAllText((Join-Path $Repo 'OptiScaler\Config.h'))
$start = $src.IndexOf('enum HasDefaultValue')
$end = $src.IndexOf('constexpr uint32_t NV_PRESET_LATEST')
if ($start -lt 0 -or $end -le $start) { Write-Error 'CustomOptional markers not found in OptiScaler\Config.h'; exit 1 }
$slice = $src.Substring($start, $end - $start)
if ($slice -notmatch 'set_volatile_value' -or $slice -notmatch 'value_for_config' -or $slice -notmatch 'UnboundKey') { Write-Error 'slice lacks the volatile API or UnboundKey'; exit 1 }
$decl = [regex]::Match($src, '(?m)^[ \t]*CustomOptional<bool> CheckForUpdate \{[^\r\n]*;').Value.Trim()
if (-not $decl) { Write-Error 'the CheckForUpdate member was not found in OptiScaler\Config.h'; exit 1 }
$head = "#pragma once`r`n#include <SimpleIni.h>`r`n#include <concepts>`r`n#include <cstdint>`r`n#include <optional>`r`n#include <string>`r`n#include <utility>`r`n"
$rest = @"

class Config
{
  public:
    $decl

    // The process-wide instance, as Config::Instance() of the product; Fresh() starts a new "launch" with the default constructed.
    static Config* Instance() { return Slot(); }
    static void Fresh()
    {
        delete Slot();
        Slot() = new Config();
    }

  private:
    static Config*& Slot()
    {
        static Config* instance = new Config();
        return instance;
    }
};
"@
[IO.File]::WriteAllText((Join-Path $Out 'Config.h'), $head + $slice + $rest, [Text.Encoding]::ASCII)
