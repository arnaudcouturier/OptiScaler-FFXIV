# Runner for the doc and packaging checks: -Check passes on the committed tree
# and on scratch copies with CRLF or LF endings, fails exit 1 on every edited
# byte with the first differing lines on the console and no temp file left
# behind, works under a comma-decimal locale, and writes the effective schema
# the packager needs.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'gen_settings_lib.ps1')
$script:failures = 0

function New-Case {
    $fake = New-SettingsFake $Repo $Out
    $pristine = Get-PristineSchema $fake
    $schema = $pristine | ConvertFrom-Json -Depth 20
    return [pscustomobject]@{ Fake = $fake; Pristine = $pristine; Schema = $schema }
}

function Set-DocEndings {
    param([string]$Fake, [string]$Style)
    $doc = Join-Path $Fake 'docs/SETTINGS.md'
    $text = [IO.File]::ReadAllText($doc) -replace "`r`n", "`n"
    if ($Style -eq 'CRLF') { $text = $text -replace "`n", "`r`n" }
    [IO.File]::WriteAllText($doc, $text)
}

# The committed tree passes -Check untouched (read-only run): the doc on disk
# matches the schema, including with CRLF endings.
$rText = & pwsh -NoProfile -File (Join-Path $Repo 'tools/gen-settings.ps1') -Check 2>&1 | Out-String
Assert-Settings ($LASTEXITCODE -eq 0) 'committed tree: -Check passes' "${LASTEXITCODE}: $rText"
Assert-Settings ($rText.Contains('settings check passed')) 'committed tree: prints the pass line' $rText

# A clean scratch copy passes with CRLF endings and with LF endings: the
# comparison normalises both sides.
$c = New-Case
Set-DocEndings $c.Fake 'CRLF'
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 0) 'CRLF doc: -Check passes' "$($r.Code): $($r.Text)"

$c = New-Case
Set-DocEndings $c.Fake 'LF'
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 0) 'LF doc: -Check passes' "$($r.Code): $($r.Text)"

# A case-only drift still fails: the doc comparison is ordinal.
$c = New-Case
$doc = Join-Path $c.Fake 'docs/SETTINGS.md'
$lines = Get-Content -LiteralPath $doc
$hit = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '\| live \|') { $hit = $i; break }
}
Assert-Settings ($hit -ge 0) 'probe setup: an applies cell reads live' ''
$lines[$hit] = $lines[$hit] -replace '\| live \|', '| Live |'
$lines | Set-Content -LiteralPath $doc -Encoding utf8
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 1) 'case drift: -Check exits 1' "code $($r.Code)"
Assert-Settings ($r.Text.Contains("line $($hit + 1) committed:")) 'case drift: names the committed line' $r.Text
Assert-Settings ($r.Text.Contains("line $($hit + 1) generated:")) 'case drift: names the generated line' $r.Text

# An added line fails with exit code exactly 1, prints the first differing
# lines, and leaves no temp file behind.
$c = New-Case
$tmpDir = [IO.Path]::GetTempPath()
$before = @(Get-ChildItem -LiteralPath $tmpDir -File -Filter 'tmp*.tmp' | ForEach-Object { $_.Name })
Add-Content -LiteralPath (Join-Path $c.Fake 'docs/SETTINGS.md') -Value '<!-- probe -->' -Encoding utf8
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 1) 'added line: -Check exits exactly 1' "code $($r.Code): $($r.Text)"
Assert-Settings ($r.Text.Contains('First differing lines')) 'added line: prints the first differing lines' $r.Text
Assert-Settings ($r.Text.Contains('committed:') -and $r.Text.Contains('generated:')) 'added line: shows both sides' $r.Text
$after = @(Get-ChildItem -LiteralPath $tmpDir -File -Filter 'tmp*.tmp' | ForEach-Object { $_.Name })
$leaked = @(Compare-Object $before $after | Where-Object { $_.SideIndicator -eq '=>' } | ForEach-Object { $_.InputObject })
Assert-Settings ($leaked.Count -eq 0) 'added line: no temp file left behind' ($leaked -join ',')

# A deleted line and a reordered pair both fail.
$c = New-Case
$doc = Join-Path $c.Fake 'docs/SETTINGS.md'
$lines = Get-Content -LiteralPath $doc
$lines | Select-Object -Skip 2 | Set-Content -LiteralPath $doc -Encoding utf8
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 1) 'deleted line: -Check exits 1' "code $($r.Code)"

