#Requires -Version 7.0
<#
.SYNOPSIS
  Reads one FG recorder session (tools/analyze-fg-recording.ps1).
.DESCRIPTION
  Pass the unsuffixed current filename; the analyzer reads its rotations in chronological order
  (.2, .1, then the base file) and reports per-provider present and sleep distributions, per-second
  windows, AutoMFG switches with the frame time around each, and NR rebuild hitches.
  A begin left open by a key-off is not unfinished; a head lost to rotation warns and exits 2.

  Exit codes: 0 = clean recording, 2 = records lost, malformed or unfinished (no session.end),
  3 = the recording is still open (a writer holds the file: quit the game or turn the key off, then
  analyze it), 1 = usage error. Counts describe submitted presents, not physical monitor scanout. A long
  present alone does not establish a cause: compare input gaps, sleep timings and warnings around the same
  timestamp.
.EXAMPLE
  pwsh -NoProfile -File tools/analyze-fg-recording.ps1 fg-recordings/fg-rec-20261030-021530-123-1234.jsonl
.EXAMPLE
  pwsh -NoProfile -File tools/analyze-fg-recording.ps1 fg-recordings/fg-rec-20261030-021530-123-1234.jsonl -Output build/fg-analysis.json
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string] $Recording,
    [string] $Output
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function FailUsage([string] $message) {
    [Console]::Error.WriteLine($message)
    exit 1
}

# A session the writer still holds open cannot be read with the strict share mode the reader uses (the writer
# shares read only): say so and use its own exit code instead of a stack trace.
function FailOpen([string] $name, [string] $message) {
    [Console]::Error.WriteLine("recording is still open: quit the game or turn the key off, then analyze it ($name): $message")
    exit 3
}

# A writer holding the session file shows up as a sharing (32) or lock (33) violation. Any other open failure (a
# missing file, a path that is a directory, a denied path) is a real error and keeps its own exit code instead
# of being reported as a session still open.
function Test-SharingViolation([System.Exception] $exception) {
    $hresult = $exception.GetBaseException().HResult
    return ($hresult -band 0xFFFF) -in 32, 33
}

function Open-Recording([string] $path, [string] $name) {
    try {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    }
    catch [System.IO.IOException], [System.UnauthorizedAccessException] {
        if (-not (Test-SharingViolation $_.Exception)) { throw }
        FailOpen $name $_.Exception.Message
    }
    return [System.IO.StreamReader]::new($stream)
}

if ([string]::IsNullOrWhiteSpace($Recording)) { FailUsage('usage: analyze-fg-recording.ps1 <session.jsonl> [-Output <file>]') }

# The recorder keeps base + .1 .. .8 (144 MiB, docs/notes/fg-recorder.md): the oldest rotation first, the base last, so the
# head of a long session is read before the newest lines and the sequence checks stay in order.
$files = @((8..1 | ForEach-Object { "$Recording.$_" }) + @($Recording) | Where-Object { Test-Path -LiteralPath $_ })
if ($files.Count -eq 0) { FailUsage("recording not found: $Recording") }

$providers = @{ '0' = 'none'; '1' = 'dlssg'; '2' = 'xefg'; '3' = 'fsr' }

function New-Distribution([double[]] $samples) {
    $sorted = @($samples | Sort-Object)
    $n = $sorted.Count
    $p = { param([double] $q) if ($n -eq 0) { 0.0 } else { $sorted[[Math]::Min($n - 1, [Math]::Max(0, [int][Math]::Ceiling($n * $q) - 1))] } }
    $mean = if ($n -eq 0) { 0.0 } else { ($sorted | Measure-Object -Average).Average }
    [ordered]@{
        count    = $n
        mean_ms  = [Math]::Round($mean, 3)
        p50_ms   = [Math]::Round((& $p 0.50), 3)
        p95_ms   = [Math]::Round((& $p 0.95), 3)
        p99_ms   = [Math]::Round((& $p 0.99), 3)
        maximum_ms = [Math]::Round((($sorted | Select-Object -Last 1) ?? 0.0), 3)
    }
}

