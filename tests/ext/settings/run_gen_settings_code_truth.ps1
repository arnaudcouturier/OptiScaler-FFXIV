# Runner for the code-truth checks: the C++ config is the truth for existence,
# type and default, so a scratch copy with a removed binder, a mistyped entry,
# a renamed default, a drifted parser set, a removed upstream key, untagged
# code or fragment keys, a missing or renamed fragment line, or a missing alias
# line must each fail -Check with exit 1 and the broken rule's own message as
# the last thing it prints. A schema edit followed by a plain regenerate must
# come back green.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'gen_settings_lib.ps1')
$script:failures = 0

function Expect-Reject {
    param([string]$Fake, [string]$What, [string]$Message)
    # The rule itself must refuse it: its message, and nothing printed after it.
    Expect-SettingsReject $Fake $What $Message
}

function New-Case {
    $fake = New-SettingsFake $Repo $Out
    $pristine = Get-PristineSchema $fake
    $schema = $pristine | ConvertFrom-Json -Depth 20
    return [pscustomobject]@{ Fake = $fake; Pristine = $pristine; Schema = $schema }
}

# A clean scratch copy passes, and the copy holds no git checkout: the check
# compares on-disk files, never a git blob.
$c = New-Case
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 0) 'clean copy: -Check passes' "$($r.Code): $($r.Text)"
Assert-Settings (-not (Test-Path -LiteralPath (Join-Path $c.Fake '.git'))) 'clean copy: scratch holds no git checkout' ''

# A schema entry whose binder line is gone no longer exists in code.
$c = New-Case
$bindFile = Join-Path $c.Fake 'OptiScaler/ext/core/ExtConfig.cpp'
$text = Get-Content -Raw -LiteralPath $bindFile
Assert-Settings ($text.Contains('b.Add("Window", "Mode", mode)')) 'probe setup: Window binder line present' ''
$text.Replace('b.Add("Window", "Mode", mode)', 'mode /* probe: no binder call on this line */') | Set-Content -NoNewline -LiteralPath $bindFile -Encoding utf8
Expect-Reject $c.Fake 'removed binder' 'no longer exists in code'

# A default the declaration does not name is rejected, even inside the range.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').defaultMeans = '2'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'default missing the declaration value' 'does not name the declaration default'

# A type the declaration does not allow is rejected.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').type = 'bool'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'type against the declaration' 'differs from declaration'

# Values outside the readable parser set are rejected.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'Mode').values[2] = 'uifree'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'parser set drift' 'differ from parser set'

# A listed upstream key that leaves the upstream config raises the alarm.
$c = New-Case
$config = Join-Path $c.Fake 'OptiScaler/Config.h'
(Get-Content -LiteralPath $config) | Where-Object { $_ -notmatch 'CustomOptional<int> LogLevel' } | Set-Content -LiteralPath $config -Encoding utf8
Expect-Reject $c.Fake 'removed upstream key' 'upstream key alarm'

# A fork key in code with no schema entry is rejected ...
$c = New-Case
$hudless = Find-SettingsSection $c.Schema 'HudLess'
$hudless.keys = @($hudless.keys | Where-Object { $_.name -cne 'MaskDilation' })
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'untagged code key' 'fork key [HudLess] MaskDilation'

# ... and so is a fragment line with no schema entry, alias or reason.
$c = New-Case
$ini = Join-Path $c.Fake 'OptiScaler/ext/ini/Window.ini'
Add-Content -LiteralPath $ini -Value 'ProbeKey=1' -Encoding utf8
Expect-Reject $c.Fake 'untagged fragment line' 'fragment key [Window] ProbeKey'

# A missing fragment and a renamed fragment key are rejected.
$c = New-Case
Remove-Item -LiteralPath (Join-Path $c.Fake 'OptiScaler/ext/ini/Window.ini')
Expect-Reject $c.Fake 'missing fragment' 'fragment missing'

$c = New-Case
$ini = Join-Path $c.Fake 'OptiScaler/ext/ini/Window.ini'
(Get-Content -LiteralPath $ini) -replace '^Mode=', 'ModeRenamed=' | Set-Content -LiteralPath $ini -Encoding utf8
Expect-Reject $c.Fake 'renamed fragment key' 'not found as a Name= line'

# The alias fragment line must exist in the canonical key's fragment.
$c = New-Case
$nr = Join-Path $c.Fake 'OptiScaler/ext/ini/DlssNr.ini'
(Get-Content -LiteralPath $nr) | Where-Object { $_ -notmatch '^PauseOnFocusLoss\s*=' } | Set-Content -LiteralPath $nr -Encoding utf8
Expect-Reject $c.Fake 'alias line missing' 'has no line in'

# Round trip: a schema edit, a plain regenerate, then -Check is green and the
# doc carries the edit.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').description += ' Probe sentence.'
Save-SettingsSchema $c.Fake $c.Schema
$r = Invoke-SettingsGen -Fake $c.Fake
Assert-Settings ($r.Code -eq 0) 'regenerate: plain run writes the doc' "$($r.Code): $($r.Text)"
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 0) 'regenerate: -Check is green after' "$($r.Code): $($r.Text)"
$doc = Get-Content -Raw -LiteralPath (Join-Path $c.Fake 'docs/SETTINGS.md')
Assert-Settings ($doc.Contains('Probe sentence.')) 'regenerate: doc carries the edit' ''

if ($script:failures) { Write-Host "gen_settings_code_truth: $script:failures check(s) failed"; exit 1 }
Write-Host 'gen_settings_code_truth: the code truth holds on every scratch copy, edits round-trip green'
exit 0
