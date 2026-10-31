# Runner (tools\test.ps1 `Runner`): fixture sessions for
# tools/analyze-fg-recording.ps1. Twelve fixtures, all with qpc_frequency 1000
# (one tick is one millisecond, so the expected p50/p95 numbers are exact):
# clean (exit 0, with one switch, five live-output-switch milestones, a
# rebuild hitch, floor per-second windows
# and one warning; a switch verdict and a self-test line ride along and must
# NOT count as switches), cut (no session.end, exit 2), rotated (.2/.1/base,
# exit 0), dropped (a records_dropped line, exit 2), malformed (one bad line,
# exit 2), nul (one null line, exit 2), mistype (one mistyped field, exit 2),
# orphan (a begin with no end after a key-off, exit 0), rotcut (head rotated
# away, exit 2 with a head_rotated warning), live (a session the writer still
# holds open: exit 3 and its message, never a stack trace), asdir (the name
# exists but is a directory: a real open error, exit 1, never exit 3) and
# usage (no arguments, exit 1).
# The sharing-violation predicate is also read out of the analyzer and driven
# with the codes an open can produce, so only a share or lock violation (32,
# 33) counts as a session still open.
# A wrong exit code, a wrong percentile, a missed switch/hitch/warning or a
# missed head_rotated flag fails here.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$failures = 0
function Fail([string]$what) { Write-Output "FAIL: $what"; $script:failures++ }
function Pass([string]$what) { Write-Output "PASS: $what" }
$analyzer = Join-Path $Repo 'tools/analyze-fg-recording.ps1'

