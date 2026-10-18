<#
.SYNOPSIS
  Summarizes an NR ViT launch capture (ini [DlssNr] VitCapture=true) for profiling a new NR model file.
.DESCRIPTION
  Reads <game>\OptiScaler\nr-vit-capture\<date-time>\capture.txt (format 1, see OptiScaler\ext\nr\native\DlssNrVitCapture.h and
  docs\nr-vit-reuse-profile-runbook.md) and prints:
    (a) the launch-order signature of the cc_vit_* kernels of each captured evaluation, with its SHA-256 (names only, and names
        plus paramSize; grid is left out on purpose, it follows the render resolution);
    (b) per-kernel parameter sizes and launch geometry;
    (c) 8-byte parameter values that look like device pointers and appear in the params of more than one launch (likely shared
        buffers and sync counters), classified by whether they are used inside the Start..End run, at its boundary or outside;
    (d) checks against what the reuse filter (DlssNrVitReuse.h) and the original verified module expect, and a hypothesis
        check of the 72-byte FFN parameter layout the FP8+NVFP4 hybrid path hard-codes for the original module.
  (e) the fixed go/no-go rule of the NR reuse study (STUDY-nr-flicker.md, "Recommended design (O3)", Step A), applied per NR pass
      feature. Sections (c), (d) and (e) judge each feature on its own evaluations: the capture records the EVAL feature=<ordinal>
      of every evaluation, and with Passes = 2 the first three evaluations are pass 0, pass 1, pass 0, two features with their
      own buffers. New per-feature checks: end-stable (End slots 0/1 and Start slot 1 across the feature's evaluations),
      start-end-link (Start.p1 == End.p0 within one evaluation, information only), waiters-outside (FAIL when a launch outside the
      run that shares a LEAK pointer is a waiter or publisher by name), clear-before (cc_cb_clear launched before Start in every
      evaluation and its pointer is a LEAK pointer), clear-range (information), epoch (a 4-byte value in an Inner kernel's
      parameters that differs between two evaluations of the same feature), end-users (launches outside the run that carry End
      slot 1 or Start slot 1, before Start or after End, with kernel names), end-xfeature (the same values in another feature's
      evaluations). A kernel name is judged after removing a trailing precision suffix (_fp8, _nvfp4 ...), so
      cc_vit_1d_projection_wait_fp8 is a *_wait kernel.

  It is best effort. It cannot tell read from write, a counter from a data buffer, whether a kernel is safe to skip, or anything
  about what a kernel computes: that needs the kernel code (SASS) or a probe. The report ends with that list.

  Without a capture it can still read the plain log: -Log OptiScaler.log uses the "NR reuse unverified launch:" lines (names,
  parameter sizes and geometry of the first two evaluations, no parameter bytes), which is enough for (a), (b) and the geometry
  part of (d).
.PARAMETER Capture
  capture.txt, a capture run folder, or the nr-vit-capture folder (the newest run is used).
.PARAMETER Log
  An OptiScaler.log to read instead of a capture.
.PARAMETER Baseline
  Another capture or log whose cc_vit_* launch order is compared with this one (for example the previous model's).
.PARAMETER PointerMin
  Smallest 8-byte value considered a pointer (default 0x10000).
.PARAMETER PointerMax
  Exclusive upper bound (default 2^48).
.PARAMETER MaxRows
  Rows printed in the pointer table.
.PARAMETER ClusterMin
  A value at or above 2^32 only counts as a pointer ("ptr") when at least this many distinct values share its high dword
  (default 4): device pointers come in bulk, a packed width/height pair does not.
.PARAMETER ShowUnlikely
  Also list the "int?" values (in range but in no address cluster).
.PARAMETER AllKernels
  Include the non-ViT kernels in table (b).
.PARAMETER PassThru
  Also return the analysis as an object (Signature, Checks, ...).
.PARAMETER Strict
  Exit code 1 when a check FAILs.
#>
[CmdletBinding()]
param(
    [string]$Capture,
    [string]$Log,
    [string]$Baseline,
    [UInt64]$PointerMin = 0x10000,
    [UInt64]$PointerMax = 0x1000000000000,
    [int]$MaxRows = 60,
    [int]$ClusterMin = 4,
    [switch]$ShowUnlikely,
    [switch]$AllKernels,
    [switch]$PassThru,
    [switch]$Strict
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Get-Sha256Text([string]$text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    return ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))).ToLowerInvariant()
}

function ConvertFrom-Record([string]$line) {
    $parts = @($line.Split(' ', [StringSplitOptions]::RemoveEmptyEntries))
    $kv = @{}
    for ($i = 1; $i -lt $parts.Count; $i++) {
        $eq = $parts[$i].IndexOf('=')
        if ($eq -gt 0) { $kv[$parts[$i].Substring(0, $eq)] = $parts[$i].Substring($eq + 1) }
    }
    return [pscustomobject]@{ Tag = $parts[0]; Kv = $kv }
}

function ConvertTo-Dim([string]$text) {
    $p = $text.Split('x')
    return @([UInt32]$p[0], [UInt32]$p[1], [UInt32]$p[2])
}

function Get-QWords([string]$hex) {
    if (-not $hex -or $hex -eq '-') { return @() }
    $bytes = [Convert]::FromHexString($hex)
    $n = [int][math]::Floor($bytes.Length / 8)
    $out = [System.Collections.Generic.List[UInt64]]::new()
    for ($i = 0; $i -lt $n; $i++) { $out.Add([BitConverter]::ToUInt64($bytes, 8 * $i)) }
    return , $out.ToArray()
}

function Resolve-CapturePath([string]$path) {
    if (Test-Path -LiteralPath $path -PathType Leaf) { return (Resolve-Path -LiteralPath $path).Path }
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "No such capture: $path" }
    $direct = Join-Path $path 'capture.txt'
    if (Test-Path -LiteralPath $direct) { return (Resolve-Path -LiteralPath $direct).Path }
    $runs = @(Get-ChildItem -LiteralPath $path -Directory | Where-Object { $_.Name -match '^\d{8}-\d{6}$' -and (Test-Path (Join-Path $_.FullName 'capture.txt')) } | Sort-Object Name -Descending)
    if ($runs.Count -eq 0) { throw "No capture.txt in $path or in a run folder below it" }
    return (Join-Path $runs[0].FullName 'capture.txt')
}

function New-Launch([hashtable]$kv, [int]$eval) {
    $psize = [int]($kv['psize'])
    $pbytes = if ($kv.ContainsKey('pbytes')) { [int]$kv['pbytes'] } else { 0 }
    $hex = if ($kv.ContainsKey('params')) { $kv['params'] } else { '-' }
    [byte[]]$bytes = if ($hex -and $hex -ne '-') { [Convert]::FromHexString($hex) } else { @() }
    return [pscustomobject]@{
        Eval = $eval; Ord = [int]$kv['ord']; Fn = $kv['fn']; Kernel = $kv['kernel']
        Vit = ($kv['vit'] -eq '1'); Role = $kv['role']; Abi = $kv['abi']
        Grid = (ConvertTo-Dim $kv['grid']); Block = (ConvertTo-Dim $kv['block']); Shared = [int]$kv['shared']
        PSize = $psize; PMode = $kv['pmode']; PBytes = $pbytes; Trunc = ($kv['trunc'] -eq '1'); Hex = $hex
        QWords = (Get-QWords $hex)
        Bytes = $bytes
    }
}

function Read-CaptureFile([string]$path) {
    $data = [pscustomobject]@{
        Kind = 'capture'; Source = $path; Format = 0; Run = '-'; Modules = @(); Evals = @(); Notes = @(); Done = $false; Funcs = @()
    }
    $evals = [System.Collections.Generic.List[object]]::new()
    $modules = [System.Collections.Generic.List[object]]::new()
    $funcs = [System.Collections.Generic.List[object]]::new()
    $notes = [System.Collections.Generic.List[string]]::new()
    $current = $null
    foreach ($line in [IO.File]::ReadLines($path)) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $r = ConvertFrom-Record $line
        $kv = $r.Kv
        switch ($r.Tag) {
            'CAPTURE' { $data.Format = [int]$kv['format']; $data.Run = $kv['run'] }
            'MODULE' { $modules.Add([pscustomobject]@{ Handle = $kv['handle']; Bytes = [int]$kv['bytes']; Sha = $kv['sha256']; Magic = $kv['magic']; Dump = $kv['dump']; Status = $kv['status'] }) }
            'FUNC' { $funcs.Add([pscustomobject]@{ Fn = $kv['fn']; Module = $kv['module']; Kernel = $kv['kernel']; Role = $kv['role'] }) }
            'EVAL' {
                $current = [pscustomobject]@{
                    N = [int]$kv['n']; Feature = [int]$kv['feature']; Reset = ($kv['reset'] -eq '1'); Every = [int]$kv['every']
                    Override = ($kv['override'] -eq '1'); ObserveOnly = ($kv['observe_only'] -eq '1')
                    Launches = [System.Collections.Generic.List[object]]::new(); Seen = 0; Recorded = 0; Skipped = 0; Success = $null
                }
                $evals.Add($current)
            }
            'LAUNCH' { if ($null -ne $current) { $current.Launches.Add((New-Launch $kv $current.N)) } }
            'END' { if ($null -ne $current) { $current.Seen = [int]$kv['launches']; $current.Recorded = [int]$kv['recorded']; $current.Skipped = [int]$kv['skipped']; $current.Success = ($kv['success'] -eq '1') } }
            'DONE' { $data.Done = $true }
            'NOTE' { $notes.Add($kv['text']) }
        }
    }
    if ($data.Format -ne 1) { throw "Unsupported or missing capture format ($($data.Format)) in $path" }
    $data.Evals = $evals.ToArray(); $data.Modules = $modules.ToArray(); $data.Funcs = $funcs.ToArray(); $data.Notes = $notes.ToArray()
    return $data
}