$frequency = $null
$version = $null
$origin = $null
$firstSeq = $null
$lastSeq = $null
$dropped = 0
$parseErrors = @()
$events = @{}
$cleanShutdown = $false
$presentsByProv = @{}
$sleepsByProv = @{}
$hiddenByProv = @{}
$pending = @{}
$lastLineByTid = @{}
$unfinished = @()
$rowNo = 0
$seenFirst = $false
$headRotated = $false
$seconds = [ordered]@{}
$presentEnds = New-Object System.Collections.Generic.List[object]
$automfgLines = New-Object System.Collections.Generic.List[object]
$rebuilds = New-Object System.Collections.Generic.List[object]
$marks = New-Object System.Collections.Generic.List[object]
$switchEvents = New-Object System.Collections.Generic.List[object]
$warnings = New-Object System.Collections.Generic.List[object]

function Convert-Ticks([long] $ticks) {
    if ($null -eq $frequency -or $frequency -le 0) { return 0.0 }
    return [double] $ticks * 1000.0 / [double] $frequency
}

foreach ($file in $files) {
    $name = Split-Path -Leaf $file
    $lineNo = 0
    $reader = Open-Recording $file $name
    try {
    while ($null -ne ($raw = $reader.ReadLine())) {
        $lineNo++
        if ([string]::IsNullOrWhiteSpace($raw)) { continue }
        try { $parsed = $raw | ConvertFrom-Json -ErrorAction Stop }
        catch { $parseErrors += "${name}:${lineNo}: $($_.Exception.Message)"; continue }
        try {
        $rowNo++
        $row = @{}
        foreach ($pr in $parsed.PSObject.Properties) { $row[$pr.Name] = $pr.Value }
        $kind = $row['kind']
        if ($null -eq $kind) { $parseErrors += "${name}:${lineNo}: missing kind"; continue }
        if ($null -ne $row['tid']) { $lastLineByTid["$($row['tid'])"] = $rowNo }
        if (-not $seenFirst) { $seenFirst = $true; $headRotated = ($kind -ne 'session.begin') }
        $events[$kind] = ([int] ($events[$kind] ?? 0)) + 1
        if ($null -ne $row['seq']) {
            if ($null -eq $firstSeq) { $firstSeq = [long] $row['seq'] }
            $lastSeq = [long] $row['seq']
        }
        if ($null -ne $row['qpc_frequency']) { $frequency = [long] $row['qpc_frequency'] }
        if ($null -ne $row['version']) { $version = [string] $row['version'] }
        if ($kind -eq 'session.end') { $cleanShutdown = $true }
        elseif ($kind -eq 'records_dropped') { $dropped += [long] $row['count'] }
        $prov = "$($row['prov'] ?? 0)"
        $sec = $null
        if ($null -ne $row['qpc'] -and $null -ne $frequency -and $frequency -gt 0) {
            if ($null -eq $origin) { $origin = [long] $row['qpc'] }
            $sec = "$([Math]::Max(0, [long][Math]::Floor(([long] $row['qpc'] - $origin) / $frequency)))"
            if (-not $seconds.Contains($sec)) {
                $seconds[$sec] = [ordered]@{ presents = 0; max_present_ms = 0.0; max_sleep_ms = 0.0 }
            }
        }
        switch ($kind) {
            { $_ -in 'fg.present.end', 'out.present.end' } {
                $ms = Convert-Ticks ([long] $row['b'])
                if (-not $presentsByProv.ContainsKey($prov)) { $presentsByProv[$prov] = New-Object System.Collections.Generic.List[double] }
                $presentsByProv[$prov].Add($ms)
                $presentEnds.Add([pscustomobject]@{ qpc = [long] $row['qpc']; ms = $ms; hr = [long] $row['a']; prov = $prov; site = $kind })
                if ($null -ne $sec) {
                    $seconds[$sec].presents = $seconds[$sec].presents + 1
                    $seconds[$sec].max_present_ms = [Math]::Max($seconds[$sec].max_present_ms, $ms)
                }
                if ([long] $row['a'] -ne 0) {
                    $warnings.Add([pscustomobject]@{ seq = $row['seq']; qpc = $row['qpc']; prov = $prov; kind = $kind; hr = ('0x{0:X}' -f [uint32]([int64]$row['a'] -band 0xFFFFFFFFL)); ms = [Math]::Round($ms, 3) })
                }
                $key = "$($row['tid']):$($kind -replace '\.end$', '.begin')"
                $pending.Remove($key)
            }
            { $_ -in 'fg.present.begin', 'out.present.begin' } {
                $pending["$($row['tid']):$kind"] = [pscustomobject]@{ seq = $row['seq']; qpc = $row['qpc']; tid = $row['tid']; kind = $kind; line = $rowNo }
            }
            'sleep' {
                $ms = Convert-Ticks ([long] $row['a'])
                if (-not $sleepsByProv.ContainsKey($prov)) { $sleepsByProv[$prov] = New-Object System.Collections.Generic.List[double] }
                $sleepsByProv[$prov].Add($ms)
                if ($null -ne $sec) { $seconds[$sec].max_sleep_ms = [Math]::Max($seconds[$sec].max_sleep_ms, $ms) }
            }
            'hidden.present' {
                $ms = Convert-Ticks ([long] $row['b'])
                if (-not $hiddenByProv.ContainsKey($prov)) { $hiddenByProv[$prov] = New-Object System.Collections.Generic.List[double] }
                $hiddenByProv[$prov].Add($ms)
                if ([long] $row['a'] -ne 0) {
                    $warnings.Add([pscustomobject]@{ seq = $row['seq']; qpc = $row['qpc']; prov = $prov; kind = $kind; hr = ('0x{0:X}' -f [uint32]([int64]$row['a'] -band 0xFFFFFFFFL)); ms = [Math]::Round($ms, 3) })
                }
            }
            'automfg' {
                $automfgLines.Add([pscustomobject]@{ qpc = [long] $row['qpc']; warn = [long] $row['a']; text = [string] $row['msg'] })
            }
            'nr.rebuild' {
                $rebuilds.Add([pscustomobject]@{ qpc = [long] $row['qpc'] })
            }
            'mark' {
                # The user's "now" (one key, one record): the mark number the log line showed, and when it was.
                $t = $null
                if ($null -ne $sec) { $t = [long] $sec }
                $marks.Add([pscustomobject]@{ n = [long] $row['a']; qpc = [long] $row['qpc']; t_s = $t })
            }
            default {
                # The live output switch's milestones (ext/fg/outswitch): one row per record, with the targets as provider
                # names and the line the game log shows. The rows are the switch's own sequence; nothing is inferred.
                if ($kind -like 'switch.*') {
                    $t = $null
                    if ($null -ne $sec) { $t = [long] $sec }
                    $switchEvents.Add([pscustomobject]@{
                            qpc = [long] $row['qpc']; t_s = $t; kind = $kind
                            from = $providers["$($row['a'])"]; to = $providers["$($row['b'])"]; text = [string] $row['msg']
                        })
                }
            }
        }
        } catch { $parseErrors += "${name}:${lineNo}: $($_.Exception.Message)"; continue }
    }
    } finally { $reader.Dispose() }
}