$c = New-Case
$doc = Join-Path $c.Fake 'docs/SETTINGS.md'
$lines = Get-Content -LiteralPath $doc
$swap = $lines[8]; $lines[8] = $lines[9]; $lines[9] = $swap
$lines | Set-Content -LiteralPath $doc -Encoding utf8
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 1) 'reordered lines: -Check exits 1' "code $($r.Code)"

# Reordered enum values fail as well: the set is the same, but the tables
# render values in schema order, so the committed doc no longer matches.
$c = New-Case
(Find-SettingsEntry $c.Schema 'HudLess' 'Mode').values = @('ui_free', 'off', 'recomposition')
Save-SettingsSchema $c.Fake $c.Schema
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 1) 'reordered values: -Check exits 1' "code $($r.Code)"

# A missing doc fails with a message saying it is missing.
$c = New-Case
Remove-Item -LiteralPath (Join-Path $c.Fake 'docs/SETTINGS.md')
$r = Invoke-SettingsGen -Fake $c.Fake -Check
Assert-Settings ($r.Code -eq 1) 'missing doc: -Check exits 1' "code $($r.Code)"
Assert-Settings ($r.Text.Contains('is missing')) 'missing doc: says the doc is missing' $r.Text

# Under a comma-decimal locale the tables still render dots: the generator
# pins the invariant culture, so ranges like 0.001 never come out as 0,001.
$c = New-Case
$gen = Join-Path $c.Fake 'tools/gen-settings.ps1'
$frText = & pwsh -NoProfile -Command "[cultureinfo]::CurrentCulture = [cultureinfo]::GetCultureInfo('fr-FR'); & '$gen' -Check" 2>&1 | Out-String
Assert-Settings ($LASTEXITCODE -eq 0) 'comma-decimal locale: -Check passes' "${LASTEXITCODE}: $frText"

# The effective schema carries the packager contract: the effective format,
# every tagged key, code facts per key, and an empty release until stamped.
$c = New-Case
$eff = Join-Path $Out 'effective.json'
$r = Invoke-SettingsGen -Fake $c.Fake -EmitEffective $eff
Assert-Settings ($r.Code -eq 0) 'effective schema: plain run writes it' "$($r.Code): $($r.Text)"
Assert-Settings (Test-Path -LiteralPath $eff) 'effective schema: file exists' ''
$j = Get-Content -Raw -LiteralPath $eff | ConvertFrom-Json -Depth 20
$wantKeys = 0
foreach ($s in @($c.Schema.sections)) { $wantKeys += @($s.keys).Count }
Assert-Settings ($j.format -ceq 'settings-schema-effective') 'effective schema: format' "$($j.format)"
Assert-Settings ($j.version -eq 1) 'effective schema: version' "$($j.version)"
Assert-Settings (@($j.keys).Count -eq $wantKeys) 'effective schema: every tagged key' "got $(@($j.keys).Count), want $wantKeys"
Assert-Settings ($j.release -ceq '') 'effective schema: release starts empty' "$($j.release)"
$probe = @($j.keys) | Where-Object { $_.key -ceq '[HudLess] MaskDilation' } | Select-Object -First 1
Assert-Settings ($null -ne $probe) 'effective schema: probe key present' ''
Assert-Settings ($probe.codeType -ceq 'int') 'effective schema: probe code type' "$($probe.codeType)"
Assert-Settings ("$($probe.codeDefault)" -ceq '1') 'effective schema: probe code default' "$($probe.codeDefault)"
Assert-Settings ($probe.hasDefault -eq $true) 'effective schema: probe has default' "$($probe.hasDefault)"
Assert-Settings ($probe.declaration -match 'HudlessConfig\.h$') 'effective schema: probe declaration' "$($probe.declaration)"

# The packager runs the generator with -EmitEffective and -Check together:
# both outputs land in one green run.
$c = New-Case
$eff = Join-Path $Out 'effective-check.json'
$r = Invoke-SettingsGen -Fake $c.Fake -Check -EmitEffective $eff
Assert-Settings ($r.Code -eq 0) 'effective plus check: green run' "$($r.Code): $($r.Text)"
Assert-Settings (Test-Path -LiteralPath $eff) 'effective plus check: file written' ''

if ($script:failures) { Write-Host "gen_settings_doc: $script:failures check(s) failed"; exit 1 }
Write-Host 'gen_settings_doc: the doc matches on disk, drifts fail loud and clean, the effective schema holds'
exit 0