# The plain log has only the first two evaluations' cc_vit_1d launches (names, parameter sizes, geometry), split at each Start.
function Read-LogFile([string]$path) {
    $data = [pscustomobject]@{
        Kind = 'log'; Source = $path; Format = 0; Run = '-'; Modules = @(); Evals = @(); Notes = @(); Done = $false; Funcs = @()
    }
    $rx = 'NR reuse unverified launch: kernel=(?<k>\S+), params=(?<p>\d+), grid=(?<g>\d+x\d+x\d+), block=(?<b>\d+x\d+x\d+), shared=(?<s>\d+)'
    $mrx = 'NR reuse module: bytes=(?<bytes>\d+), sha256=(?<sha>[0-9a-f]{64}), verified=(?<v>\w+)(, hybrid=(?<h>\w+))?, first kernel=(?<k>\S+)'
    $evals = [System.Collections.Generic.List[object]]::new()
    $modules = [System.Collections.Generic.List[object]]::new()
    $current = $null
    $ord = 0
    foreach ($line in [IO.File]::ReadLines($path)) {
        $m = [regex]::Match($line, $mrx)
        if ($m.Success) {
            $modules.Add([pscustomobject]@{ Handle = '-'; Bytes = [int]$m.Groups['bytes'].Value; Sha = $m.Groups['sha'].Value; Magic = '-'; Dump = '-'; Status = ('verified=' + $m.Groups['v'].Value) })
            continue
        }
        $m = [regex]::Match($line, $rx)
        if (-not $m.Success) { continue }
        $name = $m.Groups['k'].Value
        $role = 'inner'
        if ($name -like 'cc_vit_1d_repack_2d_to_1d*') { $role = 'start' } elseif ($name -like 'cc_vit_1d_repack_1d_to_2d*') { $role = 'end' }
        if ($role -eq 'start' -or $null -eq $current) {
            $current = [pscustomobject]@{
                N = $evals.Count + 1; Feature = 0; Reset = $false; Every = 2; Override = $false; ObserveOnly = $false
                Launches = [System.Collections.Generic.List[object]]::new(); Seen = 0; Recorded = 0; Skipped = 0; Success = $true
            }
            $evals.Add($current); $ord = 0
        }
        $kv = @{ ord = "$ord"; fn = '-'; kernel = $name; vit = '1'; role = $role; abi = 'log'; grid = $m.Groups['g'].Value; block = $m.Groups['b'].Value
                 shared = $m.Groups['s'].Value; psize = $m.Groups['p'].Value; pmode = 'none'; pbytes = '0'; trunc = '0'; params = '-' }
        $current.Launches.Add((New-Launch $kv $current.N))
        $ord++
        $current.Seen = $ord; $current.Recorded = $ord
    }
    if ($evals.Count -eq 0) { throw "No 'NR reuse unverified launch' lines in $path (needs VitEvery=2 with VitAllowUnverified off on an unverified module)" }
    $data.Evals = $evals.ToArray(); $data.Modules = $modules.ToArray()
    return $data
}

function Get-ShortName([string]$name) {
    $s = $name
    if ($s.StartsWith('cc_vit_1d_')) { $s = $s.Substring(10) }
    if ($s.EndsWith('_fp8')) { $s = $s.Substring(0, $s.Length - 4) }
    return $s
}

# Repeated groups ("x7") in a list of names; greedy, periods 1..8, only when a group repeats at least twice.
function Compress-Sequence([string[]]$seq) {
    $out = [System.Collections.Generic.List[string]]::new()
    $i = 0
    while ($i -lt $seq.Count) {
        $bestP = 0; $bestReps = 1
        for ($p = 1; $p -le 8 -and ($i + 2 * $p) -le $seq.Count; $p++) {
            $reps = 1
            while (($i + ($reps + 1) * $p) -le $seq.Count) {
                $same = $true
                for ($j = 0; $j -lt $p; $j++) { if ($seq[$i + $j] -ne $seq[$i + $reps * $p + $j]) { $same = $false; break } }
                if (-not $same) { break }
                $reps++
            }
            if ($reps -ge 2 -and ($reps * $p) -gt ($bestReps * [math]::Max($bestP, 1))) { $bestP = $p; $bestReps = $reps }
        }
        if ($bestP -gt 0) {
            $group = ($seq[$i..($i + $bestP - 1)] | ForEach-Object { Get-ShortName $_ }) -join ' ; '
            $out.Add($(if ($bestP -eq 1) { "$group x$bestReps" } else { "[$group] x$bestReps" }))
            $i += $bestP * $bestReps
        } else {
            $out.Add((Get-ShortName $seq[$i])); $i++
        }
    }
    return ($out -join ' ; ')
}

function Get-VitSignature($eval) {
    $names = @($eval.Launches | Where-Object { $_.Vit } | ForEach-Object { $_.Kernel })
    $withSize = @($eval.Launches | Where-Object { $_.Vit } | ForEach-Object { "$($_.Kernel):$($_.PSize)" })
    # What the product log lists ("NR reuse unverified launch"): only the cc_vit_1d_* family, i.e. the kernels with a role.
    $family = @($eval.Launches | Where-Object { $_.Vit -and $_.Role -ne 'none' } | ForEach-Object { $_.Kernel })
    return [pscustomobject]@{
        Names = $names; Count = $names.Count; Family = $family
        NamesHash = (Get-Sha256Text ($names -join "`n")); SizesHash = (Get-Sha256Text ($withSize -join "`n"))
        FamilyHash = (Get-Sha256Text ($family -join "`n"))
        Compressed = (Compress-Sequence $names)
    }
}

function Get-Range($eval) {
    $starts = @($eval.Launches | Where-Object { $_.Role -eq 'start' })
    $ends = @($eval.Launches | Where-Object { $_.Role -eq 'end' })
    $s = if ($starts.Count -gt 0) { $starts[0].Ord } else { -1 }
    $e = if ($ends.Count -gt 0) { $ends[$ends.Count - 1].Ord } else { -1 }
    return [pscustomobject]@{ Starts = $starts.Count; Ends = $ends.Count; First = $s; Last = $e }
}

function Add-Check($list, [string]$id, [string]$status, [string]$text, $feature = $null) {
    $list.Add([pscustomobject]@{ Id = $id; Status = $status; Text = $text; Feature = $feature })
}

# ---- kernel names ----
# The module's kernels carry a precision suffix (cc_vit_1d_projection_wait_fp8); the name is judged without it.
function Get-StrippedName([string]$name) { return [regex]::Replace($name, '_(nv)?fp\d+$', '') }
# 'waiter' = a kernel that could poll or publish synchronization flags by its name (*_wait, *_chained, *_tilesync, *_publish);
# 'unknown' = the runtime never told us the name (judged like a waiter: nothing can be assumed about it); else 'plain'.
function Get-SharerKind([string]$name) {
    if (-not $name -or $name -eq '?') { return 'unknown' }
    if ([regex]::IsMatch((Get-StrippedName $name), '_(wait|chained|tilesync|publish)$')) { return 'waiter' }
    return 'plain'
}
function Test-ClearKernel([string]$name) { return (Get-StrippedName $name) -eq 'cc_cb_clear' }

function Get-FfnKind([string]$name) {
    if ($name -like '*ffn_expand_publish*' -or $name -like '*ffn_expand_chained*') { return 'expand' }
    if ($name -like '*ffn_contract_chained*') { return 'contract' }
    return $null
}

# ---- per-feature analysis (capture only) ----
# A capture records the NR pass feature of every evaluation (EVAL feature=<ordinal in order of first use>). With Passes = 2 the first
# three evaluations are pass 0, pass 1, pass 0: two features with their own buffers, so stability, sharing and parameter changes are
# judged on the evaluations of ONE feature.
$script:ClusterHi = [System.Collections.Generic.HashSet[UInt32]]::new()

# 'ptr' = in range and in an address cluster, 'lo?' = below 4 GiB and 16-byte aligned, 'int?' = in range but in an unclustered high
# dword (probably two packed integers), $null = not a candidate.
function Get-PointerConf([UInt64]$v) {
    if ($v -lt $PointerMin -or $v -ge $PointerMax) { return $null }
    if ($v -lt [UInt64]4294967296) { if (($v % 16) -ne 0) { return $null } else { return 'lo?' } }
    if ($script:ClusterHi.Contains([UInt32]($v -shr 32))) { return 'ptr' } else { return 'int?' }
}

function Format-Ptr([object]$v) { if ($null -eq $v) { return '-' } else { return ('0x{0:x}' -f [UInt64]$v) } }

# Pointer-like values used by 2+ launches of one evaluation, classified by where the launches sit relative to Start..End.
# $laterSets: value sets of the later evaluations of the same feature (for the Stable column), or $null.
function Get-EvalRows($e, $laterSets) {
    $range = Get-Range $e
    $hasOutside = @($e.Launches | Where-Object { -not $_.Vit }).Count -gt 0
    $occ = @{}
    foreach ($l in $e.Launches) {
        if ($l.PMode -ne 'blob') { continue }
        for ($q = 0; $q -lt $l.QWords.Count; $q++) {
            $v = [UInt64]$l.QWords[$q]
            if ($null -eq (Get-PointerConf $v)) { continue }
            $inside = $range.First -ge 0 -and $range.Last -ge 0 -and $l.Ord -ge $range.First -and $l.Ord -le $range.Last
            $pos = if ($inside) { 'inside' } elseif ($range.First -ge 0 -and $l.Ord -lt $range.First) { 'before' } elseif ($range.Last -ge 0 -and $l.Ord -gt $range.Last) { 'after' } else { 'outside' }
            $key = '0x{0:x}' -f $v
            if (-not $occ.ContainsKey($key)) { $occ[$key] = [System.Collections.Generic.List[object]]::new() }
            $occ[$key].Add([pscustomobject]@{ Ord = $l.Ord; Kernel = $l.Kernel; Role = $l.Role; Off = 8 * $q; Inside = $inside; Vit = $l.Vit; Pos = $pos })
        }
    }
    $rows = [System.Collections.Generic.List[object]]::new()
    $hidden = 0; $leaks = 0; $softLeaks = 0
    foreach ($key in $occ.Keys) {
        $list = @($occ[$key])
        if ($list.Count -lt 2) { continue }
        $v = [Convert]::ToUInt64($key.Substring(2), 16)
        $conf = Get-PointerConf $v
        if ($conf -eq 'int?' -and -not $ShowUnlikely) { $hidden++; continue }
        $kernels = @($list | ForEach-Object { $_.Kernel } | Sort-Object -Unique)
        $inner = @($list | Where-Object { $_.Inside -and $_.Role -eq 'inner' }).Count
        $edge = @($list | Where-Object { $_.Inside -and $_.Role -in @('start', 'end') }).Count
        $outsideList = @($list | Where-Object { -not $_.Inside })
        $outside = $outsideList.Count
        $class = if (-not $hasOutside) { 'unknown (no launches outside the ViT kernels captured)' }
                 elseif ($range.First -lt 0 -or $range.Last -lt 0) { 'unknown (no Start..End run)' }
                 elseif ($inner -gt 0 -and $outside -gt 0) { 'LEAK: inside the run and outside it' }
                 elseif ($edge -gt 0 -and $outside -gt 0 -and $inner -eq 0) { 'boundary: Start/End and outside' }
                 elseif ($outside -eq 0) { 'run-private: only inside Start..End' }
                 else { 'outside the run only' }
        $isLeak = $class -like 'LEAK*'
        if ($isLeak) { if ($conf -eq 'ptr') { $leaks++ } else { $softLeaks++ } }
        # who outside the run shares the value
        $byName = @{}
        foreach ($o in $outsideList) {
            if (-not $byName.ContainsKey($o.Kernel)) { $byName[$o.Kernel] = [pscustomobject]@{ Kernel = $o.Kernel; Kind = (Get-SharerKind $o.Kernel); Clear = (Test-ClearKernel $o.Kernel); Launches = 0; Before = 0; After = 0 } }
            $s = $byName[$o.Kernel]
            $s.Launches++
            if ($o.Pos -eq 'before') { $s.Before++ } elseif ($o.Pos -eq 'after') { $s.After++ }
        }
        $sharers = @($byName.Values | Sort-Object @{ Expression = 'Launches'; Descending = $true }, Kernel)
        $innerUses = @{}
        foreach ($o in @($list | Where-Object { $_.Inside -and $_.Role -eq 'inner' })) { $k = '{0}@p{1}' -f (Get-ShortName $o.Kernel), ($o.Off / 8); if ($innerUses.ContainsKey($k)) { $innerUses[$k]++ } else { $innerUses[$k] = 1 } }
        $label = if ($isLeak -and $sharers.Count -eq 1 -and $sharers[0].Clear) { 'sentinel-clear' } else { '' }
        $stable = if ($null -eq $laterSets -or $laterSets.Count -eq 0) { 'n/a' } else { if (@($laterSets | Where-Object { -not $_.Contains($key) }).Count -eq 0) { 'yes' } else { 'NO' } }
        $rows.Add([pscustomobject]@{
            Value = $key; Conf = $conf; Launches = $list.Count; Kernels = $kernels.Count
            Inner = $inner; Edge = $edge; Outside = $outside; Stable = $stable; Class = $class; Label = $label
            Where = ((($list | Select-Object -First 4) | ForEach-Object { '{0}@+{1}' -f (Get-ShortName $_.Kernel), $_.Off }) -join ', ') + $(if ($list.Count -gt 4) { ', ...' } else { '' })
            IsLeak = $isLeak; Sharers = $sharers; InnerUses = $innerUses
        })
    }
    $sorted = @($rows | Sort-Object @{ Expression = { $_.Conf -ne 'ptr' } }, @{ Expression = 'Kernels'; Descending = $true }, @{ Expression = 'Launches'; Descending = $true }, Value)
    return [pscustomobject]@{ Rows = $sorted; Hidden = $hidden; Leaks = $leaks; SoftLeaks = $softLeaks; Range = $range; HasOutside = $hasOutside }
}

