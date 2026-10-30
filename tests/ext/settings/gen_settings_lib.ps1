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

# Runs the copied generator. Returns the exit code, each stream on its own and
# both combined: a refused rule writes its message to stderr and stops there,
# while a run that goes on writes to stdout (a later rule, the doc comparison,
# the pass line), so a check can tell which one spoke.
function Invoke-SettingsGen {
    param([string]$Fake, [switch]$Check, [string]$EmitEffective = '')
    $gen = Join-Path $Fake 'tools/gen-settings.ps1'
    $a = @()
    if ($EmitEffective -ne '') { $a += '-EmitEffective'; $a += $EmitEffective }
    if ($Check) { $a += '-Check' }
    $outFile = Join-Path $Fake 'gen-stdout.txt'
    $errFile = Join-Path $Fake 'gen-stderr.txt'
    & pwsh -NoProfile -File $gen @a 1>$outFile 2>$errFile
    $code = $LASTEXITCODE
    $out = if (Test-Path -LiteralPath $outFile) { [IO.File]::ReadAllText($outFile) } else { '' }
    $err = if (Test-Path -LiteralPath $errFile) { [IO.File]::ReadAllText($errFile) } else { '' }
    Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ Code = $code; Out = $out; Err = $err; Text = ($out + $err) }
}

function Assert-Settings {
    param([bool]$Ok, [string]$What, [string]$Detail = '')
    if (-not $Ok) { Write-Host "FAIL: $What $Detail"; $script:failures++ }
}

# A refused rule must be the one that refused: Fail writes its message to stderr
# as one line and exits 1 at once, so a message on stdout (a doc comparison or a
# later rule) or a second stderr line means the run went on past the broken rule
# and refused it for some other reason.
function Assert-SettingsRuleRefused {
    param($Result, [string]$What, [string]$Message)
    Assert-Settings ($Result.Code -eq 1) "${What}: -Check exits 1" "code $($Result.Code): $($Result.Text)"
    Assert-Settings ($Result.Out.Trim() -eq '') "${What}: the run stops at the rule (nothing on stdout)" $Result.Out
    $lines = @($Result.Err -split "`r?`n" | Where-Object { $_ -ne '' })
    Assert-Settings ($lines.Count -eq 1) "${What}: one failure line on stderr" ($lines -join ' / ')
    if ($lines.Count -ge 1) {
        Assert-Settings ($lines[0].Contains($Message)) "${What}: that line names the broken rule" $lines[0]
    }
}

# Refuses a broken scratch copy: exit 1, the rule's own message, nothing after it.
function Expect-SettingsReject {
    param([string]$Fake, [string]$What, [string]$Message)
    $r = Invoke-SettingsGen -Fake $Fake -Check
    Assert-SettingsRuleRefused $r $What $Message
}