foreach ($p in $pending.Values) {
    # A begin whose tid never appears again is a key-off orphan, not an unfinished span.
    $tidKey = "$($p.tid)"
    if ($null -eq $p.tid -or (($lastLineByTid[$tidKey] ?? $p.line) -gt $p.line)) {
        $unfinished += [pscustomobject]@{ seq = $p.seq; qpc = $p.qpc; tid = $p.tid; kind = $p.kind }
    }
}

if ($headRotated) {
    $warnings.Add([pscustomobject]@{ seq = $null; qpc = $null; prov = ''; kind = 'head_rotated'; hr = ''; ms = $null; text = 'the first line read is not session.begin: the session head was rotated away' })
}

$sortedEnds = @($presentEnds | Sort-Object -Property qpc)
$sortedQpcs = @($sortedEnds | ForEach-Object { $_.qpc })

function Find-EndIndex($qpc, [string] $side) {
    $lo = 0
    $hi = $sortedQpcs.Count - 1
    $found = -1
    if ($side -eq 'before') {
        while ($lo -le $hi) {
            $mid = ($lo + $hi) -shr 1
            if ($sortedQpcs[$mid] -lt $qpc) { $found = $mid; $lo = $mid + 1 } else { $hi = $mid - 1 }
        }
    } else {
        while ($lo -le $hi) {
            $mid = ($lo + $hi) -shr 1
            if ($sortedQpcs[$mid] -gt $qpc) { $found = $mid; $hi = $mid - 1 } else { $lo = $mid + 1 }
        }
    }
    return $found
}