# Start slot 0/1 and End slot 0/1 of one evaluation (8-byte parameters); Missing says exactly which field could not be read.
function Get-EvalSlots($e) {
    $miss = [System.Collections.Generic.List[string]]::new()
    $start = @($e.Launches | Where-Object { $_.Role -eq 'start' } | Select-Object -First 1)
    $end = @($e.Launches | Where-Object { $_.Role -eq 'end' } | Select-Object -Last 1)
    $s0 = $null; $s1 = $null; $e0 = $null; $e1 = $null
    if ($start.Count -eq 0) { $miss.Add("eval $($e.N): no Start launch") }
    elseif ($start[0].PMode -ne 'blob' -or $start[0].QWords.Count -lt 2) { $miss.Add("eval $($e.N): the Start launch has no readable first two 8-byte parameters (pmode $($start[0].PMode), $($start[0].PBytes) bytes)") }
    else { $s0 = [UInt64]$start[0].QWords[0]; $s1 = [UInt64]$start[0].QWords[1] }
    if ($end.Count -eq 0) { $miss.Add("eval $($e.N): no End launch") }
    elseif ($end[0].PMode -ne 'blob' -or $end[0].QWords.Count -lt 2) { $miss.Add("eval $($e.N): the End launch has no readable first two 8-byte parameters (pmode $($end[0].PMode), $($end[0].PBytes) bytes)") }
    else { $e0 = [UInt64]$end[0].QWords[0]; $e1 = [UInt64]$end[0].QWords[1] }
    $link = if ($null -ne $s1 -and $null -ne $e0) { $s1 -eq $e0 } else { $null }
    return [pscustomobject]@{ Eval = $e.N; StartP0 = $s0; StartP1 = $s1; EndP0 = $e0; EndP1 = $e1; Link = $link; Missing = $miss.ToArray() }
}

# All values of one slot across the slots of a feature's evaluations: 'same' | 'changes' | 'unknown'
function Get-SlotState($slots, [string]$field) {
    $vals = @($slots | ForEach-Object { $_.$field })
    if (@($vals | Where-Object { $null -eq $_ }).Count -gt 0) { return 'unknown' }
    if (@($vals | Select-Object -Unique).Count -eq 1) { return 'same' }
    return 'changes'
}

# cc_cb_clear launches of one evaluation: launched before Start, and does a pointer of it equal a LEAK pointer of the evaluation?
function Get-EvalClear($e, $range, $leakKeys) {
    $clears = @($e.Launches | Where-Object { Test-ClearKernel $_.Kernel })
    $before = @($clears | Where-Object { $range.First -ge 0 -and $_.Ord -lt $range.First })
    $match = $null
    foreach ($c in $before) {
        if ($c.PMode -ne 'blob') { continue }
        foreach ($q in $c.QWords) {
            $v = [UInt64]$q
            if ((Get-PointerConf $v) -ne 'ptr') { continue }
            if ($leakKeys.Contains(('0x{0:x}' -f $v))) { $match = '0x{0:x}' -f $v; break }
        }
        if ($match) { break }
    }
    return [pscustomobject]@{ Eval = $e.N; Total = $clears.Count; Before = $before.Count; Match = $match; Launch = $(if ($before.Count -gt 0) { $before[0] } else { $null }) }
}

# 4-byte values of the Inner kernels' parameters that differ between evaluations of one feature. A slot that holds a pointer
# in every evaluation and moves is a moved buffer, not a counter: counted apart (MovedPtrs). Comparable=false says why not.
function Get-EpochDiffs($fe) {
    $res = [pscustomobject]@{ Comparable = $true; Why = ''; Diffs = [System.Collections.Generic.List[object]]::new(); MovedPtrs = 0; InnerLaunches = 0 }
    if ($fe.Count -lt 2) { $res.Comparable = $false; $res.Why = "only $($fe.Count) evaluation(s) of this feature"; return $res }
    $inner = @($fe | ForEach-Object { , @($_.Launches | Where-Object { $_.Role -eq 'inner' } | Sort-Object Ord) })
    $n = $inner[0].Count
    if (@($inner | Where-Object { $_.Count -ne $n }).Count -gt 0) { $res.Comparable = $false; $res.Why = 'the evaluations have different numbers of Inner launches'; return $res }
    if ($n -eq 0) { $res.Comparable = $false; $res.Why = 'no Inner launches'; return $res }
    $res.InnerLaunches = $n
    for ($i = 0; $i -lt $n; $i++) {
        $ls = @($inner | ForEach-Object { $_[$i] })
        if (@($ls | ForEach-Object { $_.Kernel } | Select-Object -Unique).Count -ne 1) { $res.Comparable = $false; $res.Why = "Inner launch #$i is a different kernel in different evaluations"; return $res }
        if (@($ls | Where-Object { $_.PMode -ne 'blob' }).Count -gt 0) { $res.Comparable = $false; $res.Why = "Inner launch #$i ($($ls[0].Kernel)) has no readable parameter blob in every evaluation"; return $res }
        if (@($ls | ForEach-Object { $_.Bytes.Length } | Select-Object -Unique).Count -ne 1) { $res.Comparable = $false; $res.Why = "Inner launch #$i ($($ls[0].Kernel)) has a different parameter size in different evaluations"; return $res }
        $dwords = [int][math]::Floor($ls[0].Bytes.Length / 4)
        $movedSlots = @{}
        for ($o = 0; $o -lt $dwords; $o++) {
            $vals = @($ls | ForEach-Object { [BitConverter]::ToUInt32($_.Bytes, 4 * $o) })
            if (@($vals | Select-Object -Unique).Count -eq 1) { continue }
            $slot = [int][math]::Floor($o / 2)
            $allPtr = $true
            foreach ($l in $ls) {
                if ($slot -ge $l.QWords.Count -or (Get-PointerConf ([UInt64]$l.QWords[$slot])) -ne 'ptr') { $allPtr = $false; break }
            }
            if ($allPtr) { $movedSlots[$slot] = $true; continue }
            $res.Diffs.Add([pscustomobject]@{ Kernel = $ls[0].Kernel; Index = $i; Offset = 4 * $o; Values = (($vals | ForEach-Object { '0x{0:x}' -f $_ }) -join ' vs ') })
        }
        $res.MovedPtrs += $movedSlots.Count
    }
    return $res
}

# Launches that carry End slot 1 or Start slot 1 of an evaluation: outside the run (before Start / after End) and, for other
# features' evaluations, anywhere. Also counts the outside launches whose parameters cannot be read.
function Get-EndUsers($e, $slot, $allEvals, [int]$feature) {
    $range = Get-Range $e
    $users = [System.Collections.Generic.List[object]]::new()
    $unreadableBefore = 0; $unreadableAfter = 0
    $subjects = @(@{ Name = 'End.p1'; V = $slot.EndP1 }, @{ Name = 'Start.p1'; V = $slot.StartP1 })
    foreach ($l in $e.Launches) {
        $inside = $range.First -ge 0 -and $range.Last -ge 0 -and $l.Ord -ge $range.First -and $l.Ord -le $range.Last
        if ($inside) { continue }
        $pos = if ($range.First -ge 0 -and $l.Ord -lt $range.First) { 'before Start' } elseif ($range.Last -ge 0 -and $l.Ord -gt $range.Last) { 'after End' } else { 'outside' }
        if ($l.PMode -ne 'blob' -or $l.Trunc) { if ($pos -eq 'before Start') { $unreadableBefore++ } else { $unreadableAfter++ } }
        if ($l.PMode -ne 'blob') { continue }
        foreach ($s in $subjects) {
            if ($null -eq $s.V) { continue }
            for ($q = 0; $q -lt $l.QWords.Count; $q++) {
                if ([UInt64]$l.QWords[$q] -eq [UInt64]$s.V) { $users.Add([pscustomobject]@{ Eval = $e.N; Subject = $s.Name; Value = $s.V; Ord = $l.Ord; Pos = $pos; Kernel = $l.Kernel; Off = 8 * $q; Kind = (Get-SharerKind $l.Kernel) }) }
            }
        }
    }
    foreach ($o in $allEvals) {
        if ($o.Feature -eq $feature) { continue }
        foreach ($l in $o.Launches) {
            if ($l.PMode -ne 'blob') { continue }
            foreach ($s in $subjects) {
                if ($null -eq $s.V) { continue }
                for ($q = 0; $q -lt $l.QWords.Count; $q++) {
                    if ([UInt64]$l.QWords[$q] -eq [UInt64]$s.V) { $users.Add([pscustomobject]@{ Eval = $e.N; Subject = $s.Name; Value = $s.V; Ord = $l.Ord; Pos = "evaluation $($o.N) (feature $($o.Feature))"; Kernel = $l.Kernel; Off = 8 * $q; Kind = (Get-SharerKind $l.Kernel) }) }
                }
            }
        }
    }
    return [pscustomobject]@{ Users = $users.ToArray(); UnreadableBefore = $unreadableBefore; UnreadableAfter = $unreadableAfter }
}

