#Requires -Version 7.0
<#
.SYNOPSIS
  Generates docs/SETTINGS.md and the effective settings schema from tools/settings-schema.json plus the C++ code.
.DESCRIPTION
  Code is the truth for existence, type and default. This script reads the fork's config
  declarations (CustomOptional<T> member { default } in OptiScaler/ext/**/*Config.h) and the
  binder lines (b.Add("Section", "Key", member) / b.AddEnum in OptiScaler/ext/**/*.cpp and the
  inline CharaConfig.h Bind), plus every Key= line of the ini fragments under OptiScaler/ext/ini/,
  and fails (-Check exit 1) when: a fork key exists in code or in a fragment but has no schema
  entry and is not on the explicit reasoned untaggedYet list; a schema entry no longer exists in
  code; a schema type or default differs from the declaration; an enum's values differ from the
  parser's set where that set is readable; or an upstream key of tools/schema-upstream-keys.txt
  disappeared from OptiScaler/Config.h.

  The JSON keeps only what code cannot provide: range, applies-when, menu location, description,
  flags (plus aliases, generated-family notes, retired keys and the untaggedYet list itself).
  -EmitEffective writes the effective schema (code facts + JSON metadata) for the packager.
  tools/package.ps1 calls it to put settings-schema.json into the package; tools/package-release.ps1
  stamps the release tag into that copy, puts it at the zip root (covered by SHA256SUMS.txt) and
  next to the zip as a release asset. The plan's ;@ fragment tag lines are not used in this tree
  (no fragment line starts with ;@), so the packager strips nothing: it only drops ;@ lines if any
  ever appear. -Check compares the on-disk docs/SETTINGS.md with the fresh generator output
  (it does not read the git blob) and fails when they differ.
.EXAMPLE
  pwsh -NoProfile -File tools/gen-settings.ps1
.EXAMPLE
  pwsh -NoProfile -File tools/gen-settings.ps1 -Check
.EXAMPLE
  pwsh -NoProfile -File tools/gen-settings.ps1 -EmitEffective C:\work\pkg\settings-schema.json
#>
[CmdletBinding()]
param([switch] $Check, [string] $EmitEffective)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# S3: the Type column is formatted with -f; pin the invariant culture so a
# comma-decimal locale (fr-FR) cannot write 0,001 into the player doc.
[cultureinfo]::CurrentCulture = [cultureinfo]::InvariantCulture

$repoRoot = Split-Path -Parent $PSScriptRoot
$schemaPath = Join-Path $PSScriptRoot 'settings-schema.json'
$upstreamListPath = Join-Path $PSScriptRoot 'schema-upstream-keys.txt'
$outPath = Join-Path $repoRoot 'docs/SETTINGS.md'

$validTypes = @('bool', 'int', 'float', 'enum', 'string')
$validApplies = @('live', 'restart', 'start')
$validFlags = @('ffxivOnly', 'experimental', 'diagnostic')

function Fail([string] $message) {
    [Console]::Error.WriteLine($message)
    exit 1
}

function FailDocs([string] $generated) {
    $tmp = [IO.Path]::GetTempFileName()
    try {
        Set-Content -NoNewline -LiteralPath $tmp -Value $generated -Encoding utf8
        Write-Output 'docs/SETTINGS.md differs from the generator output. First differing lines:'
        $oldLines = @(Get-Content -LiteralPath $outPath)
        $newLines = @($generated -split "`n")
        $count = [Math]::Max($oldLines.Count, $newLines.Count)
        $shown = 0
        for ($i = 0; $i -lt $count -and $shown -lt 10; $i++) {
            $o = if ($i -lt $oldLines.Count) { $oldLines[$i] } else { '<no line>' }
            $n = if ($i -lt $newLines.Count) { $newLines[$i] } else { '<no line>' }
            if ($o -cne $n) {
                Write-Output "line $($i + 1) committed: $o"
                Write-Output "line $($i + 1) generated: $n"
                $shown++
            }
        }
    } finally {
        Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
    }
    exit 1
}

function GetProp($obj, [string] $name) {
    $p = $obj.PSObject.Properties[$name]
    if ($null -eq $p) { return $null }
    return $p.Value
}

$schema = Get-Content -Raw -LiteralPath $schemaPath | ConvertFrom-Json -Depth 20
if ($schema.format -cne 'settings-schema') { Fail "schema format is '$($schema.format)', expected 'settings-schema'" }
if ($schema.version -ne 1) { Fail "schema version is '$($schema.version)', expected 1" }

# Declaration forms understood (reported verbatim in the run output; anything else is listed
# as unresolved, never silently skipped):
#   CustomOptional<bool> name { false };  CustomOptional<float> name { 1.0f };
#   CustomOptional<int|int32_t|uint32_t> name { 0 };  CustomOptional<std::string> name { "off" };
#   CustomOptional<std::wstring> name { L"..." };  CustomOptional<Scaler> name { Scaler::X };
#   CustomOptional<T, NoDefault> name;  CustomOptional<T, NoDefault> name {};  (no default: auto until set)
#   CustomOptional<float, NoDefault> a, b, c;  (comma member lists share one declaration)
#   Initializers may name a constant (kThresholdDefault, kGuardDefault, kXeFgMaxInterpolations,
#   UnboundKey): the value comes from the constexpr in the same header set. Members of
#   NrConfig::ExtraPass (Pass4..Pass30 generated keys) are families, not per-key entries.
# Binder forms understood: b.Add("Sec", "Key", member); b.Add("Sec", "Key", member, true);
#   b.AddEnum("Sec", "Key", member, Enum::Count); b.Add("Sec", "Key", Config::Instance()->X)
#   (upstream persistence: exempt from fork tagging); b.Add("Sec", add("Suffix"), pass.x)
#   (generated Pass4..Pass30 keys: families, exempt). Binder struct context is void X::Bind;
#   the inline template Bind of CharaConfig.h uses its struct context instead.
# Fragment forms: every `^Name=` line of OptiScaler/ext/ini/*.ini (a leading [Section] line is
#   tolerated and dropped by the packager; ;@ tag lines do not exist in this tree).
# Upstream forms: CustomOptional<...> Member { ... }; or CustomOptional<...> Member; in
#   OptiScaler/Config.h (member names of tools/schema-upstream-keys.txt).