function Write-Fixture([string]$path, [string[]]$lines) {
    [IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"))
}

$script:seq = 0
function Row([long]$qpc, [string]$kind, [int]$prov, [long]$a, [long]$b, [string]$msg, [long]$frame) {
    $script:seq++
    return ('{"seq":' + $script:seq + ',"qpc":' + $qpc + ',"tid":7,"frame":' + $frame +
        ',"gen":0,"prov":' + $prov + ',"kind":"' + $kind + '","a":' + $a + ',"b":' + $b +
        ',"msg":"' + $msg + '"}')
}
function Begin-Line {
    return '{"schema":1,"kind":"session.begin","qpc_frequency":1000,"pid":42,"utc":"2026-01-01T00:00:00Z","version":"t","exe":"g.exe","build":"matched","summary":"s"}'
}
function End-Line { return '{"kind":"session.end"}' }

function Build-Clean {
    $script:seq = 0
    $lines = @(Begin-Line)
    for ($i = 1; $i -le 70; $i++) {
        $qb = 1000 + $i * 40
        $lines += Row $qb 'fg.present.begin' 1 0 0 '' $i
        $hr = if ($i -eq 70) { -1 } else { 0 }
        $lines += Row ($qb + $i) 'fg.present.end' 1 $hr $i '' $i
        if ($i -eq 30) {
            $lines += Row ($qb + 20) 'automfg' 1 1 0 'AutoMFG: 2x -> 3x (real 8.3 ms = 120 fps, presented 3, target 3, settled)' $i
            # A switch verdict and a self-test carry Nx -> Mx but are not switches (raw lines, no seq).
            $lines += '{"qpc":' + ($qb + 21) + ',"tid":7,"frame":' + $i + ',"gen":0,"prov":1,"kind":"automfg","a":1,"b":0,"msg":"AutoMFG: presented frames stuck at 1, switch 2x -> 3x; going back to 2x, automatic mode stopped"}'
            $lines += '{"qpc":' + ($qb + 22) + ',"tid":7,"frame":' + $i + ',"gen":0,"prov":1,"kind":"automfg","a":0,"b":0,"msg":"AutoMFG self-test: 2x -> 3x (back to the count in use before the test)"}'
        }
        if ($i -eq 50) { $lines += Row ($qb + 60) 'nr.rebuild' 1 0 0 '' $i }
        if ($i -eq 30) { $lines += Row ($qb + 25) 'mark' 1 1 0 '' $i } # the user's "now": one mark record
    }
    # The live output switch's milestones: one record per phase, targets as provider ids (3 = fsr, 2 = xefg).
    $lines += Row 2900 'switch.begin' 3 3 2 'begin gen=1 from=fsrfg to=xefg why=script' 70
    $lines += Row 2910 'switch.teardown' 3 3 1 'teardown old chain=0000000000000001 class=fsrfg lib=amd_fidelityfx_framegeneration_dx12.dll/1/65F1A2B3 model=M2 pinRelease=yes witness=alive ms=800' 70
    $lines += Row 2920 'switch.create' 3 3 2 'create ok chain=0000000000000002 hr=0x0 retries=0 ms=1500' 70
    $lines += Row 2930 'switch.commit' 3 3 2 'commit gen=1 hitchMs=2400 budgetLeft=7' 70
    $lines += Row 2940 'switch.probation' 2 2 2 'probation ok presents=20 ms=8000 fgActive=yes' 70
    $lines += End-Line
    return $lines
}

function Invoke-Analyzer([string]$rec, [string]$name) {
    $json = Join-Path $Out "$name.json"
    if (Test-Path $json) { Remove-Item $json }
    $null = pwsh -NoProfile -File $analyzer $rec -Output $json
    $code = $LASTEXITCODE # child process exit code: 0 clean, 1 usage, 2 findings
    # (no $null mapping needed: the child process always reports 0, 1 or 2)
    $parsed = $null
    if (Test-Path $json) { $parsed = Get-Content $json -Raw | ConvertFrom-Json }
    return [pscustomobject]@{ Code = $code; Json = $parsed }
}

$clean = Build-Clean
$cleanPath = Join-Path $Out 'clean.jsonl'
Write-Fixture $cleanPath $clean

$r = Invoke-Analyzer $cleanPath 'clean'
if ($r.Code -ne 0) { Fail "clean: exit $($r.Code), want 0" } else { Pass 'clean exits 0' }
if ($null -eq $r.Json) { Fail 'clean: no JSON report' }
else {
    $p = $r.Json.by_provider.dlssg.presents
    if ($p.count -ne 70) { Fail "clean: presents $($p.count), want 70" }
    if ($p.p50_ms -ne 35) { Fail "clean: p50 $($p.p50_ms), want 35" }
    if ($p.p95_ms -ne 67) { Fail "clean: p95 $($p.p95_ms), want 67" }
    if ($p.p99_ms -ne 70) { Fail "clean: p99 $($p.p99_ms), want 70" }
    if ($p.count -eq 70 -and $p.p50_ms -eq 35 -and $p.p95_ms -eq 67 -and $p.p99_ms -eq 70) {
        Pass 'clean reports p50 35 / p95 67 / p99 70 on the known fixture'
    }
    if ($r.Json.automfg_switches.Count -ne 1) { Fail "clean: switches $($r.Json.automfg_switches.Count), want 1 (verdict and self-test are not switches)" }
    else {
        $s = $r.Json.automfg_switches[0]
        if ($s.prev_present_ms -ne 29 -or $s.next_present_ms -ne 30) {
            Fail "clean: switch neighbours $($s.prev_present_ms)/$($s.next_present_ms), want 29/30"
        }
        else { Pass 'clean reports the switch with its frame times' }
    }
    if ($r.Json.rebuild_hitches.Count -ne 1) { Fail 'clean: hitch not reported' }
    elseif ($r.Json.rebuild_hitches[0].next_present_gap_ms -ne 31) {
        Fail "clean: hitch gap $($r.Json.rebuild_hitches[0].next_present_gap_ms), want 31"
    }
    else { Pass 'clean reports the rebuild hitch gap' }
    if ((@($r.Json.seconds.PSObject.Properties)).Count -ne 3) { Fail 'clean: want 3 floor per-second windows' }
    else { Pass 'clean reports the 3 per-second windows of the known fixture' }
    if ($r.Json.head_rotated -ne $false) { Fail 'clean: head_rotated must be false' }
    if ($r.Json.warnings.Count -ne 1) { Fail 'clean: want the one HRESULT warning' }
    if ($r.Json.clean_shutdown -ne $true) { Fail 'clean: not marked clean' }
    if ($r.Json.events.'fg.present.end' -ne 70) { Fail 'clean: event count off' }
    if ($r.Json.first_sequence -ne 1 -or $r.Json.last_sequence -ne 148) { Fail 'clean: sequence range off' }
    if ((@($r.Json.switch_events)).Count -ne 5) { Fail "clean: switch rows $((@($r.Json.switch_events)).Count), want 5" }
    else {
        $bad = @($r.Json.switch_events | Where-Object { $_.kind -notlike 'switch.*' -or $null -eq $_.from -or $null -eq $_.to -or [string]::IsNullOrEmpty($_.text) })
        if ($bad.Count) { Fail "clean: $($bad.Count) switch row(s) miss their kind, targets or text" }
        elseif ($r.Json.switch_events[0].from -ne 'fsr' -or $r.Json.switch_events[0].to -ne 'xefg') { Fail "clean: begin targets $($r.Json.switch_events[0].from)/$($r.Json.switch_events[0].to), want fsr/xefg" }
        elseif ($r.Json.switch_events[3].kind -ne 'switch.commit' -or $r.Json.switch_events[3].text -notlike 'commit gen=1 hitchMs=2400*') { Fail 'clean: the commit row lost its line' }
        else { Pass 'clean reports the five switch milestones with their targets and lines' }
    }
    if ($r.Json.dropped_records -ne 0) { Fail 'clean: dropped not zero' }
    if ((@($r.Json.marks)).Count -ne 1) { Fail "clean: marks $((@($r.Json.marks)).Count), want 1" }
    elseif ($r.Json.marks[0].n -ne 1) { Fail "clean: mark number $($r.Json.marks[0].n), want 1" }
    elseif ($r.Json.marks[0].t_s -ne 1) { Fail "clean: mark second $($r.Json.marks[0].t_s), want 1" }
    else { Pass 'clean reports the mark with its number and its second' }
}

$cutPath = Join-Path $Out 'cut.jsonl'
Write-Fixture $cutPath ($clean | Where-Object { $_ -ne (End-Line) })
$r = Invoke-Analyzer $cutPath 'cut'
if ($r.Code -ne 2) { Fail "cut: exit $($r.Code), want 2" } else { Pass 'cut exits 2' }
if ($r.Json.clean_shutdown -ne $false) { Fail 'cut: still marked clean' }

$rotBase = Join-Path $Out 'rot.jsonl'
Write-Fixture ($rotBase + '.3') ($clean[0..30]) # the oldest rotation of a nine-file session
Write-Fixture ($rotBase + '.2') ($clean[31..70])
Write-Fixture ($rotBase + '.1') ($clean[71..111])
Write-Fixture $rotBase ($clean[112..($clean.Count - 1)])
$r = Invoke-Analyzer $rotBase 'rot'
if ($r.Code -ne 0) { Fail "rotated: exit $($r.Code), want 0" } else { Pass 'rotated exits 0' }
if ($r.Json.by_provider.dlssg.presents.count -ne 70) { Fail 'rotated: presents not 70' }
if ($r.Json.first_sequence -ne 1 -or $r.Json.last_sequence -ne 148) { Fail 'rotated: sequence range off' }
if ($r.Json.head_rotated -ne $false) { Fail 'rotated: head_rotated must be false' }
else { Pass 'rotated keeps a complete head, .3 included' }
if ((@($r.Json.marks)).Count -ne 1) { Fail 'rotated: the mark of the .3 part was not read' }

$dropPath = Join-Path $Out 'dropped.jsonl'
$withDrop = @($clean[0..99]) + '{"kind":"records_dropped","count":5}' + @($clean[100..($clean.Count - 1)])
Write-Fixture $dropPath $withDrop
$r = Invoke-Analyzer $dropPath 'dropped'
if ($r.Code -ne 2) { Fail "dropped: exit $($r.Code), want 2" } else { Pass 'dropped exits 2' }
if ($r.Json.dropped_records -ne 5) { Fail "dropped: count $($r.Json.dropped_records), want 5" }

$badPath = Join-Path $Out 'malformed.jsonl'
$withBad = @($clean[0..49]) + 'this is not json {{{' + @($clean[50..($clean.Count - 1)])
Write-Fixture $badPath $withBad
$r = Invoke-Analyzer $badPath 'malformed'
if ($r.Code -ne 2) { Fail "malformed: exit $($r.Code), want 2" } else { Pass 'malformed exits 2' }
if ($r.Json.parse_errors.Count -ne 1) { Fail 'malformed: want exactly one parse error' }

$nulPath = Join-Path $Out 'nul.jsonl'
$withNul = @($clean[0..49]) + 'null' + @($clean[50..($clean.Count - 1)])
Write-Fixture $nulPath $withNul
$r = Invoke-Analyzer $nulPath 'nul'
if ($r.Code -ne 2) { Fail "nul: exit $($r.Code), want 2" } else { Pass 'nul exits 2' }
if ($null -eq $r.Json -or $r.Json.parse_errors.Count -lt 1) { Fail 'nul: want at least one parse error' }
else { Pass 'nul counts the null line as malformed' }

$mistypePath = Join-Path $Out 'mistype.jsonl'
$withMistype = @($clean[0..49]) + ($clean[50] -replace '"b":\d+', '"b":"abc"') + @($clean[51..($clean.Count - 1)])
Write-Fixture $mistypePath $withMistype
$r = Invoke-Analyzer $mistypePath 'mistype'
if ($r.Code -ne 2) { Fail "mistype: exit $($r.Code), want 2" } else { Pass 'mistype exits 2' }
if ($null -eq $r.Json -or $r.Json.parse_errors.Count -lt 1) { Fail 'mistype: want at least one parse error' }
else { Pass 'mistype counts the mistyped field as malformed' }

$orphanPath = Join-Path $Out 'orphan.jsonl'
$script:seq = 0
$orphan = @(Begin-Line)
for ($i = 1; $i -le 5; $i++) {
    $qb = 1000 + $i * 40
    $orphan += Row $qb 'fg.present.begin' 1 0 0 '' $i
    $orphan += Row ($qb + $i) 'fg.present.end' 1 0 $i '' $i
}
$orphan += Row 1300 'fg.present.begin' 1 0 0 '' 6 # key off: no end follows on this tid
$orphan += End-Line
Write-Fixture $orphanPath $orphan
$r = Invoke-Analyzer $orphanPath 'orphan'
if ($r.Code -ne 0) { Fail "orphan: exit $($r.Code), want 0" } else { Pass 'orphan exits 0' }
if ($null -eq $r.Json) { Fail 'orphan: no JSON report' }
elseif ((@($r.Json.unfinished)).Count -ne 0) { Fail 'orphan: the key-off orphan must not be unfinished' }
else { Pass 'orphan ignores the begin left open by the key-off' }

$rotcutBase = Join-Path $Out 'rotcut.jsonl'
$script:seq = 0
$head = @('{"kind":"continuation","schema":1,"qpc_frequency":1000}')
for ($i = 1; $i -le 10; $i++) {
    $qb = 1000 + $i * 40
    $head += Row $qb 'fg.present.begin' 1 0 0 '' $i
    $head += Row ($qb + $i) 'fg.present.end' 1 0 $i '' $i
}
$head += End-Line
Write-Fixture ($rotcutBase + '.1') $head # no .2: the session head was rotated away
Write-Fixture $rotcutBase $head
$r = Invoke-Analyzer $rotcutBase 'rotcut'
if ($r.Code -ne 2) { Fail "rotcut: exit $($r.Code), want 2" } else { Pass 'rotcut exits 2' }
if ($null -eq $r.Json -or $r.Json.head_rotated -ne $true) { Fail 'rotcut: head_rotated must be true' }
else { Pass 'rotcut flags the rotated-away head' }
if ($null -eq $r.Json -or @(($r.Json.warnings | Where-Object { $_.kind -eq 'head_rotated' })).Count -ne 1) {
    Fail 'rotcut: want one head_rotated warning'
}
else { Pass 'rotcut warns about the rotated-away head' }

$null = pwsh -NoProfile -File $analyzer
if ($LASTEXITCODE -ne 1) { Fail "usage: exit $LASTEXITCODE, want 1" } else { Pass 'usage exits 1' }

# L6: a session the writer still holds open. The writer shares read only, so the reader's
# strict share mode fails; the analyzer must say what to do and use its own exit code
# instead of a stack trace (which under $ErrorActionPreference='Stop' exits 1, the usage code).
$livePath = Join-Path $Out 'live.jsonl'
Write-Fixture $livePath $clean
$live = [IO.File]::Open($livePath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $err = Join-Path $Out 'live.err'
    $null = pwsh -NoProfile -File $analyzer $livePath 2> $err
    $code = $LASTEXITCODE
    if ($code -ne 3) { Fail "live: exit $code, want 3 (the recording is still open)" } else { Pass 'live exits 3' }
    $text = Get-Content $err -Raw
    if ($text -notmatch 'recording is still open: quit the game or turn the key off') {
        Fail "live: the message is missing: $text"
    }
    else { Pass 'live says the recording is still open' }
}
finally { $live.Dispose() }

# Only a sharing (32) or lock (33) violation means the writer still holds the file. A catch wide enough to take
# every IO failure reports a missing file, a path that is a directory or a denied path as a session still open
# and exits 3, which hides the real cause. The predicate is read out of the analyzer and driven with the codes
# an open can produce, so the decision itself is tested and not only the one case that reaches it.
$source = Get-Content $analyzer -Raw
$sourceLines = Get-Content $analyzer
$predicateStart = -1
for ($i = 0; $i -lt $sourceLines.Count; $i++) {
    if ($sourceLines[$i] -match '^function Test-SharingViolation') { $predicateStart = $i; break }
}
$predicateEnd = -1
if ($predicateStart -ge 0) {
    for ($i = $predicateStart + 1; $i -lt $sourceLines.Count; $i++) {
        if ($sourceLines[$i] -match '^\}') { $predicateEnd = $i; break }
    }
}
if ($predicateStart -lt 0 -or $predicateEnd -lt 0) {
    Fail 'the analyzer has no sharing-violation test: every open failure would be reported as a session still open'
}
else {
    Invoke-Expression (($sourceLines[$predicateStart..$predicateEnd]) -join "`n")
    # A Win32 HRESULT written as a hex literal arrives as its signed 32-bit value (0x80070020 = -2147024864).
    $share = [IO.IOException]::new('sharing violation', 0x80070020)
    $lock = [IO.IOException]::new('lock violation', 0x80070021)
    $missing = [IO.IOException]::new('missing file', 0x80070002)
    $denied = [IO.IOException]::new('denied path', 0x80070005)
    $mapped = [IO.IOException]::new('mapped file', 0x800704C8)
    $wrapped = [Exception]::new('invocation wrapper', $missing)
    if (Test-SharingViolation $share) { Pass 'a sharing violation is a session still open' }
    else { Fail 'a sharing violation is not recognised as a session still open' }
    if (Test-SharingViolation $lock) { Pass 'a lock violation is a session still open' }
    else { Fail 'a lock violation is not recognised as a session still open' }
    foreach ($other in @($missing, $denied, $mapped, $wrapped)) {
        if (Test-SharingViolation $other) {
            Fail "an open failure that is not a sharing violation is reported as a session still open: $($other.Message)"
        }
    }
    if ($source.Contains('if (-not (Test-SharingViolation $_.Exception)) { throw }')) {
        Pass 'the analyzer rethrows every open failure that is not a sharing violation'
    }
    else { Fail 'the analyzer does not rethrow an open failure that is not a sharing violation' }
}

# The recording name exists but is a directory: that cannot be read, and it is a real error, not a session
# still open. A catch wide enough to take every IO failure would answer exit 3 and hide the cause.
$asDir = Join-Path $Out 'asdir.jsonl'
if (Test-Path -LiteralPath $asDir) { Remove-Item -Recurse -Force $asDir }
New-Item -ItemType Directory -Path $asDir | Out-Null
$asDirErr = Join-Path $Out 'asdir.err'
$null = pwsh -NoProfile -File $analyzer $asDir 2> $asDirErr
$asDirCode = $LASTEXITCODE
$asDirText = Get-Content $asDirErr -Raw
if ($asDirCode -ne 1) { Fail "asdir: exit $asDirCode, want 1 (a real open error)" }
elseif ($asDirText -match 'recording is still open') { Fail 'asdir: an open error that is not a sharing violation is reported as a session still open' }
else { Pass 'asdir exits 1 and is not reported as a session still open' }

if ($failures -gt 0) { throw "$failures analyzer check(s) failed" }
Write-Output 'PASS: analyzer fixtures (clean, cut, rotated, dropped, malformed, nul, mistype, orphan, rotcut, live, asdir, usage)'