# Everything about one feature's evaluations; the checks and the rule read this object only.
function Get-FeatureAnalysis([int]$feature, $fe, $allEvals) {
    $first = $fe[0]
    $laterSets = @()
    foreach ($e in ($fe | Select-Object -Skip 1)) {
        $set = [System.Collections.Generic.HashSet[string]]::new()
        foreach ($l in $e.Launches) { if ($l.PMode -eq 'blob') { foreach ($v in $l.QWords) { [void]$set.Add(('0x{0:x}' -f [UInt64]$v)) } } }
        $laterSets += , $set
    }
    $rowsByEval = @{}
    foreach ($e in $fe) {
        if ($e.N -eq $first.N) { $rowsByEval[$e.N] = Get-EvalRows $e $laterSets } else { $rowsByEval[$e.N] = Get-EvalRows $e $null }
    }

    # LEAK pointers, merged over the feature's evaluations (conservative: a LEAK in any evaluation counts; sharers are unioned)
    $ptrLeaks = @{}; $softLeakKeys = [System.Collections.Generic.HashSet[string]]::new()
    $classifiable = $true
    foreach ($e in $fe) {
        $r = $rowsByEval[$e.N]
        if (-not $r.HasOutside -or $r.Range.First -lt 0 -or $r.Range.Last -lt 0) { $classifiable = $false }
        foreach ($row in $r.Rows) {
            if (-not $row.IsLeak) { continue }
            if ($row.Conf -ne 'ptr') { [void]$softLeakKeys.Add($row.Value); continue }
            if (-not $ptrLeaks.ContainsKey($row.Value)) { $ptrLeaks[$row.Value] = [pscustomobject]@{ Value = $row.Value; Evals = [System.Collections.Generic.List[int]]::new(); Sharers = @{}; InnerUses = @{} } }
            $m = $ptrLeaks[$row.Value]
            $m.Evals.Add($e.N)
            foreach ($k in $row.InnerUses.Keys) { if (-not $m.InnerUses.ContainsKey($k) -or $m.InnerUses[$k] -lt $row.InnerUses[$k]) { $m.InnerUses[$k] = $row.InnerUses[$k] } }
            foreach ($s in $row.Sharers) {
                if (-not $m.Sharers.ContainsKey($s.Kernel)) { $m.Sharers[$s.Kernel] = [pscustomobject]@{ Kernel = $s.Kernel; Kind = $s.Kind; Clear = $s.Clear; Launches = 0; Before = 0; After = 0 } }
                $t = $m.Sharers[$s.Kernel]; $t.Launches += $s.Launches; $t.Before += $s.Before; $t.After += $s.After
            }
        }
    }
    $leakList = @($ptrLeaks.Values | ForEach-Object {
        $sharers = @($_.Sharers.Values | Sort-Object @{ Expression = 'Launches'; Descending = $true }, Kernel)
        $others = @($sharers | Where-Object { -not $_.Clear })
        [pscustomobject]@{
            Value = $_.Value; Evals = $_.Evals.ToArray(); Sharers = $sharers
            InnerUses = @($_.InnerUses.GetEnumerator() | Sort-Object Name | ForEach-Object { '{0} x{1}' -f $_.Name, $_.Value })
            Label = $(if ($sharers.Count -gt 0 -and $others.Count -eq 0) { 'sentinel-clear' } else { '' })
            WaiterSharers = @($others | Where-Object { $_.Kind -ne 'plain' })
            PlainSharers = @($others | Where-Object { $_.Kind -eq 'plain' })
        }
    } | Sort-Object Value)

    $slots = @($fe | ForEach-Object { Get-EvalSlots $_ })
    $clears = @($fe | ForEach-Object {
        $r = $rowsByEval[$_.N]
        $keys = [System.Collections.Generic.HashSet[string]]::new()
        foreach ($row in $r.Rows) { if ($row.IsLeak -and $row.Conf -eq 'ptr') { [void]$keys.Add($row.Value) } }
        Get-EvalClear $_ $r.Range $keys
    })
    # the pointer slots of the End kernel (its last launch in each evaluation; ptr values only), as the old report printed them
    $endLaunches = @($fe | ForEach-Object { @($_.Launches | Where-Object { $_.Role -eq 'end' -and $_.PMode -eq 'blob' } | Select-Object -Last 1) })
    $endSlotRows = @()
    if ($endLaunches.Count -eq $fe.Count -and $endLaunches.Count -gt 0) {
        $nSlots = [math]::Min(@($endLaunches | ForEach-Object { $_.QWords.Count } | Measure-Object -Minimum).Minimum, 64)
        for ($q = 0; $q -lt $nSlots; $q++) {
            $vals = @($endLaunches | ForEach-Object { [UInt64]$_.QWords[$q] })
            $isPtr = @($vals | Where-Object { (Get-PointerConf $_) -eq 'ptr' }).Count -eq $vals.Count
            if ($isPtr) { $endSlotRows += [pscustomobject]@{ Slot = $q; Offset = 8 * $q; Stable = (@($vals | Sort-Object -Unique).Count -eq 1); Values = (($vals | ForEach-Object { '0x{0:x}' -f $_ }) -join ' ') } }
        }
    }
    $epoch = Get-EpochDiffs $fe
    $endUsers = [System.Collections.Generic.List[object]]::new()
    for ($i = 0; $i -lt $fe.Count; $i++) { $endUsers.Add((Get-EndUsers $fe[$i] $slots[$i] $allEvals $feature)) }

    # where cc_cb_clear points (information): the region [ptr, ptr + 4 * count) if the second parameter is a dword count
    $clearRange = $null
    $cl = @($clears | Where-Object { $null -ne $_.Launch -and $_.Launch.PMode -eq 'blob' -and $_.Launch.Bytes.Length -ge 12 } | Select-Object -First 1)
    if ($cl.Count -gt 0) {
        $e1 = @($fe | Where-Object { $_.N -eq $cl[0].Eval })[0]
        $r1 = $rowsByEval[$e1.N].Range
        [UInt64]$lo = $cl[0].Launch.QWords[0]
        [UInt32]$cnt = [BitConverter]::ToUInt32($cl[0].Launch.Bytes, 8)
        [UInt64]$hi = $lo + 4 * [UInt64]$cnt
        $innerVals = [System.Collections.Generic.HashSet[UInt64]]::new()
        $outsideVals = [System.Collections.Generic.HashSet[UInt64]]::new()
        foreach ($l in $e1.Launches) {
            if ($l.PMode -ne 'blob') { continue }
            $inside = $r1.First -ge 0 -and $r1.Last -ge 0 -and $l.Ord -ge $r1.First -and $l.Ord -le $r1.Last
            foreach ($q in $l.QWords) {
                $v = [UInt64]$q
                if ((Get-PointerConf $v) -ne 'ptr') { continue }
                if ($inside -and $l.Role -eq 'inner') { [void]$innerVals.Add($v) } elseif (-not $inside) { [void]$outsideVals.Add($v) }
            }
        }
        $in = @($innerVals | Where-Object { $_ -ge $lo -and $_ -lt $hi })
        $shared = @($in | Where-Object { $outsideVals.Contains([UInt64]$_) })
        $clearRange = [pscustomobject]@{ Eval = $e1.N; Ord = $cl[0].Launch.Ord; Pointer = $lo; Count = $cnt; End = $hi; InnerValues = $innerVals.Count; InnerInside = $in.Count; InnerInsideAlsoOutside = $shared.Count }
    }

    $anyObserve = @($fe | Where-Object { $_.ObserveOnly }).Count
    return [pscustomobject]@{
        Feature = $feature; Evals = @($fe | ForEach-Object { $_.N }); First = $first; Fe = $fe
        EveryValues = @($fe | ForEach-Object { $_.Every } | Select-Object -Unique); WatchedOnly = ($anyObserve -eq $fe.Count)
        RowsByEval = $rowsByEval; PtrLeaks = $leakList; SoftLeaks = $softLeakKeys.Count; Classifiable = $classifiable
        Slots = $slots; EndSlotRows = $endSlotRows; Clears = $clears; ClearRange = $clearRange; Epoch = $epoch; EndUsers = $endUsers.ToArray()
    }
}