function Find-NeighbourMs($qpc, [string] $side) {
    $i = Find-EndIndex $qpc $side
    if ($i -lt 0) { return $null }
    return [Math]::Round($sortedEnds[$i].ms, 3)
}

$switches = @()
foreach ($line in $automfgLines) {
    if ($line.text -match '^AutoMFG: (\d+)x -> (\d+)x') {
        $switches += [ordered]@{
            qpc            = $line.qpc
            text           = $line.text
            prev_present_ms = Find-NeighbourMs $line.qpc 'before'
            next_present_ms = Find-NeighbourMs $line.qpc 'after'
        }
    }
}

$hitches = @()
foreach ($r in $rebuilds) {
    $gap = $null
    $i = Find-EndIndex $r.qpc 'after'
    if ($i -ge 0 -and $null -ne $frequency -and $frequency -gt 0) {
        $gap = [Math]::Round(([double] ($sortedEnds[$i].qpc - $r.qpc)) * 1000.0 / [double] $frequency, 3)
    }
    $hitches += [ordered]@{ qpc = $r.qpc; next_present_gap_ms = $gap }
}

$byProv = [ordered]@{}
foreach ($id in ($presentsByProv.Keys + $sleepsByProv.Keys + $hiddenByProv.Keys | Sort-Object -Unique)) {
    $byProv[($providers[$id] ?? "prov$id")] = [ordered]@{
        presents = (New-Distribution ([double[]] ($presentsByProv[$id] ?? @())))
        sleeps   = (New-Distribution ([double[]] ($sleepsByProv[$id] ?? @())))
        hidden   = (New-Distribution ([double[]] ($hiddenByProv[$id] ?? @())))
    }
}

$result = [ordered]@{
    recording         = $Recording
    qpc_frequency     = $frequency
    version           = $version
    events            = $events
    dropped_records   = $dropped
    first_sequence    = $firstSeq
    last_sequence     = $lastSeq
    parse_errors      = $parseErrors
    by_provider       = $byProv
    seconds           = $seconds
    automfg_switches  = $switches
    switch_events     = $switchEvents.ToArray()
    rebuild_hitches   = $hitches
    marks             = $marks.ToArray()
    warnings          = $warnings
    unfinished        = $unfinished
    clean_shutdown    = $cleanShutdown
    head_rotated      = $headRotated
    note              = 'Long Present is not by itself a root cause. Compare warnings, input gaps, sleep timings and provider switch generations around the same QPC timestamp.'
}

$encoded = $result | ConvertTo-Json -Depth 6
if ($Output) { Set-Content -LiteralPath $Output -Value ($encoded + "`n") -Encoding utf8; Write-Output $Output }
else { Write-Output $encoded }

if ($dropped -gt 0 -or $parseErrors.Count -gt 0 -or $unfinished.Count -gt 0 -or $headRotated -or -not $cleanShutdown) { exit 2 }
