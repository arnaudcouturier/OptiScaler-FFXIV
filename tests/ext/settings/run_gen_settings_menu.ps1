# Runner for the menu and list checks: every menu placement rule of the
# settings generator (tab+card or iniOnly, known card and title, ini-only
# keys listed with the others) and every list rule (aliases point at tagged
# keys, retired keys are gone, the not-yet-tagged list holds live gaps) is
# broken once on a scratch copy, and -Check must refuse each broken copy
# with exit 1 and a message naming the broken rule.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'gen_settings_lib.ps1')
$script:failures = 0

function Expect-Reject {
    param([string]$Fake, [string]$What, [string]$Message)
    $r = Invoke-SettingsGen -Fake $Fake -Check
    Assert-Settings ($r.Code -eq 1) "${What}: -Check exits 1" "code $($r.Code): $($r.Text)"
    Assert-Settings ($r.Text.Contains($Message)) "${What}: names the broken rule" $r.Text
}

function New-Case {
    $fake = New-SettingsFake $Repo $Out
    $pristine = Get-PristineSchema $fake
    $schema = $pristine | ConvertFrom-Json -Depth 20
    return [pscustomobject]@{ Fake = $fake; Pristine = $pristine; Schema = $schema }
}

# A clean scratch copy passes.
$c = New-Case
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 0) 'clean copy: -Check passes' "$($r.Code): $($r.Text)"

# A menu entry must give tab+card or iniOnly, never both and never neither,
# and neither part may be empty.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').menu | Add-Member -NotePropertyName 'iniOnly' -NotePropertyValue 'probe reason'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'menu with tab+card and iniOnly' 'menu must give tab+card or iniOnly, not both/neither'

$c = New-Case
(Find-SettingsEntry $c.Schema 'FramePacing' 'Trace').menu.PSObject.Properties.Remove('iniOnly')
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'menu with neither' 'menu must give tab+card or iniOnly, not both/neither'

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').menu.tab = ''
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'empty menu tab' 'menu.tab/menu.card must not be empty'

# Menu placement must name the card title players see, and the card must sit
# in a known tab: an unknown card and a wrong title are both rejected.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').menu.PSObject.Properties.Remove('cardTitle')
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'menu without cardTitle' 'menu needs cardTitle'

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').menu.cardTitle = 'Wrong Card'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'wrong cardTitle' 'differs from the registered'

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').menu.card = 'no_such_card'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'unknown card' 'is in neither MenuLayout.h kSlots nor the upstream menu boxes'

# An ini-only key must stay listed with the other ini-only keys.
$c = New-Case
$manifest = Join-Path $c.Fake 'OptiScaler/ext/core/MenuManifest.h'
(Get-Content -LiteralPath $manifest) | Where-Object { $_ -notmatch '\[FramePacing\] Trace' } | Set-Content -LiteralPath $manifest -Encoding utf8
Expect-Reject $c.Fake 'ini-only key outside the manifest' 'is not in kIniOnlyKeys'

# An alias must point at a tagged key, must not double as a tagged entry, and
# its fragment line must exist.
$c = New-Case
$c.Schema.aliases[0].canonical = '[DlssNr] NoSuchKey'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'alias at an untagged key' 'which is not a tagged key'

$c = New-Case
$c.Schema.aliases[0].key = '[HudLess] Mode'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'alias doubling as a tagged entry' 'is also a tagged entry'

# A retired key must be gone from the entries and from the aliases.
$c = New-Case
$c.Schema.retired = @($c.Schema.retired) + @([pscustomobject]@{ key = '[HudLess] Mode'; note = 'probe retired entry for a key that is still tagged' })
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'retired key still tagged' 'is still tagged'

$c = New-Case
$c.Schema.retired = @($c.Schema.retired) + @([pscustomobject]@{ key = '[DlssNr] PauseOnFocusLoss'; note = 'probe retired entry for a key that is also an alias' })
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'retired key also an alias' 'is also an alias'

# The not-yet-tagged list must hold live gaps only: stale entries and entries
# for tagged keys are rejected.
$c = New-Case
$c.Schema.untaggedYet = @([pscustomobject]@{ key = '[Nope] Missing'; reason = 'probe stale entry' })
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'stale untagged entry' 'no longer exists in code or in a fragment (stale)'

$c = New-Case
$c.Schema.untaggedYet = @([pscustomobject]@{ key = '[HudLess] Mode'; reason = 'probe entry for a tagged key' })
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'untagged entry for a tagged key' 'is already tagged'

if ($script:failures) { Write-Host "gen_settings_menu: $script:failures check(s) failed"; exit 1 }
Write-Host 'gen_settings_menu: every menu and list rule refused its broken copy, clean copies pass'
exit 0