# The fixed go/no-go rule of STUDY-nr-flicker.md (decided before looking at any capture), applied to one feature. Every
# condition must be PASS for GO. Which failures could be settled by nvdisasm (a waiter outside the run, an after-End user that
# might write the output) and which are final is stated per condition: Kind = 'final' | 'nvdisasm' | 'decode'.
function Get-RuleResult($fa) {
    $c = [System.Collections.Generic.List[object]]::new()
    function Add-Cond([string]$id, [string]$status, [string]$kind, [string]$text) { $c.Add([pscustomobject]@{ Id = $id; Status = $status; Kind = $kind; Text = $text }) }

    # (1) End slot 1 and Start slot 1 identical in every evaluation of the feature, at least 2 evaluations
    $missing = @($fa.Slots | ForEach-Object { $_.Missing } | Where-Object { $_ })
    if ($fa.Evals.Count -lt 2) { Add-Cond '1 end-stable' 'UNKNOWN' 'final' "only $($fa.Evals.Count) evaluation(s) of this feature were captured; the rule needs at least 2" }
    elseif ($missing.Count -gt 0) { Add-Cond '1 end-stable' 'UNKNOWN' 'final' ("missing field(s): " + ($missing -join '; ')) }
    else {
        $e1 = Get-SlotState $fa.Slots 'EndP1'; $s1 = Get-SlotState $fa.Slots 'StartP1'
        if ($e1 -eq 'same' -and $s1 -eq 'same') { Add-Cond '1 end-stable' 'PASS' 'final' "End slot 1 = $(Format-Ptr $fa.Slots[0].EndP1) and Start slot 1 = $(Format-Ptr $fa.Slots[0].StartP1) in all $($fa.Evals.Count) evaluations (eval $($fa.Evals -join ', '))" }
        else { Add-Cond '1 end-stable' 'FAIL' 'final' ("End slot 1 " + $(if ($e1 -eq 'same') { 'is stable' } else { 'CHANGES' }) + ", Start slot 1 " + $(if ($s1 -eq 'same') { 'is stable' } else { 'CHANGES' }) + " between the evaluations") }
    }

    # (2) every LEAK pointer is sentinel-clear (or has no outside sharer); no waiters-outside FAIL
    if (-not $fa.Classifiable) { Add-Cond '2 leaks' 'UNKNOWN' 'final' 'the run or the launches outside it are not in the capture, so sharing cannot be classified' }
    else {
        $leaks = @($fa.PtrLeaks)
        $notClear = @($leaks | Where-Object { $_.Label -ne 'sentinel-clear' })
        $hard = @($notClear | Where-Object { $_.PlainSharers.Count -gt 0 })
        $waiters = @($leaks | Where-Object { $_.WaiterSharers.Count -gt 0 })
        if ($notClear.Count -eq 0) { Add-Cond '2 leaks' 'PASS' 'final' $(if ($leaks.Count -eq 0) { 'no pointer is shared between an Inner kernel of the run and a launch outside it' } else { "all $($leaks.Count) LEAK pointer(s) are shared only with cc_cb_clear (sentinel-clear) and no outside waiter shares one" }) }
        else {
            if ($hard.Count -gt 0) { Add-Cond '2a leaks-not-clear' 'FAIL' 'final' "$($hard.Count) LEAK pointer(s) are also used by an ordinary outside kernel that is not cc_cb_clear: $((@($hard | ForEach-Object { $_.Value }) | Select-Object -First 5) -join ', ')" }
            if ($waiters.Count -gt 0) {
                $names = @($waiters | ForEach-Object { $_.WaiterSharers | ForEach-Object { $_.Kernel } } | Select-Object -Unique)
                Add-Cond '2b waiters-outside' 'FAIL' 'nvdisasm' "$($waiters.Count) LEAK pointer(s) are shared with $($names.Count) outside waiter/publisher kernel(s) (e.g. $((@($names) | Select-Object -First 3) -join ', ')); this is the nvdisasm trigger"
            }
        }
    }

    # (3) clear-before
    $leakCount = @($fa.PtrLeaks).Count
    $badClear = @($fa.Clears | Where-Object { $_.Before -lt 1 -or -not $_.Match })
    if ($leakCount -eq 0) { Add-Cond '3 clear-before' 'N/A' 'final' 'there is no LEAK pointer for cc_cb_clear to be shared with; the rule asks for PASS, so this does not satisfy it' }
    elseif ($badClear.Count -eq 0) { Add-Cond '3 clear-before' 'PASS' 'final' "cc_cb_clear is launched before Start in every evaluation and its pointer equals a LEAK pointer" }
    else { Add-Cond '3 clear-before' 'FAIL' 'final' ("in eval " + (($badClear | ForEach-Object { $_.Eval }) -join ', ') + ": " + $(if (@($badClear | Where-Object { $_.Before -lt 1 }).Count -gt 0) { 'no cc_cb_clear launch before Start' } else { 'a cc_cb_clear launch before Start exists but its pointer is not a LEAK pointer' })) }

    # (4) epoch
    if (-not $fa.Epoch.Comparable) { Add-Cond '4 epoch' 'UNKNOWN' 'final' "not comparable: $($fa.Epoch.Why)" }
    elseif ($fa.Epoch.Diffs.Count -eq 0) { Add-Cond '4 epoch' 'PASS' 'final' "no 4-byte value in the parameters of the $($fa.Epoch.InnerLaunches) Inner launches differs between the evaluations (moved pointers apart: $($fa.Epoch.MovedPtrs))" }
    else { Add-Cond '4 epoch' 'FAIL' 'final' "$($fa.Epoch.Diffs.Count) 4-byte value(s) in Inner kernel parameters differ between evaluations, e.g. $(($fa.Epoch.Diffs | Select-Object -First 2 | ForEach-Object { '{0} +{1}: {2}' -f $_.Kernel, $_.Offset, $_.Values }) -join ' | ')" }

    # (5) end-users of End slot 1
    $eu = @($fa.EndUsers | ForEach-Object { $_.Users } | Where-Object { $_.Subject -eq 'End.p1' })
    $unreadBefore = ($fa.EndUsers | Measure-Object -Property UnreadableBefore -Sum).Sum
    $before = @($eu | Where-Object { $_.Pos -eq 'before Start' })
    $after = @($eu | Where-Object { $_.Pos -eq 'after End' })
    if (@($fa.Slots | Where-Object { $null -eq $_.EndP1 }).Count -gt 0) { Add-Cond '5a end-users-before' 'UNKNOWN' 'final' 'End slot 1 is not readable in every evaluation' }
    elseif ($before.Count -gt 0) { Add-Cond '5a end-users-before' 'FAIL' 'final' "a launch before Start carries End slot 1: $((@($before | ForEach-Object { $_.Kernel } | Select-Object -Unique) | Select-Object -First 3) -join ', ')" }
    elseif ($unreadBefore -gt 0) { Add-Cond '5a end-users-before' 'UNKNOWN' 'final' "$unreadBefore launch(es) before Start have no complete parameter blob, so a user of End slot 1 cannot be excluded" }
    else { Add-Cond '5a end-users-before' 'PASS' 'final' 'no launch before Start carries End slot 1' }
    if ($after.Count -eq 0) {
        if (@($fa.Slots | Where-Object { $null -eq $_.EndP1 }).Count -gt 0 -or ($fa.EndUsers | Measure-Object -Property UnreadableAfter -Sum).Sum -gt 0) { Add-Cond '5b end-users-after' 'UNKNOWN' 'final' 'End slot 1 or the launches after End are not fully readable' }
        else { Add-Cond '5b end-users-after' 'PASS' 'final' 'no launch after End carries End slot 1' }
    }
    else {
        $names = @($after | ForEach-Object { $_.Kernel } | Select-Object -Unique)
        Add-Cond '5b end-users-after' 'DECODE' 'decode' "$($names.Count) kernel(s) after End carry End slot 1 and must be shown to only read it (static decode needed): $($names -join ', ')"
    }

    $items = $c.ToArray()
    $finalFail = @($items | Where-Object { $_.Status -ne 'PASS' -and $_.Status -ne 'DECODE' -and $_.Kind -eq 'final' })
    $nv = @($items | Where-Object { $_.Status -eq 'FAIL' -and $_.Kind -eq 'nvdisasm' })
    $dec = @($items | Where-Object { $_.Status -eq 'DECODE' })
    # a 2 PASS row replaces 2a/2b; absent both rows means the condition was evaluated
    $verdict = if ($fa.Evals.Count -lt 2) { 'NOT-JUDGED' }
               elseif ($finalFail.Count -gt 0) { 'NO-GO' }
               elseif ($nv.Count -gt 0) { 'NEEDS-NVDISASM' }
               elseif ($dec.Count -gt 0) { 'NEEDS-DECODE' }
               else { 'GO' }
    return [pscustomobject]@{ Feature = $fa.Feature; Verdict = $verdict; Conditions = $items }
}

$sourcePath = $null
if ($Log) { $data = Read-LogFile (Resolve-Path -LiteralPath $Log).Path }
elseif ($Capture) { $data = Read-CaptureFile (Resolve-CapturePath $Capture) }
else { throw 'Give -Capture <capture.txt | run folder | nr-vit-capture folder> or -Log <OptiScaler.log>. See Get-Help.' }

$out = [System.Collections.Generic.List[string]]::new()
function Say([string]$text = '') { $out.Add($text) }
function Say-Table($rows, $props) { foreach ($l in (($rows | Format-Table -Property $props -AutoSize | Out-String -Width 240).TrimEnd() -split "`r?`n")) { Say $l } }

$checks = [System.Collections.Generic.List[object]]::new()
$evals = @($data.Evals)
$isCapture = $data.Kind -eq 'capture'

Say "NR ViT launch summary"
Say "source: $($data.Source)  ($($data.Kind), $($evals.Count) evaluation(s), $(if ($isCapture) { "run $($data.Run)" } else { 'taken from the plain log: names, parameter sizes and geometry only, no parameter bytes' }))"
if ($isCapture) {
    foreach ($m in $data.Modules) { Say ("module {0}  {1,9} bytes  sha256 {2}  magic {3}  dump {4}  ({5})" -f $m.Handle, $m.Bytes, $m.Sha, $m.Magic, $m.Dump, $m.Status) }
    foreach ($n in $data.Notes) { Say "NOTE: $n" }
    foreach ($e in $evals) { Say ("eval {0}: feature {1}, reset {2}, every {3}, override {4}{5}, {6} launches seen, {7} recorded, success {8}" -f $e.N, $e.Feature, [int]$e.Reset, $e.Every, [int]$e.Override, $(if ($e.ObserveOnly) { ', watched only' } else { '' }), $e.Seen, $e.Recorded, $e.Success) }
} else {
    foreach ($m in $data.Modules) { Say ("module  {0,9} bytes  sha256 {1}  ({2})" -f $m.Bytes, $m.Sha, $m.Status) }
}
Say

# ---- (a) signature ----
Say '== (a) launch-order signature of the cc_vit_* kernels =='
$sigs = @($evals | ForEach-Object { Get-VitSignature $_ })
for ($i = 0; $i -lt $evals.Count; $i++) {
    Say ("eval {0}: {1} cc_vit_* launches" -f $evals[$i].N, $sigs[$i].Count)
    Say ("  sequence : {0}" -f $sigs[$i].Compressed)
    Say ("  SIGNATURE (names)          sha256: {0}" -f $sigs[$i].NamesHash)
    Say ("  SIGNATURE (names + psize)  sha256: {0}" -f $sigs[$i].SizesHash)
    if ($sigs[$i].FamilyHash -ne $sigs[$i].NamesHash) { Say ("  SIGNATURE (cc_vit_1d_* only, {0} launches; what the log lists) sha256: {1}" -f $sigs[$i].Family.Count, $sigs[$i].FamilyHash) }
    if ($isCapture) {
        $allNames = @($evals[$i].Launches | ForEach-Object { $_.Kernel })
        Say ("  all {0} launches of the evaluation (names)  sha256: {1}" -f $allNames.Count, (Get-Sha256Text ($allNames -join "`n")))
    }
}
$sameNames = @($sigs | Select-Object -ExpandProperty NamesHash -Unique).Count -le 1
$sameSizes = @($sigs | Select-Object -ExpandProperty SizesHash -Unique).Count -le 1
Say ("same cc_vit_* order in every captured evaluation: names {0}, names + psize {1}" -f $(if ($sameNames) { 'yes' } else { 'NO' }), $(if ($sameSizes) { 'yes' } else { 'NO' }))
Say

# ---- (b) per-kernel parameter sizes ----
Say '== (b) per-kernel parameter sizes and launch geometry (first evaluation) =='
$first = $evals[0]
$rows = foreach ($g in ($first.Launches | Where-Object { $AllKernels -or $_.Vit } | Group-Object Kernel)) {
    $l = @($g.Group)
    [pscustomobject]@{
        Kernel = $g.Name; Launches = $l.Count; Role = $l[0].Role
        PSize = (@($l | ForEach-Object { $_.PSize } | Sort-Object -Unique) -join ',')
        Grid = (@($l | ForEach-Object { $_.Grid -join 'x' } | Sort-Object -Unique) -join ' ')
        Block = (@($l | ForEach-Object { $_.Block -join 'x' } | Sort-Object -Unique) -join ' ')
        Shared = (@($l | ForEach-Object { $_.Shared } | Sort-Object -Unique) -join ',')
        FirstOrd = $l[0].Ord
    }
}
Say-Table ($rows | Sort-Object FirstOrd) @('Kernel', 'Launches', 'Role', 'PSize', 'Grid', 'Block', 'Shared')
Say