$sectionList = GetProp $schema 'sections'
if ($null -eq $sectionList -or @($sectionList).Count -eq 0) { Fail 'the schema has no sections' }
$extDir = Join-Path $repoRoot 'OptiScaler/ext'

# ---- menu truth: fork cards (kSlots), upstream host cards, ini-only keys, card titles ----
$layoutText = [IO.File]::ReadAllText((Join-Path $repoRoot 'OptiScaler/ext/core/MenuLayout.h'))
$slotRegion = $layoutText.Substring($layoutText.IndexOf('kSlots[] = {'))
$slotRegion = $slotRegion.Substring(0, $slotRegion.IndexOf('};'))
$slotTab = @{}
foreach ($m in ([regex]::Matches($slotRegion, '\{\s*"(?<id>[^"]+)"\s*,\s*"(?<tab>[^"]+)"'))) {
    $slotTab[$m.Groups['id'].Value] = $m.Groups['tab'].Value
}
$manifestText = [IO.File]::ReadAllText((Join-Path $repoRoot 'OptiScaler/ext/core/MenuManifest.h'))
$iniOnlyRegion = $manifestText.Substring($manifestText.IndexOf('kIniOnlyKeys[] = {'))
$iniOnlyRegion = $iniOnlyRegion.Substring(0, $iniOnlyRegion.IndexOf('};'))
$iniOnlyKeys = @{}
foreach ($m in ([regex]::Matches($iniOnlyRegion, '"(?<full>\[[^\]]+\] [A-Za-z0-9_]+)"'))) {
    $iniOnlyKeys[$m.Groups['full'].Value] = $true
}
# Card titles players see: fork AutoRegister/Register triples plus the upstream boxes table
# (the fork XeFG extras are drawn into the upstream 'XeFG Settings' card).
$cardTitles = @{}
$upstreamTab = @{}
foreach ($ef in (Get-ChildItem -LiteralPath $extDir -Recurse -Include '*.cpp' -File)) {
    $etext = [IO.File]::ReadAllText($ef.FullName)
    foreach ($m in ([regex]::Matches($etext, '\{\s*"(?<id>[a-z0-9_]+)"\s*,\s*"(?<tab>[A-Za-z ]+)"\s*,\s*"(?<title>[^"]+)"'))) {
        $cid = $m.Groups['id'].Value
        if ($cardTitles.ContainsKey($cid) -and $cardTitles[$cid] -cne $m.Groups['title'].Value) {
            Fail "card '$cid' is registered with two titles: '$($cardTitles[$cid])' and '$($m.Groups['title'].Value)'"
        }
        $cardTitles[$cid] = $m.Groups['title'].Value
    }
}
$mcText = [IO.File]::ReadAllText((Join-Path $repoRoot 'OptiScaler/menu/menu_common.cpp'))
$boxRegion = $mcText.Substring($mcText.IndexOf('MenuBox boxes[]'))
foreach ($m in ([regex]::Matches($boxRegion, '\{\s*"(?<id>[a-z0-9_]+)"\s*,\s*"(?<tab>[A-Za-z ]+)"\s*,\s*"(?<title>[^"]+)"'))) {
    $cid = $m.Groups['id'].Value
    if (-not $cardTitles.ContainsKey($cid)) { $cardTitles[$cid] = $m.Groups['title'].Value }
    if (-not $upstreamTab.ContainsKey($cid)) { $upstreamTab[$cid] = $m.Groups['tab'].Value }
}

