# Runner for the settings schema checks: every schema validation rule of the
# settings generator is broken once on a scratch copy, and -Check must refuse
# each broken copy with exit 1 and the broken rule's own message as the last
# thing it prints. A clean scratch copy must pass.
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

# Clean scratch copies pass: the schema, fragments and doc agree.
$c = New-Case
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 0) 'clean copy: -Check passes' "$($r.Code): $($r.Text)"
Assert-Settings ($r.Text.Contains('settings check passed')) 'clean copy: prints the pass line' $r.Text

# A missing required field is rejected: reading the absent property stops the
# run, and the message names the missing field.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').PSObject.Properties.Remove('description')
Save-SettingsSchema $c.Fake $c.Schema
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 1) 'missing field: -Check exits 1' "code $($r.Code): $($r.Text)"
Assert-Settings ($r.Text.Contains("'description'") -and $r.Text.Contains('cannot be found')) 'missing field: names the missing field' $r.Text

# Words outside the type/applies/flag vocabularies are rejected.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').type = 'number'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'bad type' "unknown type 'number'"

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').applies = 'now'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'bad applies' "unknown applies 'now'"

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').flags[0] = 'windowsOnly'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'bad flag' "unknown flag 'windowsOnly'"

# An enum without values, and an enum value that is not a string, are rejected.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'Mode').PSObject.Properties.Remove('values')
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'enum without values' "enum needs a non-empty 'values' list"

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'Mode').values[1] = ''
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'empty enum value' 'enum values must be non-empty strings'

# A range without min/max, a range running backwards, and a default outside
# the range are rejected.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').PSObject.Properties.Remove('min')
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'range without min' "needs 'min' and 'max'"

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').min = 5
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'min above max' 'is above max'

$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').defaultMeans = '9'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'default outside range' 'is outside min..max'

# A key listed twice is rejected.
$c = New-Case
$hud = Find-SettingsSection $c.Schema 'HudLess'
$clone = ((Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold') | ConvertTo-Json -Depth 20) | ConvertFrom-Json -Depth 20
$hud.keys = @($hud.keys) + @($clone)
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'duplicate key' 'duplicate key entry: [HudLess] MaskThreshold'

# The key string must read [Section] Name, and the entry must sit in its own
# section.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold').key = '[HudLess] MaskThresholdX'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'key string drift' "'key' must read '[HudLess] MaskThreshold'"

$c = New-Case
$moved = Find-SettingsEntry $c.Schema 'HudLess' 'MaskThreshold'
$moved.section = 'HDR'
$moved.key = '[HDR] MaskThreshold'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'entry in the wrong section' "does not match its section 'HudLess'"

# The default line must stay auto: every key defaults to today's behaviour.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'MaskDilation').default = 'off'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'default not auto' "'default' must be 'auto'"

# An enum default must name one of the values (or auto for keys the code
# leaves unset by default).
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'Mode').defaultMeans = '2 (unknown)'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'enum default naming no value' 'names no value of'

# A section must hold keys, the schema must hold sections, and the format
# line must read settings-schema.
$c = New-Case
(Find-SettingsSection $c.Schema 'Window').keys = @()
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'section without keys' 'has no keys'

$c = New-Case
$c.Schema.sections = @()
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'schema without sections' 'the schema has no sections'

$c = New-Case
$c.Schema.format = 'settings-schema-x'
Save-SettingsSchema $c.Fake $c.Schema
Expect-Reject $c.Fake 'wrong schema format' "expected 'settings-schema'"

if ($script:failures) { Write-Host "gen_settings_schema: $script:failures check(s) failed"; exit 1 }
Write-Host 'gen_settings_schema: every schema rule refused its broken copy, clean copies pass'
exit 0