# ---- (c) pointer-like values ----
Say '== (c) 8-byte values that look like device pointers and appear in more than one launch =='
$analyses = @()
$pointerRows = @()
$featureGroups = @($evals | Group-Object -Property Feature | Sort-Object { [int]$_.Name })
$multiFeature = $featureGroups.Count -gt 1
if (-not $isCapture) {
    Say 'not available from the plain log (no parameter bytes). Use a VitCapture capture.'
} else {
    # Which high dwords are real address clusters: device pointers come in bulk (every weight buffer, scratch buffer and counter
    # shares a few high dwords), while a packed pair of 32-bit integers (width/height) lands in a high dword of its own.
    $byHigh = @{}
    foreach ($e in $evals) {
        foreach ($l in $e.Launches) {
            if ($l.PMode -ne 'blob') { continue }
            foreach ($q in $l.QWords) {
                $v = [UInt64]$q
                if ($v -lt [UInt64]4294967296 -or $v -lt $PointerMin -or $v -ge $PointerMax) { continue }
                $h = [UInt32]($v -shr 32)
                if (-not $byHigh.ContainsKey($h)) { $byHigh[$h] = [System.Collections.Generic.HashSet[UInt64]]::new() }
                [void]$byHigh[$h].Add($v)
            }
        }
    }
    foreach ($h in $byHigh.Keys) { if ($byHigh[$h].Count -ge $ClusterMin) { [void]$script:ClusterHi.Add([UInt32]$h) } }

    foreach ($g in $featureGroups) { $analyses += , (Get-FeatureAnalysis ([int]$g.Name) @($g.Group) $evals) }

    $firstBlock = $true
    if ($analyses.Count -gt 0) { $pointerRows = @($analyses[0].RowsByEval[$analyses[0].First.N].Rows) }
    foreach ($fa in $analyses) {
        $r1 = $fa.RowsByEval[$fa.First.N]
        $prows = @($r1.Rows)
        if ($multiFeature) {
            Say
            Say ("-- feature {0}: evaluation(s) {1}; VitEvery as applied {2}{3} --" -f $fa.Feature, ($fa.Evals -join ', '), ($fa.EveryValues -join '/'), $(if ($fa.WatchedOnly) { ', watched only' } else { '' }))
        }
        Say ("candidates: {0} value(s) in {1}..{2} used by 2+ launches of eval {3}. ptr = in an address cluster, lo? = below 4 GiB (aligned, uncertain), int? = unclustered high dword, probably packed integers" -f $prows.Count, ('0x{0:x}' -f $PointerMin), ('0x{0:x}' -f $PointerMax), $fa.First.N)
        if ($r1.Hidden -gt 0) { Say ("{0} int? value(s) hidden (-ShowUnlikely lists them)" -f $r1.Hidden) }
        if ($firstBlock -and $byHigh.Count -gt 0) {
            $top = $byHigh.GetEnumerator() | Sort-Object { $_.Value.Count } -Descending | Select-Object -First 6
            Say ('high dwords of the values in 2^32..2^48 with their distinct values (address clusters have {0}+): ' -f $ClusterMin + (($top | ForEach-Object { '0x{0:x8} x{1}' -f $_.Key, $_.Value.Count }) -join ', '))
        }
        $firstBlock = $false
        if ($prows.Count -gt 0) {
            Say-Table ($prows | Select-Object -First $MaxRows) @('Value', 'Conf', 'Launches', 'Kernels', 'Inner', 'Edge', 'Outside', 'Stable', 'Class', 'Label', 'Where')
            if ($prows.Count -gt $MaxRows) { Say ("... {0} more rows (raise -MaxRows)" -f ($prows.Count - $MaxRows)) }
        }
        Say 'Inner = launches strictly between Start and End, Edge = the Start or End launch, Outside = every other launch of the evaluation. Label sentinel-clear = a LEAK pointer whose only outside sharer is cc_cb_clear.'

        # the kernels outside the run that share a LEAK pointer, over all evaluations of the feature
        if (@($fa.PtrLeaks).Count -gt 0) {
            Say ''
            Say ("LEAK pointers of feature {0} and every launch outside the run that shares them (all {1} evaluation(s) of the feature merged):" -f $fa.Feature, $fa.Evals.Count)
            foreach ($lk in $fa.PtrLeaks) {
                $tot = ($lk.Sharers | Measure-Object -Property Launches -Sum).Sum
                Say ("  {0}  {1}  outside launches {2} in {3} kernel(s); used inside the run by {4} (launches per evaluation):" -f $lk.Value, $(if ($lk.Label) { "[$($lk.Label)]" } else { '[not sentinel-clear]' }), $tot, @($lk.Sharers).Count, ($lk.InnerUses -join ', '))
                foreach ($s in $lk.Sharers) {
                    $where = if ($s.Before -gt 0 -and $s.After -gt 0) { 'before Start and after End' } elseif ($s.Before -gt 0) { 'before Start' } elseif ($s.After -gt 0) { 'after End' } else { 'outside' }
                    Say ("      {0} x{1}  ({2}; {3})" -f $s.Kernel, $s.Launches, $where, $(if ($s.Clear) { 'the clear kernel' } elseif ($s.Kind -eq 'waiter') { 'waiter/publisher by name' } elseif ($s.Kind -eq 'unknown') { 'name unknown' } else { 'ordinary kernel by name' }))
                }
            }
        }

        # pointer slots of the End kernel across the feature's evaluations (as before)
        Say
        Say $(if ($multiFeature) { "pointer slots of the End kernel of feature $($fa.Feature) (its last launch in each of the feature's evaluations; ptr values only):" } else { 'pointer slots of the End kernel (its last launch in each evaluation; ptr values only):' })
        if (@($fa.EndSlotRows).Count -gt 0) { Say-Table $fa.EndSlotRows @('Slot', 'Offset', 'Stable', 'Values') } else { Say '  none found' }

        # Start / End slots per evaluation and the Start.p1 == End.p0 relation
        Say
        Say 'Start and End parameter slots per evaluation (8-byte values; "-" = not readable):'
        $slotTable = @($fa.Slots | ForEach-Object { [pscustomobject]@{ Eval = $_.Eval; 'Start.p0' = (Format-Ptr $_.StartP0); 'Start.p1' = (Format-Ptr $_.StartP1); 'End.p0' = (Format-Ptr $_.EndP0); 'End.p1' = (Format-Ptr $_.EndP1); 'Start.p1==End.p0' = $(if ($null -eq $_.Link) { '?' } elseif ($_.Link) { 'yes' } else { 'NO' }) } })
        Say-Table $slotTable @('Eval', 'Start.p0', 'Start.p1', 'End.p0', 'End.p1', 'Start.p1==End.p0')

        # who else carries End slot 1 / Start slot 1
        $users = @($fa.EndUsers | ForEach-Object { $_.Users })
        Say
        Say ("launches outside the run that carry End slot 1 or Start slot 1 (feature {0}), and launches of other features' evaluations that carry them:" -f $fa.Feature)
        if ($users.Count -gt 0) {
            Say-Table ($users | Select-Object Eval, Subject, @{ n = 'Value'; e = { Format-Ptr $_.Value } }, Ord, @{ n = 'Position'; e = { $_.Pos } }, Kernel, @{ n = 'Slot'; e = { 'p' + ($_.Off / 8) } }, Kind) @('Eval', 'Subject', 'Value', 'Ord', 'Position', 'Kernel', 'Slot', 'Kind')
        } else { Say '  none' }

        if ($fa.ClearRange) {
            $cr = $fa.ClearRange
            Say ("cc_cb_clear (eval {0}, ord {1}): pointer 0x{2:x}, second parameter {3}; if that is a dword count (inferred, not proved) it clears [0x{2:x}, 0x{4:x}). {5} of {6} distinct pointer values of the Inner kernels lie inside that range, {7} of those are also used by launches outside the run." -f $cr.Eval, $cr.Ord, $cr.Pointer, $cr.Count, $cr.End, $cr.InnerInside, $cr.InnerValues, $cr.InnerInsideAlsoOutside)
        }
        if ($fa.Epoch.Comparable) {
            Say ("epoch scan: {0} Inner launches compared across evaluation(s) {1}: {2} differing 4-byte value(s), {3} moved pointer slot(s)" -f $fa.Epoch.InnerLaunches, ($fa.Evals -join ','), $fa.Epoch.Diffs.Count, $fa.Epoch.MovedPtrs)
            foreach ($d in ($fa.Epoch.Diffs | Select-Object -First 10)) { Say ("  inner #{0} {1} +{2}: {3}" -f $d.Index, $d.Kernel, $d.Offset, $d.Values) }
        } else { Say ("epoch scan: not comparable ({0})" -f $fa.Epoch.Why) }
    }
}
Say