$sectionFragment = @{}
$fragmentTotals = @{}
$fragmentKeys = @{}  # "[Sec] Key" -> fragment rel
$fragmentNames = @{}  # fragment rel -> Name= set
$seen = @{}
$keyCount = 0
foreach ($section in @($sectionList)) {
    if ([string]::IsNullOrWhiteSpace($section.name)) { Fail 'a section has no name' }
    if ([string]::IsNullOrWhiteSpace($section.fragment)) { Fail "section $($section.name) has no fragment" }
    $sectionFragment[$section.name] = $section.fragment
    $shapeKeys = GetProp $section 'keys'
    if ($null -eq $shapeKeys -or @($shapeKeys).Count -eq 0) { Fail "section $($section.name) has no keys" }
    foreach ($key in @($shapeKeys)) {
        $keyCount++
        $where = "key #$keyCount ($($key.key))"
        foreach ($field in @('key', 'section', 'name', 'type', 'default', 'defaultMeans', 'applies', 'menu', 'description')) {
            if ($null -eq $key.$field -or ($key.$field -is [string] -and [string]::IsNullOrWhiteSpace($key.$field))) {
                Fail "$where is missing '$field'"
            }
        }
        if ($key.key -cne "[$($key.section)] $($key.name)") { Fail "${where}: 'key' must read '[$($key.section)] $($key.name)'" }
        if ($key.section -cne $section.name) { Fail "${where}: section '$($key.section)' does not match its section '$($section.name)'" }
        if ($seen.ContainsKey($key.key)) { Fail "duplicate key entry: $($key.key)" }
        $seen[$key.key] = $true
        if ($validTypes -cnotcontains $key.type) { Fail "${where}: unknown type '$($key.type)'" }
        if ($key.default -cne 'auto') { Fail "${where}: 'default' must be 'auto' (every key defaults to today's behaviour)" }
        if ($validApplies -cnotcontains $key.applies) { Fail "${where}: unknown applies '$($key.applies)'" }
        if ($key.type -eq 'enum') {
            $values = GetProp $key 'values'
            if ($null -eq $values -or @($values).Count -eq 0) { Fail "${where}: enum needs a non-empty 'values' list" }
            foreach ($v in @($values)) {
                if ($v -isnot [string] -or [string]::IsNullOrWhiteSpace($v)) { Fail "${where}: enum values must be non-empty strings" }
            }
        }
        if (($key.type -eq 'int' -or $key.type -eq 'float')) {
            $lo = GetProp $key 'min'
            $hi = GetProp $key 'max'
            if ($null -eq $lo -or $null -eq $hi) { Fail "${where}: $($key.type) needs 'min' and 'max'" }
            if (($lo -isnot [int] -and $lo -isnot [long] -and $lo -isnot [double]) -or ($hi -isnot [int] -and $hi -isnot [long] -and $hi -isnot [double])) { Fail "${where}: min/max must be numbers" }
            if ([double] $lo -gt [double] $hi) { Fail "${where}: min $lo is above max $hi" }
            if ("$($key.defaultMeans)" -match '^-?[0-9]+(\.[0-9]+)?$') {
                $dv = [double] "$($key.defaultMeans)"
                if ($dv -lt [double] $lo -or $dv -gt [double] $hi) { Fail "${where}: defaultMeans '$($key.defaultMeans)' is outside min..max ($lo..$hi)" }
            }
        }
        $menuTab = GetProp $key.menu 'tab'
        $menuCard = GetProp $key.menu 'card'
        $menuIniOnly = GetProp $key.menu 'iniOnly'
        $rawTab = $null
        $rawCard = $null
        if ($null -ne $key.menu) {
            $tp = $key.menu.PSObject.Properties['tab']
            if ($null -ne $tp) { $rawTab = $tp.Value }
            $cp = $key.menu.PSObject.Properties['card']
            if ($null -ne $cp) { $rawCard = $cp.Value }
        }
        if (($null -ne $rawTab -and $rawTab -eq '') -or ($null -ne $rawCard -and $rawCard -eq '')) { Fail "${where}: menu.tab/menu.card must not be empty" }
        $hasMenu = ($null -ne $menuTab -and $menuTab -ne '' -and $null -ne $menuCard -and $menuCard -ne '')
        $hasIniOnly = ($null -ne $menuIniOnly -and $menuIniOnly -ne '')
        if ($hasMenu -eq $hasIniOnly) { Fail "${where}: menu must give tab+card or iniOnly, not both/neither" }
        if ($hasMenu) {
            $wantTitle = GetProp $key.menu 'cardTitle'
            if ([string]::IsNullOrWhiteSpace($wantTitle)) { Fail "${where}: menu needs cardTitle (the card title players see)" }
            if ($slotTab.ContainsKey($menuCard)) {
                if ($slotTab[$menuCard] -cne $menuTab) { Fail "${where}: card '$menuCard' sits in tab '$($slotTab[$menuCard])', not '$menuTab' (MenuLayout.h kSlots)" }
            } elseif ($upstreamTab.ContainsKey($menuCard)) {
                if ($upstreamTab[$menuCard] -cne $menuTab) { Fail "${where}: card '$menuCard' sits in tab '$($upstreamTab[$menuCard])', not '$menuTab' (upstream menu boxes)" }
            } else {
                Fail "${where}: card '$menuCard' is in neither MenuLayout.h kSlots nor the upstream menu boxes"
            }
            if (-not $cardTitles.ContainsKey($menuCard)) { Fail "${where}: no registered title for card '$menuCard'" }
            if ($cardTitles[$menuCard] -cne $wantTitle) { Fail "${where}: cardTitle '$wantTitle' differs from the registered '$($cardTitles[$menuCard])'" }
        } else {
            if (-not $iniOnlyKeys.ContainsKey($key.key)) { Fail "${where}: ini-only key '$($key.key)' is not in kIniOnlyKeys (MenuManifest.h)" }
        }
        $flagList = GetProp $key 'flags'
        if ($null -ne $flagList) {
            foreach ($flag in @($flagList)) {
                if ($validFlags -cnotcontains $flag) { Fail "${where}: unknown flag '$flag'" }
            }
        }
    }
}

$untagged = @{}
$untaggedList = GetProp $schema 'untaggedYet'
if ($null -eq $untaggedList) { $untaggedList = @() }
foreach ($u in @($untaggedList)) {
    if ([string]::IsNullOrWhiteSpace($u.key) -or [string]::IsNullOrWhiteSpace($u.reason)) { Fail 'untaggedYet entries need key and reason' }
    if ($untagged.ContainsKey($u.key)) { Fail "duplicate untaggedYet entry: $($u.key)" }
    if ($seen.ContainsKey($u.key)) { Fail "untaggedYet entry is already tagged: $($u.key)" }
    $untagged[$u.key] = $u.reason
}

