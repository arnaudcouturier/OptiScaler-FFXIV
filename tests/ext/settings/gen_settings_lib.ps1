# Shared setup for the settings checks. Each check builds a scratch copy of the
# files the settings generator reads (schema, fragments, menu and config
# sources, committed doc), breaks exactly one rule in the copy, and runs the
# copied generator with -Check. The real checkout is never written to; every
# run works without git, so the checks also prove the comparison is against
# the on-disk doc and not a git blob.
function New-SettingsFake {
    param([string]$Repo, [string]$Out)
    $fake = Join-Path $Out 'fake'
    Remove-Item -LiteralPath $fake -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($d in @('tools', 'docs', 'OptiScaler/ext', 'OptiScaler/menu')) {
        $dst = Join-Path $fake $d
        New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
        Copy-Item -LiteralPath (Join-Path $Repo $d) -Destination (Split-Path $dst) -Recurse -Force
    }
    Copy-Item -LiteralPath (Join-Path $Repo 'OptiScaler/Config.h') -Destination (Join-Path $fake 'OptiScaler/Config.h') -Force
    return $fake
}

# The pristine schema text, so each check can start from the committed schema.
function Get-PristineSchema {
    param([string]$Fake)
    return Get-Content -Raw -LiteralPath (Join-Path $Fake 'tools/settings-schema.json')
}

function Read-SettingsSchema {
    param([string]$Fake)
    return Get-Content -Raw -LiteralPath (Join-Path $Fake 'tools/settings-schema.json') | ConvertFrom-Json -Depth 20
}

function Save-SettingsSchema {
    param([string]$Fake, $Schema)
    $Schema | ConvertTo-Json -Depth 20 | Set-Content -NoNewline -LiteralPath (Join-Path $Fake 'tools/settings-schema.json') -Encoding utf8
}

function Restore-SettingsSchema {
    param([string]$Fake, [string]$Pristine)
    Set-Content -NoNewline -LiteralPath (Join-Path $Fake 'tools/settings-schema.json') -Value $Pristine -Encoding utf8
}

function Find-SettingsEntry {
    param($Schema, [string]$Section, [string]$Name)
    foreach ($s in @($Schema.sections)) {
        if ($s.name -ceq $Section) {
            foreach ($k in @($s.keys)) {
                if ($k.name -ceq $Name) { return $k }
            }
        }
    }
    return $null
}

function Find-SettingsSection {
    param($Schema, [string]$Section)
    foreach ($s in @($Schema.sections)) {
        if ($s.name -ceq $Section) { return $s }
    }
    return $null
}

# Runs the copied generator. Returns an object with the exit code and the
# combined output, so checks can assert both the verdict and the message.
function Invoke-SettingsGen {
    param([string]$Fake, [switch]$Check, [string]$EmitEffective = '')
    $gen = Join-Path $Fake 'tools/gen-settings.ps1'
    $a = @()
    if ($EmitEffective -ne '') { $a += '-EmitEffective'; $a += $EmitEffective }
    if ($Check) { $a += '-Check' }
    $text = & pwsh -NoProfile -File $gen @a 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text }
}

function Assert-Settings {
    param([bool]$Ok, [string]$What, [string]$Detail = '')
    if (-not $Ok) { Write-Host "FAIL: $What $Detail"; $script:failures++ }
}