# ---- (d) checks ----
Say '== (d) checks against what the reuse filter and the original verified module expect =='
for ($i = 0; $i -lt $evals.Count; $i++) {
    $e = $evals[$i]
    $r = Get-Range $e
    $label = "eval $($e.N)"
    if ($r.Starts -eq 1 -and $r.Ends -eq 1 -and $r.First -lt $r.Last) { Add-Check $checks "run-$($e.N)" 'PASS' "$label has exactly one Start (cc_vit_1d_repack_2d_to_1d*) before exactly one End (cc_vit_1d_repack_1d_to_2d*)" }
    else { Add-Check $checks "run-$($e.N)" 'FAIL' "$label has $($r.Starts) Start and $($r.Ends) End launches (Start at ord $($r.First), End at ord $($r.Last)); the filter needs exactly one of each, in that order, or it switches reuse off ('unexpected NR launch order')" }
    if ($r.First -ge 0 -and $r.Last -gt $r.First) {
        $inside = @($e.Launches | Where-Object { $_.Ord -gt $r.First -and $_.Ord -lt $r.Last })
        $foreign = @($inside | Where-Object { $_.Role -eq 'none' })
        $gap = ($r.Last - $r.First - 1) - $inside.Count
        if ($foreign.Count -eq 0 -and $gap -eq 0) { Add-Check $checks "contig-$($e.N)" 'PASS' "$label : the $($inside.Count) launches between Start and End are all cc_vit_1d_* inner kernels (contiguous run)" }
        elseif ($foreign.Count -gt 0) { Add-Check $checks "contig-$($e.N)" 'FAIL' "$label : $($foreign.Count) launch(es) of other kernels inside the run, e.g. $(($foreign | Select-Object -First 3 | ForEach-Object { '{0} (ord {1})' -f $_.Kernel, $_.Ord }) -join ', '). A kept foreign kernel inside a dropped run makes the filter turn reuse off, and may wait on dropped work." }
        else { Add-Check $checks "contig-$($e.N)" 'WARN' "$label : $gap launch ordinal(s) inside the run are missing from the capture (cap reached?)" }
    }
}
if ($sameNames) { Add-Check $checks 'sig-names' 'PASS' 'the cc_vit_* launch order is identical in every captured evaluation' }
else { Add-Check $checks 'sig-names' 'FAIL' 'the cc_vit_* launch order differs between captured evaluations (the filter drops by kernel role per evaluation; a varying run is not covered by the checked invariants)' }
if ($sameNames -and -not $sameSizes) { Add-Check $checks 'sig-sizes' 'WARN' 'parameter sizes differ between evaluations for the same order' }
if ($isCapture) {
    if (@($evals | Where-Object { $_.Success -ne $true }).Count -gt 0) { Add-Check $checks 'complete' 'WARN' 'at least one evaluation did not end with success=1' }
    if ($data.Notes.Count -gt 0) { Add-Check $checks 'complete-note' 'WARN' ("the capture stopped early: " + ($data.Notes -join '; ')) }
    if (-not $data.Done) { Add-Check $checks 'complete-done' 'WARN' 'no DONE record: fewer evaluations than the budget were captured (the game exited early or NR did not run 3 times)' }
    $skipped = ($evals | Measure-Object -Property Skipped -Sum).Sum
    if ($skipped -gt 0) { Add-Check $checks 'complete-cap' 'WARN' "$skipped launch(es) were over the per-evaluation cap and not recorded" }
    $nonBlob = @($evals | ForEach-Object { $_.Launches } | Where-Object { $_.Vit -and $_.PMode -ne 'blob' })
    if ($nonBlob.Count -gt 0) { Add-Check $checks 'blob' 'WARN' "$($nonBlob.Count) cc_vit_* launch(es) have no readable parameter blob (pmode ptrs/none): their parameters cannot be analysed" }
    else { Add-Check $checks 'blob' 'PASS' 'every cc_vit_* launch has its raw parameter blob' }
    $trunc = @($evals | ForEach-Object { $_.Launches } | Where-Object { $_.Vit -and $_.Trunc })
    if ($trunc.Count -gt 0) { Add-Check $checks 'trunc' 'WARN' "$($trunc.Count) cc_vit_* parameter blob(s) were cut at 512 bytes" }
    # ---- per-feature checks ----
    foreach ($fa in $analyses) {
        $ft = $fa.Feature
        $nEval = $fa.Evals.Count
        $noOutside = @($fa.Fe | Where-Object { @($_.Launches | Where-Object { -not $_.Vit }).Count -eq 0 })
        $leakN = @($fa.PtrLeaks).Count
        if ($noOutside.Count -gt 0) { Add-Check $checks 'outside' 'WARN' 'no launches outside the cc_vit_* kernels were captured: invariant 1 (nothing outside the run references its counters) cannot be checked' $ft }
        elseif ($leakN -gt 0 -and @($fa.PtrLeaks | Where-Object { $_.Label -ne 'sentinel-clear' }).Count -eq 0) {
            Add-Check $checks 'inv1-leak' 'WARN' "$leakN pointer value(s) are used by an Inner kernel of the run AND by cc_cb_clear only (sentinel-clear: the clear kernel resets the run's flags before Start; whether it does so every evaluation is judged by clear-before, whether anything else waits on them by waiters-outside). Still needs section 6 of the runbook." $ft
        }
        elseif ($leakN -gt 0) {
            $clearN = @($fa.PtrLeaks | Where-Object { $_.Label -eq 'sentinel-clear' }).Count
            Add-Check $checks 'inv1-leak' 'FAIL' "$leakN pointer value(s) are used by an Inner kernel of the run AND by a kernel outside it: shared scratch or a sync counter that dropping the run would leave stale (invariant 1). Needs a closer look.$(if ($clearN -gt 0) { " $clearN of them are shared only with cc_cb_clear (sentinel-clear)." })" $ft
        }
        elseif ($fa.SoftLeaks -gt 0) { Add-Check $checks 'inv1-leak' 'WARN' "$($fa.SoftLeaks) low or unclustered value(s) (lo?/int? in the table) appear both inside the run and outside it; probably integers or small constants, but check them" $ft }
        else { Add-Check $checks 'inv1-leak' 'PASS' 'no pointer-like value is shared between an Inner kernel of the run and a kernel outside it (necessary, not sufficient: see the limits below)' $ft }

        # End slot 0/1 and Start slot 1 across the feature's own evaluations (+ other pointer slots of the End kernel)
        $missingSlots = @($fa.Slots | ForEach-Object { $_.Missing } | Where-Object { $_ })
        if ($nEval -lt 2) { Add-Check $checks 'end-stable' 'INFO' "not judged: only $nEval evaluation(s) of this feature were captured, stability needs 2 or more" $ft }
        elseif ($missingSlots.Count -gt 0) { Add-Check $checks 'end-stable' 'WARN' ("cannot be judged, missing field(s): " + ($missingSlots -join '; ')) $ft }
        else {
            $moving = [System.Collections.Generic.List[string]]::new()
            if ((Get-SlotState $fa.Slots 'EndP0') -ne 'same') { $moving.Add('End slot 0') }
            if ((Get-SlotState $fa.Slots 'EndP1') -ne 'same') { $moving.Add('End slot 1') }
            if ((Get-SlotState $fa.Slots 'StartP1') -ne 'same') { $moving.Add('Start slot 1') }
            foreach ($r in @($fa.EndSlotRows | Where-Object { -not $_.Stable -and $_.Slot -gt 1 })) { $moving.Add("End slot $($r.Slot)") }
            if ($moving.Count -eq 0) { Add-Check $checks 'end-stable' 'PASS' "the End kernel passes the same pointer values in every evaluation of this feature (its output buffer does not rotate) and Start passes the same 1D buffer (slot 1); evaluations $($fa.Evals -join ', ')" $ft }
            else { Add-Check $checks 'end-stable' 'WARN' "$($moving -join ', ') change between evaluations of this feature (evaluations $($fa.Evals -join ', ')): if one of them is the output buffer, a reused result would sit in another buffer" $ft }
        }
        $linkKnown = @($fa.Slots | Where-Object { $null -ne $_.Link })
        $linkYes = @($linkKnown | Where-Object { $_.Link })
        if ($linkKnown.Count -eq 0) { Add-Check $checks 'start-end-link' 'INFO' 'Start.p1 == End.p0 cannot be read (no readable Start/End parameters)' $ft }
        elseif ($linkYes.Count -eq $linkKnown.Count) { Add-Check $checks 'start-end-link' 'INFO' "Start.p1 == End.p0 in $($linkYes.Count) of $($linkKnown.Count) evaluation(s): the buffer Start fills is the buffer End reads" $ft }
        else { Add-Check $checks 'start-end-link' 'INFO' "Start.p1 == End.p0 in only $($linkYes.Count) of $($linkKnown.Count) evaluation(s): the End kernel reads another buffer than the one Start fills, so Start slot 1 does not predict End's input or output" $ft }

        # a launch outside the run that shares a LEAK pointer and polls or publishes flags by its name
        if (-not $fa.Classifiable) { Add-Check $checks 'waiters-outside' 'WARN' 'cannot be evaluated: the run or the launches outside it are not in the capture' $ft }
        else {
            $wl = @($fa.PtrLeaks | Where-Object { $_.WaiterSharers.Count -gt 0 })
            if ($wl.Count -gt 0) {
                $detail = ($wl | Select-Object -First 3 | ForEach-Object { '{0}: {1}' -f $_.Value, ((@($_.WaiterSharers | ForEach-Object { $_.Kernel }) | Select-Object -First 4) -join ', ') }) -join ' | '
                Add-Check $checks 'waiters-outside' 'FAIL' "$($wl.Count) LEAK pointer(s) are shared with launches outside the run whose names say they poll or publish flags (*_wait, *_chained, *_tilesync, *_publish) or are unknown. A kept waiter next to a dropped run could hang the GPU: this is the nvdisasm trigger. $detail" $ft
            }
            elseif ($leakN -eq 0) { Add-Check $checks 'waiters-outside' 'PASS' 'there is no LEAK pointer, so no launch outside the run shares a pointer with it' $ft }
            else { Add-Check $checks 'waiters-outside' 'PASS' "no launch outside the run that shares one of the $leakN LEAK pointer(s) is named like a waiter or publisher (*_wait, *_chained, *_tilesync, *_publish) or unknown" $ft }
        }

        # cc_cb_clear before Start in every evaluation, and its pointer is a LEAK pointer
        if (-not $fa.Classifiable) { Add-Check $checks 'clear-before' 'WARN' 'cannot be evaluated: the run or the launches outside it are not in the capture' $ft }
        elseif ($leakN -eq 0) { Add-Check $checks 'clear-before' 'INFO' 'n/a: no LEAK pointer, so there is nothing shared with cc_cb_clear (the fixed rule still asks for PASS, so this does not satisfy it)' $ft }
        else {
            $badClear = @($fa.Clears | Where-Object { $_.Before -lt 1 -or -not $_.Match })
            if ($badClear.Count -eq 0) { Add-Check $checks 'clear-before' 'PASS' "cc_cb_clear is launched before Start in every evaluation ($($fa.Evals -join ', ')) and its pointer ($(($fa.Clears | ForEach-Object { $_.Match } | Select-Object -Unique) -join ', ')) equals a LEAK pointer" $ft }
            else {
                $noBefore = @($badClear | Where-Object { $_.Before -lt 1 })
                $why = if ($noBefore.Count -gt 0) { "no cc_cb_clear launch before Start in eval $(($noBefore | ForEach-Object { $_.Eval }) -join ', ')" } else { "cc_cb_clear is launched before Start, but its pointer is not one of the $leakN LEAK pointer(s) (eval $(($badClear | ForEach-Object { $_.Eval }) -join ', '))" }
                Add-Check $checks 'clear-before' 'FAIL' "${why}: the LEAK pointers are not shown to be reset by the clear kernel before each evaluation" $ft
            }
        }
        if ($fa.ClearRange) {
            $cr = $fa.ClearRange
            Add-Check $checks 'clear-range' 'INFO' ("cc_cb_clear (eval {0}, ord {1}) pointer 0x{2:x}, second parameter {3}: if that is a dword count (inferred) it covers [0x{2:x}, 0x{4:x}); {5} of {6} distinct pointer values of the Inner kernels lie inside, {7} of those are also used outside the run. Context only: the fixed rule compares the pointer value itself" -f $cr.Eval, $cr.Ord, $cr.Pointer, $cr.Count, $cr.End, $cr.InnerInside, $cr.InnerValues, $cr.InnerInsideAlsoOutside) $ft
        }

        # a 4-byte value in an Inner kernel's parameters that differs between two evaluations of the feature
        if (-not $fa.Epoch.Comparable) { Add-Check $checks 'epoch' 'INFO' "not judged: $($fa.Epoch.Why)" $ft }
        elseif ($fa.Epoch.Diffs.Count -gt 0) {
            $ex = ($fa.Epoch.Diffs | Select-Object -First 3 | ForEach-Object { '{0} (inner #{1}) +{2}: {3}' -f $_.Kernel, $_.Index, $_.Offset, $_.Values }) -join ' | '
            Add-Check $checks 'epoch' 'WARN' "$($fa.Epoch.Diffs.Count) 4-byte value(s) in the parameters of Inner kernels differ between evaluations of this feature (a counter or epoch passed in parameters?): $ex" $ft
        }
        else { Add-Check $checks 'epoch' 'PASS' "no 4-byte value in the parameters of the $($fa.Epoch.InnerLaunches) Inner launches differs between evaluations $($fa.Evals -join ', ') of this feature ($($fa.Epoch.MovedPtrs) moved pointer slot(s) not counted)" $ft }

        # who else touches End slot 1 / Start slot 1
        $allUsers = @($fa.EndUsers | ForEach-Object { $_.Users })
        $endBefore = @($allUsers | Where-Object { $_.Subject -eq 'End.p1' -and $_.Pos -eq 'before Start' })
        $endAfter = @($allUsers | Where-Object { $_.Subject -eq 'End.p1' -and $_.Pos -eq 'after End' })
        $startOut = @($allUsers | Where-Object { $_.Subject -eq 'Start.p1' -and ($_.Pos -eq 'before Start' -or $_.Pos -eq 'after End') })
        $otherEval = @($allUsers | Where-Object { $_.Pos -like 'evaluation*' })
        $unreadable = ($fa.EndUsers | Measure-Object -Property UnreadableBefore -Sum).Sum + ($fa.EndUsers | Measure-Object -Property UnreadableAfter -Sum).Sum
        $names = { param($list) (@($list | ForEach-Object { $_.Kernel } | Select-Object -Unique) | Select-Object -First 5) -join ', ' }
        if (@($fa.Slots | Where-Object { $null -eq $_.EndP1 }).Count -gt 0) { Add-Check $checks 'end-users' 'WARN' ("cannot be evaluated, missing field(s): " + ($missingSlots -join '; ')) $ft }
        elseif ($endBefore.Count -gt 0) { Add-Check $checks 'end-users' 'FAIL' "$($endBefore.Count) launch(es) BEFORE Start carry End slot 1: $(& $names $endBefore). A kernel that uses the output buffer as scratch before the run would put its data into a skipped result." $ft }
        elseif ($endAfter.Count -gt 0 -or $startOut.Count -gt 0 -or $unreadable -gt 0) {
            $parts = @()
            if ($endAfter.Count -gt 0) { $parts += "after End, End slot 1 is also used by: $(& $names $endAfter)" }
            if ($startOut.Count -gt 0) { $parts += "Start slot 1 is also used outside the run by: $(& $names $startOut)" }
            if ($unreadable -gt 0) { $parts += "$unreadable launch(es) outside the run have no complete parameter blob" }
            Add-Check $checks 'end-users' 'WARN' (($parts -join '; ') + '. Whether they only read the buffer is not in the files (parameters are bytes): the fixed rule needs a static decode of each.') $ft
        }
        else { Add-Check $checks 'end-users' 'PASS' 'no launch outside the run carries End slot 1 or Start slot 1' $ft }
        if ($multiFeature) {
            if ($otherEval.Count -gt 0) { Add-Check $checks 'end-xfeature' 'WARN' "End slot 1 or Start slot 1 of this feature also appears in evaluation(s) of another feature: $(& $names $otherEval). Another pass would share the buffer (not part of the fixed rule)." $ft }
            else { Add-Check $checks 'end-xfeature' 'PASS' "no launch of another feature's evaluations carries End slot 1 or Start slot 1 of this feature" $ft }
        }
    }
}