$aliasKeys = @{}
$aliasList = GetProp $schema 'aliases'
if ($null -eq $aliasList) { $aliasList = @() }
foreach ($a in @($aliasList)) {
    if ([string]::IsNullOrWhiteSpace($a.key)) { Fail 'an alias has no key' }
    if ([string]::IsNullOrWhiteSpace($a.canonical)) { Fail "alias $($a.key) has no canonical key" }
    if ([string]::IsNullOrWhiteSpace($a.note)) { Fail "alias $($a.key) has no note" }
    if (-not $seen.ContainsKey($a.canonical)) { Fail "alias $($a.key) points at '$($a.canonical)', which is not a tagged key" }
    if ($seen.ContainsKey($a.key)) { Fail "alias $($a.key) is also a tagged entry" }
    $aliasKeys[$a.key] = $true
}
$familyList = GetProp $schema 'families'
if ($null -eq $familyList) { $familyList = @() }
foreach ($f in @($familyList)) {
    if ([string]::IsNullOrWhiteSpace($f.pattern) -or [string]::IsNullOrWhiteSpace($f.note)) { Fail 'family entries need pattern and note' }
}
$retiredList = GetProp $schema 'retired'
if ($null -eq $retiredList) { $retiredList = @() }
foreach ($r in @($retiredList)) {
    if ([string]::IsNullOrWhiteSpace($r.key) -or [string]::IsNullOrWhiteSpace($r.note)) { Fail 'retired entries need key and note' }
    if ($r.key -notmatch '^\[[^\]]+\] [A-Za-z0-9_]+$') { Fail "retired key '$($r.key)' must read '[Section] Name'" }
    if ($seen.ContainsKey($r.key)) { Fail "retired key $($r.key) is still tagged" }
    if ($aliasKeys.ContainsKey($r.key)) { Fail "retired key $($r.key) is also an alias" }
}
# ---- code truth: declarations ----
$configHeaders = @(Get-ChildItem -LiteralPath $extDir -Recurse -Include '*Config.h' -File)
$decls = @()  # @{ member, type, noDefault, init, file, struct }
$constMap = @{}
foreach ($h in $configHeaders) {
    $text = [IO.File]::ReadAllText($h.FullName)
    $rel = $h.FullName.Substring($repoRoot.Length).TrimStart('\', '/') -replace '\\', '/'
    $curStruct = ''
    foreach ($ln in ($text -split "`r?`n")) {
        $code = ($ln -split '//', 2)[0]
        $sm = [regex]::Match($code, '(?:struct|class)\s+(?<s>[A-Za-z0-9_]+)')
        if ($sm.Success -and $code -notmatch ';\s*$') { $curStruct = $sm.Groups['s'].Value }
        $cm = [regex]::Match($code, 'constexpr\s+[A-Za-z0-9_:<>]+\s+(?<n>k[A-Za-z0-9_]+)\s*=\s*(?<v>[^;]+);')
        if ($cm.Success) {
            $constMap[$cm.Groups['n'].Value] = $cm.Groups['v'].Value.Trim()
        }
        $dm = [regex]::Match($code, 'CustomOptional\s*<\s*(?<t>[A-Za-z0-9_:]+)(?:\s*,\s*(?<nd>NoDefault))?\s*>\s+(?<mems>[A-Za-z0-9_]+(?:\s*,\s*[A-Za-z0-9_]+)*)\s*(?:\{\s*(?<init>[^}]*)\s*\})?\s*;')
        if ($dm.Success) {
            $t = $dm.Groups['t'].Value
            $nd = $dm.Groups['nd'].Success
            $init = if ($dm.Groups['init'].Success) { $dm.Groups['init'].Value.Trim() } else { $null }
            foreach ($m in ($dm.Groups['mems'].Value -split '\s*,\s*')) {
                $decls += [pscustomobject]@{ member = $m; type = $t; noDefault = $nd; init = $init; file = $rel; struct = $curStruct }
            }
        }
    }
}
$constMap['kXeFgMaxInterpolations'] = '7'
$constMap['UnboundKey'] = '-1'

function CodeTypeOf([string] $t) {
    $b = $t -replace '^std::', ''
    if ($b -eq 'bool') { return 'bool' }
    if ($b -eq 'float' -or $b -eq 'double') { return 'float' }
    if ($b -eq 'int' -or $b -eq 'int32_t' -or $b -eq 'uint32_t' -or $b -eq 'int64_t' -or $b -eq 'uint64_t') { return 'int' }
    if ($b -eq 'string' -or $b -eq 'wstring') { return 'string-or-enum' }
    if ($b -eq 'Scaler') { return 'int' }
    return "unknown:$t"
}

function ResolveDefault([string] $init) {
    if ($null -eq $init -or $init -eq '') { return $null }
    $s = $init.Trim()
    if ($s -eq '' -or $s -eq 'std::string()' -or $s -eq 'std::wstring()') { return '' }
    if ($constMap.ContainsKey($s)) { $s = $constMap[$s] }
    if ($s -match '^L?"(?<v>.*)"$') { return $Matches.v }
    $s = $s -replace 'f$', '' -replace 'F$', ''
    if ($s -match 'Scaler::(?<v>[A-Za-z0-9_]+)') {
        $order = @('FSR1', 'Bicubic', 'CatmullRom', 'Lanczos2', 'Lanczos3', 'Kaiser2', 'Kaiser3', 'Magic')
        $idx = [array]::IndexOf($order, $Matches.v)
        if ($idx -ge 0) { return "$idx" }
        return $null
    }
    if ($s -match '^(?<n>-?[0-9]+(\.[0-9]+)?)\s*$') { return $Matches.n }
    if ($s -eq 'true' -or $s -eq 'false') { return $s }
    return $null
}
# ---- code truth: binders ----
$codeKeys = @()  # @{ section, key, member, struct, file, upstream }
$generatedFamilies = @()
$binderFiles = @(Get-ChildItem -LiteralPath $extDir -Recurse -Include '*.cpp', '*.h' -File)
foreach ($f in $binderFiles) {
    $text = [IO.File]::ReadAllText($f.FullName)
    $rel = $f.FullName.Substring($repoRoot.Length).TrimStart('\', '/') -replace '\\', '/'
    $curBind = ''
    $curStruct = ''
    foreach ($ln in ($text -split "`r?`n")) {
        $code = ($ln -split '//', 2)[0]
        $bm = [regex]::Match($code, 'void\s+(?<s>[A-Za-z0-9_]+)::Bind\s*\(')
        if ($bm.Success) { $curBind = $bm.Groups['s'].Value }
        $sm2 = [regex]::Match($code, 'struct\s+(?<s>[A-Za-z0-9_]+)')
        if ($sm2.Success -and $code -notmatch ';\s*$') { $curStruct = $sm2.Groups['s'].Value }
        if ($code -notmatch 'b\.Add') { continue }
        $am = [regex]::Match($code, 'b\.Add(Enum)?\s*\(\s*"(?<sec>[^"]+)"\s*,\s*(?<rest>.*)$')
        if (-not $am.Success) { continue }
        $sec = $am.Groups['sec'].Value
        $rest = $am.Groups['rest'].Value.Trim()
        $ctx = if ($curBind -ne '') { $curBind } else { $curStruct }
        if ($rest -match '^add\s*\(') {
            $generatedFamilies += [pscustomobject]@{ section = $sec; file = $rel }
            continue
        }
        $km = [regex]::Match($rest, '^"(?<k>[^"]+)"\s*,\s*(?<m>.*)$')
        if (-not $km.Success) {
            $generatedFamilies += [pscustomobject]@{ section = $sec; file = $rel }
            continue
        }
        $k = $km.Groups['k'].Value
        $mrest = $km.Groups['m'].Value.Trim()
        $up = $mrest -match '^Config::Instance\(\)'
        $mm = [regex]::Match($mrest, '^(?:Config::Instance\(\)->)?(?<m>[A-Za-z0-9_]+)')
        $member = if ($mm.Success) { $mm.Groups['m'].Value } else { '' }
        $codeKeys += [pscustomobject]@{ section = $sec; key = $k; member = $member; struct = $ctx; file = $rel; upstream = $up }
    }
}

function FindDecl([string] $member, [string] $struct) {
    $cands = @($decls | Where-Object { $_.member -eq $member })
    if ($cands.Count -eq 0) { return $null }
    if ($cands.Count -eq 1) { return $cands[0] }
    $byStruct = @($cands | Where-Object { $_.struct -eq $struct })
    if ($byStruct.Count -eq 1) { return $byStruct[0] }
    if ($byStruct.Count -gt 1) { return $byStruct[0] }
    return $cands[0]
}

# ---- fragments ----
foreach ($section in @($sectionList)) {
    $fragmentPath = Join-Path $repoRoot ($section.fragment -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $fragmentPath)) { Fail "fragment missing: $($section.fragment)" }
    $lines = Get-Content -LiteralPath $fragmentPath
    $names = @{}
    $total = 0
    foreach ($line in $lines) {
        if ($line -cmatch '^[ \t]*(?<name>[A-Za-z0-9_]+)\s*=') {
            $names[$Matches.name] = $true
            $total++
            $fragmentKeys["[$($section.name)] $($Matches.name)"] = $section.fragment
        }
    }
    $fragmentTotals[$section.name] = $total
    $fragmentNames[$section.fragment] = $names
    $fragKeys = GetProp $section 'keys'
    if ($null -eq $fragKeys) { $fragKeys = @() }
    foreach ($key in @($fragKeys)) {
        if (-not $names.ContainsKey($key.name)) {
            Fail "$($key.key): not found as a Name= line in $($section.fragment)"
        }
    }
}
# Alias lines must exist as a line of the canonical key's fragment.
foreach ($a in @($aliasList)) {
    $canon = "$($a.canonical)"
    $am = [regex]::Match($canon, '^\[(?<sec>[^\]]+)\] (?<nm>.+)$')
    $aliasShort = ("$($a.key)" -replace '^\[[^\]]+\] ', '')
    if (-not $sectionFragment.ContainsKey($am.Groups['sec'].Value)) { Fail "alias $($a.key): section '$($am.Groups['sec'].Value)' has no fragment" }
    $fragRel = $sectionFragment[$am.Groups['sec'].Value]
    if (-not $fragmentNames[$fragRel].ContainsKey($aliasShort)) { Fail "alias $($a.key) has no line in $fragRel" }
}
# ---- upstream alarm ----
if (-not (Test-Path -LiteralPath $upstreamListPath)) { Fail "upstream key list missing: tools/schema-upstream-keys.txt" }
$upstreamMembers = @{}
foreach ($ln in (Get-Content -LiteralPath $upstreamListPath)) {
    $t = $ln.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    $m = ([regex]::Match($t, '^(?<m>[A-Za-z0-9_]+)')).Groups['m'].Value
    if ($m -eq '') { Fail "bad upstream list line: $ln" }
    $upstreamMembers[$m] = $true
}
$upstreamConfigPath = Join-Path $repoRoot 'OptiScaler/Config.h'
$upstreamText = [IO.File]::ReadAllText($upstreamConfigPath)
foreach ($m in $upstreamMembers.Keys) {
    if ($upstreamText -notmatch "\b$([regex]::Escape($m))\b") {
        Fail "upstream key alarm: '$m' (tools/schema-upstream-keys.txt) no longer exists in OptiScaler/Config.h"
    }
}

# ---- enum parser sets (readable sources) ----
$enumSets = @{
    '[HudLess] Mode'           = @{ values = @('off', 'recomposition', 'ui_free'); source = 'ext/hudless/HudlessMode.h ParseMode (alias uifree accepted, not canonical)' }
    '[HudLess] GuardMode'      = @{ values = @('legacy', 'density'); source = 'ext/hudless/HudlessMode.h ParseGuardPolicy' }
    '[HudLess] HdrPaste'       = @{ values = @('off', 'on'); source = 'ext/hudless/HudlessMode.h ParseHdrPaste' }
    '[HudLess] AnchorRule'     = @{ values = @('r1b', 'early'); source = 'ext/hudless/HudlessMode.h ParseAnchorRule' }
    '[HudLess] ReShadePresets' = @{ values = @('auto', 'on', 'off'); source = 'ext/reshade/ReShadeBoundaryCore.cpp ParsePresetsMode' }
    '[Window] Mode'            = @{ values = @('off', 'probe', 'on'); source = 'ext/window/WindowMode.h ParseMode' }
    '[Menu] Style'             = @{ values = @('classic', 'ember'); source = 'ext/menu/MenuStyle.h Parse' }
}

# ---- code-truth checks ----
$unresolved = [System.Collections.Generic.List[string]]::new()
$covered = @{}

# 1. every schema entry must exist in code; type/default/enum must match the declaration
$schemaByKey = @{}
foreach ($section in @($sectionList)) {
    $mapKeys = GetProp $section 'keys'
    if ($null -eq $mapKeys) { $mapKeys = @() }
    foreach ($key in @($mapKeys)) { $schemaByKey[$key.key] = $key }
}
foreach ($full in $schemaByKey.Keys) {
    $hit = @($codeKeys | Where-Object { "[$($_.section)] $($_.key)" -eq $full -and -not $_.upstream })
    if ($hit.Count -eq 0) { Fail "schema entry $full no longer exists in code (no b.Add for it under OptiScaler/ext)" }
    $b = $hit[0]
    $d = FindDecl $b.member $b.struct
    if ($null -eq $d) { $unresolved.Add("${full}: declaration of member '$($b.member)' not resolved (binder $($b.file))"); continue }
    $covered[$full] = $true
    $entry = $schemaByKey[$full]
    $ct = CodeTypeOf $d.type
    if ($ct -like 'unknown:*') { $unresolved.Add("${full}: unknown declaration type '$($d.type)' ($($d.file))"); continue }
    $st = $entry.type
    $typeOk = ($ct -eq $st) -or ($ct -eq 'string-or-enum' -and ($st -eq 'enum' -or $st -eq 'string')) -or ($ct -eq 'int' -and ($st -eq 'int' -or $st -eq 'enum')) -or ($ct -eq 'bool' -and $st -eq 'enum')
    if (-not $typeOk) { Fail "$full`: schema type '$st' differs from declaration '$($d.type) $($d.member)' ($($d.file))" }
    if ($st -eq 'enum') {
        $dmText = "$($entry.defaultMeans)"
        $leadToken = ($dmText -split '[\s(]')[0]
        $named = $false
        foreach ($vv in @($entry.values)) { if ("$vv" -ceq $leadToken) { $named = $true; break } }
        if (-not $named) {
            if (-not ($leadToken -ceq 'auto' -and $d.noDefault)) { Fail "$full`: defaultMeans '$($entry.defaultMeans)' names no value of ($($entry.values -join ',')) and the declaration has a default" }
        }
    }
    if (-not $d.noDefault) {
        $rv = ResolveDefault $d.init
        if ($null -eq $rv) { $unresolved.Add("$full`: default '$($d.init)' not resolved ($($d.file))") }
        else {
            $dm = "$($entry.defaultMeans)".ToLowerInvariant()
            $need = "$rv".ToLowerInvariant()
            $ok = $false
            if ($rv -eq '') { $ok = $true }
            elseif ($dm -match [regex]::Escape($need)) { $ok = $true }
            else {
                $numNeed = 0
                if ([double]::TryParse($need, [ref] $numNeed)) {
                    foreach ($mm in ([regex]::Matches($dm, '-?[0-9]+(\.[0-9]+)?'))) {
                        $v = 0
                        if ([double]::TryParse($mm.Value, [ref] $v) -and [Math]::Abs($v - $numNeed) -lt 1e-6) { $ok = $true; break }
                    }
                }
            }
            if (-not $ok) {
                Fail "$full`: schema defaultMeans '$($entry.defaultMeans)' does not name the declaration default '$rv' ($($d.file))"
            }
        }
    }
    if ($enumSets.ContainsKey($full)) {
        if ($st -ne 'enum') { Fail "$full`: parser set is readable, schema type must be enum" }
        $want = @($enumSets[$full]['values'])
        $got = @($entry.values)
        if ((@($want | Sort-Object) -join '|') -ne (@($got | Sort-Object) -join '|')) {
            Fail "$full`: schema values ($($got -join ',')) differ from parser set ($($want -join ','), $($enumSets[$full]['source']))"
        }
    }
}

# 2. every fork key in code must be tagged, aliased or explicitly untagged-yet
foreach ($b in $codeKeys) {
    if ($b.upstream) { continue }
    $full = "[$($b.section)] $($b.key)"
    if ($covered.ContainsKey($full) -or $aliasKeys.ContainsKey($full) -or $untagged.ContainsKey($full)) { continue }
    Fail "fork key $full (binder $($b.file), member $($b.member)) has no schema entry and is not on the untaggedYet list"
}

# 3. every fragment Key= line must be tagged, aliased, untagged-yet or documented upstream persistence
foreach ($full in $fragmentKeys.Keys) {
    if ($seen.ContainsKey($full) -or $aliasKeys.ContainsKey($full) -or $untagged.ContainsKey($full)) { continue }
    if ($full -eq '[LowLatency] Input' -or $full -eq '[LowLatency] Output') { continue }
    Fail "fragment key $full ($($fragmentKeys[$full])) has no schema entry and is not on the untaggedYet list"
}

# 4. untaggedYet entries must still exist (in code or in a fragment); stale ones fail
foreach ($full in $untagged.Keys) {
    $inCode = @($codeKeys | Where-Object { "[$($_.section)] $($_.key)" -eq $full }).Count -gt 0
    $inFrag = $fragmentKeys.ContainsKey($full)
    if (-not $inCode -and -not $inFrag) { Fail "untaggedYet entry $full no longer exists in code or in a fragment (stale)" }
}

Write-Host "code-truth check passed: $($codeKeys.Count) binder keys, $keyCount tagged, $($untagged.Count) untagged-yet, $($aliasKeys.Count) aliases."
if ($unresolved.Count -gt 0) {
    Write-Host "unresolved declarations (listed, not skipped):"
    foreach ($u in ($unresolved | Sort-Object | Get-Unique)) { Write-Host "  $u" }
} else {
    Write-Host "all declarations resolved."
}
function Cell([string] $text) { return ($text -replace '\|', '/') -replace "`r?`n", ' ' }

function TypeText($key) {
    if ($key.type -eq 'enum') {
        $quoted = @()
        foreach ($v in $key.values) {
            if ("$v" -match '^-?[0-9]+(\.[0-9]+)?$') { $quoted += "$v" }
            else { $quoted += '"{0}"' -f $v }
        }
        return 'one of: ' + ($quoted -join ', ')
    }
    if ($key.type -eq 'int') { return 'integer {0}..{1}' -f $key.min, $key.max }
    if ($key.type -eq 'float') { return 'number {0}..{1}' -f $key.min, $key.max }
    if ($key.type -eq 'bool') { return 'true or false' }
    return $key.type
}

function MenuText($key) {
    $iniOnly = GetProp $key.menu 'iniOnly'
    if ($null -ne $iniOnly -and $iniOnly -ne '') { return 'ini only: ' + $iniOnly }
    $title = GetProp $key.menu 'cardTitle'
    if ([string]::IsNullOrWhiteSpace($title)) { $title = $key.menu.card }
    return "$($key.menu.tab) tab, $title card"
}

$lines = @()
$lines += '# Settings reference'
$lines += ''
$lines += 'Generated by `tools/gen-settings.ps1` from `tools/settings-schema.json` checked against the C++ config (schema v1). Do not edit by hand: change the schema and re-run the generator.'
$lines += ''
$lines += "Tagged keys: $keyCount across $(@($sectionList).Count) sections."
$lines += ''
$lines += 'How to read this: the keys go in the game folder''s `OptiScaler.ini` under the `[Section]` heading; `auto` means "use the built-in default" (and for keys where `auto` is a real mode, the table says what it does); write a value to override.'
$lines += ''
$lines += 'Applies: live = the menu change applies at once; restart = needs a game restart; start = read once at start.'
$lines += ''
foreach ($section in @($sectionList)) {
    $lines += "## [$($section.name)]"
    $lines += ''
    $lines += '| Key | Type | Default | Applies | Menu | Description |'
    $lines += '|---|---|---|---|---|---|'
    $tabKeys = GetProp $section 'keys'
    if ($null -eq $tabKeys) { $tabKeys = @() }
    foreach ($key in @($tabKeys)) {
        $flagText = ''
        $flagList = GetProp $key 'flags'
        if ($null -ne $flagList -and @($flagList).Count -gt 0) { $flagText = ' *(' + ((@($flagList)) -join ', ') + ')*' }
        $defText = "$($key.defaultMeans)"
        $paren = $defText.IndexOf(' (')
        if ($paren -ge 0) { $defText = $defText.Substring(0, $paren) }
        $lines += "| ``$($key.name)`` | $(Cell (TypeText $key)) | ``$(Cell $defText)`` | $($key.applies) | $(Cell (MenuText $key)) | $(Cell $key.description)$flagText |"
    }
    $lines += ''
}

if ($untagged.Count -gt 0) {
    $lines += '## Not yet tagged (explicit list, each with a reason)'
    $lines += ''
    foreach ($full in ($untagged.Keys | Sort-Object)) {
        $lines += "- ``$full``: $($untagged[$full])"
    }
    $lines += ''
}

if (@($aliasList).Count -gt 0) {
    $lines += '## Aliases (another name for a tagged key)'
    $lines += ''
    $lines += '| Alias | Canonical key | Note |'
    $lines += '|---|---|---|'
    foreach ($alias in @($aliasList)) {
        $lines += "| ``$($alias.key)`` | ``$($alias.canonical)`` | $(Cell $alias.note) |"
    }
    $lines += ''
}

if (@($familyList).Count -gt 0) {
    $lines += '## Generated families (not one entry per key yet)'
    $lines += ''
    foreach ($family in @($familyList)) {
        $lines += "- ``$($family.pattern)``: $($family.note)"
    }
    $lines += ''
}

if (@($retiredList).Count -gt 0) {
    $lines += '## Retired keys'
    $lines += ''
    foreach ($old in @($retiredList)) {
        $lines += "- ``$($old.key)``: $($old.note)"
    }
    $lines += ''
} else {
    $lines += '## Retired keys'
    $lines += ''
    $lines += 'None.'
    $lines += ''
}

$lines += '## Maintainer notes'
$lines += ''
$lines += 'Coverage counts tagged keys against every `Name=` line of the matching ini fragment (aliases cover their fragment line; `[LowLatency]` lines persist upstream keys and are not fork keys).'
$lines += ''
$lines += '| Section | Tagged | Fragment lines | Fragment |'
$lines += '|---|---|---|---|'
foreach ($section in @($sectionList)) {
    $lines += "| ``[$($section.name)]`` | $(@(GetProp $section 'keys').Count) | $($fragmentTotals[$section.name]) | ``$($section.fragment)`` |"
}
$lines += ''
$text = ($lines -join "`n") + "`n"

# ---- effective schema (code facts + JSON metadata) for the packager ----
function Write-Effective([string] $dest) {
    $commit = ''
    try { $commit = (git -C $repoRoot rev-parse --short HEAD 2>$null | Select-Object -First 1).Trim() } catch { }
    $effKeys = @()
    foreach ($section in @($sectionList)) {
        $effKeys0 = GetProp $section 'keys'
        if ($null -eq $effKeys0) { $effKeys0 = @() }
        foreach ($key in @($effKeys0)) {
            $full = $key.key
            $hit = @($codeKeys | Where-Object { "[$($_.section)] $($_.key)" -eq $full }) | Select-Object -First 1
            $codeType = $null; $codeDefault = $null; $hasDefault = $null; $declFile = $null
            if ($null -ne $hit) {
                $d = FindDecl $hit.member $hit.struct
                if ($null -ne $d) {
                    $codeType = CodeTypeOf $d.type
                    $declFile = $d.file
                    $hasDefault = (-not $d.noDefault)
                    if ($hasDefault) { $codeDefault = ResolveDefault $d.init }
                }
            }
            $effKeys += [pscustomobject]@{
                key = $full; section = $key.section; name = $key.name
                codeType = $codeType; codeDefault = $codeDefault; hasDefault = $hasDefault; declaration = $declFile
                type = $key.type; default = $key.default; defaultMeans = $key.defaultMeans
                values = (GetProp $key 'values'); min = (GetProp $key 'min'); max = (GetProp $key 'max')
                applies = $key.applies; menu = $key.menu; description = $key.description; flags = (GetProp $key 'flags')
            }
        }
    }
    $eff = [pscustomobject]@{
        format = 'settings-schema-effective'; version = 1
        generatedFrom = [pscustomobject]@{ source = 'tools/settings-schema.json'; commit = $commit }
        release = ''
        keys = $effKeys
        untaggedYet = @($untaggedList); aliases = @($aliasList); families = @($familyList); retired = @($retiredList)
    }
    $json = $eff | ConvertTo-Json -Depth 20
    $dir = Split-Path -Parent $dest
    if ($dir -ne '' -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -LiteralPath $dir | Out-Null }
    Set-Content -NoNewline -LiteralPath $dest -Value ($json + "`n") -Encoding utf8
    Write-Output "wrote effective schema $dest ($($effKeys.Count) keys)."
}

if ($EmitEffective -ne '' -and $null -ne $EmitEffective) {
    Write-Effective $EmitEffective
    if (-not $Check) {
        Set-Content -NoNewline -LiteralPath $outPath -Value $text -Encoding utf8
        Write-Output "wrote docs/SETTINGS.md ($keyCount keys)."
    } else {
        if (-not (Test-Path -LiteralPath $outPath)) { Fail "docs/SETTINGS.md is missing: run tools/gen-settings.ps1 first" }
        $committed = Get-Content -Raw -LiteralPath $outPath
        if (($committed -replace "`r`n", "`n") -cne ($text -replace "`r`n", "`n")) { FailDocs $text }
        Write-Output "settings check passed: docs/SETTINGS.md matches the schema ($keyCount keys)."
    }
} elseif ($Check) {
    if (-not (Test-Path -LiteralPath $outPath)) { Fail "docs/SETTINGS.md is missing: run tools/gen-settings.ps1 first" }
    $committed = Get-Content -Raw -LiteralPath $outPath
    if (($committed -replace "`r`n", "`n") -cne ($text -replace "`r`n", "`n")) { FailDocs $text }
    Write-Output "settings check passed: docs/SETTINGS.md matches the schema ($keyCount keys)."
} else {
    Set-Content -NoNewline -LiteralPath $outPath -Value $text -Encoding utf8
    Write-Output "wrote docs/SETTINGS.md ($keyCount keys)."
}