# hybrid FFN layout hypothesis (what DlssNrNative.cpp hard-codes for the original module; informational, not used for reuse)
$ffn = @($first.Launches | Where-Object { Get-FfnKind $_.Kernel })
if ($ffn.Count -gt 0) {
    $psizeBad = @($ffn | Where-Object { $_.PSize -ne 72 })
    Add-Check $checks 'ffn-abi' $(if ($psizeBad.Count -eq 0) { 'INFO' } else { 'INFO' }) ("FFN kernels (expand_publish, expand_chained, contract_chained): {0} launch(es), parameter size {1} (the original module uses 72)" -f $ffn.Count, (@($ffn | ForEach-Object { $_.PSize } | Sort-Object -Unique) -join ','))
    $kExpand = @($ffn | Where-Object { (Get-FfnKind $_.Kernel) -eq 'expand' } | ForEach-Object { if ($_.Grid[0] % 32 -eq 0) { $_.Grid[0] / 32 } else { -1 } } | Sort-Object -Unique)
    $kContract = @($ffn | Where-Object { (Get-FfnKind $_.Kernel) -eq 'contract' } | ForEach-Object { if ($_.Grid[0] % 8 -eq 0) { $_.Grid[0] / 8 } else { -1 } } | Sort-Object -Unique)
    Add-Check $checks 'ffn-geometry' 'INFO' ("hybrid geometry rule of the original module: expand grid.x = 32*ceil(M/128), contract grid.x = 8*ceil(M/128), z = 4, block 32x4x1, no dynamic shared memory. Here: ceil(M/128) implied by expand grid = {0}, by contract grid = {1}; contract z = {2}; blocks = {3}; shared = {4}" -f ($kExpand -join ','), ($kContract -join ','), (@($ffn | Where-Object { (Get-FfnKind $_.Kernel) -eq 'contract' } | ForEach-Object { $_.Grid[2] } | Sort-Object -Unique) -join ','), (@($ffn | ForEach-Object { $_.Block -join 'x' } | Sort-Object -Unique) -join ' '), (@($ffn | ForEach-Object { $_.Shared } | Sort-Object -Unique) -join ','))
    if ($isCapture -and @($ffn | Where-Object { $_.PMode -eq 'blob' -and $_.PSize -eq 72 }).Count -eq $ffn.Count) {
        $okSlots = 0; $pairs = 0; $detail = [System.Collections.Generic.List[string]]::new()
        $ordered = @($ffn | Sort-Object Ord)
        for ($i = 0; $i -lt $ordered.Count - 1; $i++) {
            $a = $ordered[$i]; $b = $ordered[$i + 1]
            if ((Get-FfnKind $a.Kernel) -ne 'expand' -or (Get-FfnKind $b.Kernel) -ne 'contract') { continue }
            $pairs++
            $r1 = ($b.QWords[0] -eq $a.QWords[2]); $r2 = ($b.QWords[1] -eq $a.QWords[0]); $r3 = ($b.QWords[6] -eq $a.QWords[7])
            if ($r1 -and $r2 -and $r3) { $okSlots++ }
            if ($detail.Count -lt 3) { $detail.Add(("expand ord {0} -> contract ord {1}: contract.p0==expand.p2 {2}, contract.p1==expand.p0 {3}, contract.p6==expand.p7 {4}" -f $a.Ord, $b.Ord, $r1, $r2, $r3)) }
        }
        $verdict = if ($pairs -eq 0) { 'no expand->contract pair found' } elseif ($okSlots -eq $pairs) { "all $pairs pair(s) match the original layout hypothesis" } else { "$okSlots of $pairs pair(s) match the original layout hypothesis" }
        Add-Check $checks 'ffn-slots' 'INFO' ("72-byte FFN slot hypothesis from the original module (expand: p0 x, p2 y, p7 done; contract: p0 y, p1 x, p2 output, p4 counter, p6 expand's done, p7 done): $verdict. " + ($detail -join ' | '))
    }
}

# comparison with the original verified module and with a baseline
Add-Check $checks 'old-module' 'INFO' 'original verified module (3,202,680 bytes): no capture of its launch order exists in the repository or the tests, so there is nothing to diff against. What the code fixes: one Start (repack_2d_to_1d*), one End (repack_1d_to_2d*), only cc_vit_1d_* kernels between, and for the hybrid path the three FFN kernels with 72-byte parameters (checked above).'
if ($Baseline) {
    $bpath = $Baseline
    $bdata = if ((Test-Path -LiteralPath $bpath -PathType Leaf) -and $bpath -like '*.log') { Read-LogFile (Resolve-Path -LiteralPath $bpath).Path } else { Read-CaptureFile (Resolve-CapturePath $bpath) }
    # Compared on the cc_vit_1d_* family, which is what both a capture and the plain log carry.
    $bsig = Get-VitSignature $bdata.Evals[0]
    $nsig = $sigs[0]
    if ($bsig.FamilyHash -eq $nsig.FamilyHash) { Add-Check $checks 'baseline' 'PASS' "cc_vit_1d_* order is identical to the baseline ($($bdata.Source)): $($nsig.FamilyHash)" }
    else {
        $n = [math]::Min($bsig.Family.Count, $nsig.Family.Count); $d = 0
        while ($d -lt $n -and $bsig.Family[$d] -eq $nsig.Family[$d]) { $d++ }
        $here = if ($d -lt $nsig.Family.Count) { $nsig.Family[$d] } else { '(end)' }
        $there = if ($d -lt $bsig.Family.Count) { $bsig.Family[$d] } else { '(end)' }
        Add-Check $checks 'baseline' 'FAIL' "cc_vit_1d_* order differs from the baseline ($($bdata.Source)): $($nsig.Family.Count) vs $($bsig.Family.Count) launches, first difference at position $d ($here here, $there in the baseline)"
    }
}
foreach ($c in $checks) { Say ("CHECK {0,-5} {1,-13} {2}{3}" -f $c.Status, $c.Id, $(if ($multiFeature -and $null -ne $c.Feature) { "feature $($c.Feature): " } else { '' }), $c.Text) }
Say
$ruleResults = @()
if ($isCapture) {
    Say '== (e) the fixed go/no-go rule of STUDY-nr-flicker.md (Step A), applied per NR pass feature =='
    Say 'GO needs every condition to PASS for pass 0 (feature 0, the feature used first): (1) End slot 1 and Start slot 1 identical in all of its evaluations (2 or more); (2) every LEAK pointer is sentinel-clear and no outside waiter shares one; (3) clear-before PASS; (4) no epoch WARN; (5) no launch before Start carries End slot 1, and every launch after End that carries it only reads it (needs a static decode: the files cannot show it).'
    foreach ($fa in $analyses) {
        $rr = Get-RuleResult $fa
        $ruleResults += $rr
        Say ("RULE {0,-14} feature {1} (evaluation(s) {2}; VitEvery as applied {3}{4})" -f $rr.Verdict, $rr.Feature, ($fa.Evals -join ', '), ($fa.EveryValues -join '/'), $(if ($fa.WatchedOnly) { ', watched only' } else { '' }))
        foreach ($cd in $rr.Conditions) { Say ("    {0,-8} {1,-22} {2}" -f $cd.Status, $cd.Id, $cd.Text) }
    }
    Say 'GO = every condition passes. NEEDS-DECODE = every condition passes except that the launches after End that carry End slot 1 are not yet shown to only read it. NEEDS-NVDISASM = no final failure, but an outside waiter/publisher shares a LEAK pointer (the study''s nvdisasm trigger). NO-GO = a condition failed or could not be evaluated from the files (n/a and UNKNOWN count as not satisfied). NOT-JUDGED = fewer than 2 evaluations of the feature.'
    Say
}

Say '== what this cannot tell you =='
Say '- Whether a buffer is read or written by a kernel: the parameters are bytes, not a signature. A shared pointer is a hint, not a dependency.'
Say '- Which shared pointer is a sync counter and which is data; whether a kernel waits on a counter that a dropped kernel signals.'
Say '- Whether the End kernel writes the only copy of the result, or whether anything after the run reads buffers the run also writes.'
Say '- Anything about values that are not device pointers (sizes, strides, flags, scales).'
Say '- Whether a launch after End that carries End slot 1 only reads it: that needs the kernel code (static decode), the parameters do not say.'
Say '- Whether cc_cb_clear''s second parameter is a dword count (the clear-range line assumes it), and which feature is pass 0 (the feature ordinal is the order of first use; pass 0 is evaluated first in a frame, from the code).'
Say '- Pointer detection is a heuristic on 8-byte aligned values (see -PointerMin/-PointerMax and the high-dword histogram); sub-allocations and unusual address ranges can hide or invent candidates.'
Say 'Settling those needs the kernel code (SASS of the dumped module with nvdisasm/cuobjdump, which are not installed: ask before downloading anything) or a probe session.'

$out | ForEach-Object { Write-Output $_ }

$failed = @($checks | Where-Object { $_.Status -eq 'FAIL' }).Count
if ($PassThru) {
    [pscustomobject]@{ Source = $data.Source; Kind = $data.Kind; Evaluations = $evals.Count; NamesHash = $sigs[0].NamesHash; SizesHash = $sigs[0].SizesHash; FamilyHash = $sigs[0].FamilyHash; SameNames = $sameNames; Checks = $checks.ToArray(); Pointers = @($pointerRows | Select-Object Value, Conf, Launches, Kernels, Inner, Edge, Outside, Stable, Class, Label, Where); Rules = $ruleResults; Failed = $failed }
}
if ($Strict -and $failed -gt 0) { exit 1 }
