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
    return [pscustomobject]@{
        Eval = $eval; Ord = [int]$kv['ord']; Fn = $kv['fn']; Kernel = $kv['kernel']
        Vit = ($kv['vit'] -eq '1'); Role = $kv['role']; Abi = $kv['abi']
        Grid = (ConvertTo-Dim $kv['grid']); Block = (ConvertTo-Dim $kv['block']); Shared = [int]$kv['shared']
        PSize = $psize; PMode = $kv['pmode']; PBytes = $pbytes; Trunc = ($kv['trunc'] -eq '1'); Hex = $hex
        QWords = (Get-QWords $hex)
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

function Add-Check($list, [string]$id, [string]$status, [string]$text) {
    $list.Add([pscustomobject]@{ Id = $id; Status = $status; Text = $text })
}

function Get-FfnKind([string]$name) {
    if ($name -like '*ffn_expand_publish*' -or $name -like '*ffn_expand_chained*') { return 'expand' }
    if ($name -like '*ffn_contract_chained*') { return 'contract' }
    return $null
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
$pointerRows = @()
$leaks = 0
$softLeaks = 0
$endSlots = @()
if (-not $isCapture) {
    Say 'not available from the plain log (no parameter bytes). Use a VitCapture capture.'
} else {
    $range = Get-Range $first
    $hasOutside = @($first.Launches | Where-Object { -not $_.Vit }).Count -gt 0

    # Which high dwords are real address clusters: device pointers come in bulk (every weight buffer, scratch buffer and counter
    # shares a few high dwords), while a packed pair of 32-bit integers (width/height) lands in a high dword of its own.
    $byHigh = @{}
    foreach ($l in $first.Launches) {
        if ($l.PMode -ne 'blob') { continue }
        foreach ($q in $l.QWords) {
            $v = [UInt64]$q
            if ($v -lt [UInt64]4294967296 -or $v -lt $PointerMin -or $v -ge $PointerMax) { continue }
            $h = [UInt32]($v -shr 32)
            if (-not $byHigh.ContainsKey($h)) { $byHigh[$h] = [System.Collections.Generic.HashSet[UInt64]]::new() }
            [void]$byHigh[$h].Add($v)
        }
    }
    $clusterHi = [System.Collections.Generic.HashSet[UInt32]]::new()
    foreach ($h in $byHigh.Keys) { if ($byHigh[$h].Count -ge $ClusterMin) { [void]$clusterHi.Add([UInt32]$h) } }

    # 'ptr' = in range and in an address cluster; 'lo?' = below 4 GiB and 16-byte aligned; 'int?' = in range, unclustered
    # high dword (probably two packed integers); $null = not a candidate.
    function Get-PointerConf([UInt64]$v) {
        if ($v -lt $PointerMin -or $v -ge $PointerMax) { return $null }
        if ($v -lt [UInt64]4294967296) { if (($v % 16) -ne 0) { return $null } else { return 'lo?' } }
        if ($clusterHi.Contains([UInt32]($v -shr 32))) { return 'ptr' } else { return 'int?' }
    }

    $occ = @{}
    foreach ($l in $first.Launches) {
        if ($l.PMode -ne 'blob') { continue }
        for ($q = 0; $q -lt $l.QWords.Count; $q++) {
            $v = [UInt64]$l.QWords[$q]
            if ($null -eq (Get-PointerConf $v)) { continue }
            $inside = $range.First -ge 0 -and $range.Last -ge 0 -and $l.Ord -ge $range.First -and $l.Ord -le $range.Last
            $key = '0x{0:x}' -f $v
            if (-not $occ.ContainsKey($key)) { $occ[$key] = [System.Collections.Generic.List[object]]::new() }
            $occ[$key].Add([pscustomobject]@{ Ord = $l.Ord; Kernel = $l.Kernel; Role = $l.Role; Off = 8 * $q; Inside = $inside; Vit = $l.Vit })
        }
    }
    $laterSets = @()
    foreach ($e in ($evals | Select-Object -Skip 1)) {
        $set = [System.Collections.Generic.HashSet[string]]::new()
        foreach ($l in $e.Launches) { if ($l.PMode -eq 'blob') { foreach ($v in $l.QWords) { [void]$set.Add(('0x{0:x}' -f [UInt64]$v)) } } }
        $laterSets += , $set
    }
    $hidden = 0
    foreach ($key in $occ.Keys) {
        $list = @($occ[$key])
        if ($list.Count -lt 2) { continue }
        $v = [Convert]::ToUInt64($key.Substring(2), 16)
        $conf = Get-PointerConf $v
        if ($conf -eq 'int?' -and -not $ShowUnlikely) { $hidden++; continue }
        $kernels = @($list | ForEach-Object { $_.Kernel } | Sort-Object -Unique)
        $inner = @($list | Where-Object { $_.Inside -and $_.Role -eq 'inner' }).Count
        $edge = @($list | Where-Object { $_.Inside -and $_.Role -in @('start', 'end') }).Count
        $outside = @($list | Where-Object { -not $_.Inside }).Count
        $class = if (-not $hasOutside) { 'unknown (no launches outside the ViT kernels captured)' }
                 elseif ($range.First -lt 0 -or $range.Last -lt 0) { 'unknown (no Start..End run)' }
                 elseif ($inner -gt 0 -and $outside -gt 0) { 'LEAK: inside the run and outside it' }
                 elseif ($edge -gt 0 -and $outside -gt 0 -and $inner -eq 0) { 'boundary: Start/End and outside' }
                 elseif ($outside -eq 0) { 'run-private: only inside Start..End' }
                 else { 'outside the run only' }
        if ($class -like 'LEAK*') { if ($conf -eq 'ptr') { $leaks++ } else { $softLeaks++ } }
        $stable = if ($laterSets.Count -eq 0) { 'n/a' } else { if (@($laterSets | Where-Object { -not $_.Contains($key) }).Count -eq 0) { 'yes' } else { 'NO' } }
        $pointerRows += [pscustomobject]@{
            Value = $key; Conf = $conf; Launches = $list.Count; Kernels = $kernels.Count
            Inner = $inner; Edge = $edge; Outside = $outside; Stable = $stable; Class = $class
            Where = ((($list | Select-Object -First 4) | ForEach-Object { '{0}@+{1}' -f (Get-ShortName $_.Kernel), $_.Off }) -join ', ') + $(if ($list.Count -gt 4) { ', ...' } else { '' })
        }
    }
    $pointerRows = @($pointerRows | Sort-Object @{ Expression = { $_.Conf -ne 'ptr' } }, @{ Expression = 'Kernels'; Descending = $true }, @{ Expression = 'Launches'; Descending = $true }, Value)
    Say ("candidates: {0} value(s) in {1}..{2} used by 2+ launches of eval {3}. ptr = in an address cluster, lo? = below 4 GiB (aligned, uncertain), int? = unclustered high dword, probably packed integers" -f $pointerRows.Count, ('0x{0:x}' -f $PointerMin), ('0x{0:x}' -f $PointerMax), $first.N)
    if ($hidden -gt 0) { Say ("{0} int? value(s) hidden (-ShowUnlikely lists them)" -f $hidden) }
    if ($byHigh.Count -gt 0) {
        $top = $byHigh.GetEnumerator() | Sort-Object { $_.Value.Count } -Descending | Select-Object -First 6
        Say ('high dwords of the values in 2^32..2^48 with their distinct values (address clusters have {0}+): ' -f $ClusterMin + (($top | ForEach-Object { '0x{0:x8} x{1}' -f $_.Key, $_.Value.Count }) -join ', '))
    }
    if ($pointerRows.Count -gt 0) {
        Say-Table ($pointerRows | Select-Object -First $MaxRows) @('Value', 'Conf', 'Launches', 'Kernels', 'Inner', 'Edge', 'Outside', 'Stable', 'Class', 'Where')
        if ($pointerRows.Count -gt $MaxRows) { Say ("... {0} more rows (raise -MaxRows)" -f ($pointerRows.Count - $MaxRows)) }
    }
    Say 'Inner = launches strictly between Start and End, Edge = the Start or End launch, Outside = every other launch of the evaluation.'
    # pointer slots of the End kernel across evaluations
    $endLaunches = @($evals | ForEach-Object { @($_.Launches | Where-Object { $_.Role -eq 'end' -and $_.PMode -eq 'blob' } | Select-Object -Last 1) })
    if ($endLaunches.Count -eq $evals.Count -and $endLaunches.Count -gt 0) {
        $slots = [math]::Min(@($endLaunches | ForEach-Object { $_.QWords.Count } | Measure-Object -Minimum).Minimum, 64)
        for ($q = 0; $q -lt $slots; $q++) {
            $vals = @($endLaunches | ForEach-Object { [UInt64]$_.QWords[$q] })
            $isPtr = @($vals | Where-Object { (Get-PointerConf $_) -eq 'ptr' }).Count -eq $vals.Count
            if ($isPtr) { $endSlots += [pscustomobject]@{ Slot = $q; Offset = 8 * $q; Stable = (@($vals | Sort-Object -Unique).Count -eq 1); Values = (($vals | ForEach-Object { '0x{0:x}' -f $_ }) -join ' ') } }
        }
        Say
        Say 'pointer slots of the End kernel (its last launch in each evaluation; ptr values only):'
        if ($endSlots.Count -gt 0) { Say-Table $endSlots @('Slot', 'Offset', 'Stable', 'Values') } else { Say '  none found' }
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
    if (@($first.Launches | Where-Object { -not $_.Vit }).Count -eq 0) { Add-Check $checks 'outside' 'WARN' 'no launches outside the cc_vit_* kernels were captured: invariant 1 (nothing outside the run references its counters) cannot be checked' }
    elseif ($leaks -gt 0) { Add-Check $checks 'inv1-leak' 'FAIL' "$leaks pointer value(s) are used by an Inner kernel of the run AND by a kernel outside it: shared scratch or a sync counter that dropping the run would leave stale (invariant 1). Needs a closer look." }
    elseif ($softLeaks -gt 0) { Add-Check $checks 'inv1-leak' 'WARN' "$softLeaks low or unclustered value(s) (lo?/int? in the table) appear both inside the run and outside it; probably integers or small constants, but check them" }
    else { Add-Check $checks 'inv1-leak' 'PASS' 'no pointer-like value is shared between an Inner kernel of the run and a kernel outside it (necessary, not sufficient: see the limits below)' }
    if ($endSlots.Count -gt 0) {
        $moving = @($endSlots | Where-Object { -not $_.Stable })
        if ($moving.Count -eq 0) { Add-Check $checks 'end-stable' 'PASS' 'the End kernel passes the same pointer values in every evaluation (its output buffer does not rotate)' }
        else { Add-Check $checks 'end-stable' 'WARN' "End kernel pointer slot(s) $(($moving | ForEach-Object { $_.Slot }) -join ',') change between evaluations: if one of them is the output buffer, a reused result would sit in another buffer" }
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
foreach ($c in $checks) { Say ("CHECK {0,-5} {1,-13} {2}" -f $c.Status, $c.Id, $c.Text) }
Say

Say '== what this cannot tell you =='
Say '- Whether a buffer is read or written by a kernel: the parameters are bytes, not a signature. A shared pointer is a hint, not a dependency.'
Say '- Which shared pointer is a sync counter and which is data; whether a kernel waits on a counter that a dropped kernel signals.'
Say '- Whether the End kernel writes the only copy of the result, or whether anything after the run reads buffers the run also writes.'
Say '- Anything about values that are not device pointers (sizes, strides, flags, scales).'
Say '- Pointer detection is a heuristic on 8-byte aligned values (see -PointerMin/-PointerMax and the high-dword histogram); sub-allocations and unusual address ranges can hide or invent candidates.'
Say 'Settling those needs the kernel code (SASS of the dumped module with nvdisasm/cuobjdump, which are not installed: ask before downloading anything) or a probe session.'

$out | ForEach-Object { Write-Output $_ }

$failed = @($checks | Where-Object { $_.Status -eq 'FAIL' }).Count
if ($PassThru) {
    [pscustomobject]@{ Source = $data.Source; Kind = $data.Kind; Evaluations = $evals.Count; NamesHash = $sigs[0].NamesHash; SizesHash = $sigs[0].SizesHash; FamilyHash = $sigs[0].FamilyHash; SameNames = $sameNames; Checks = $checks.ToArray(); Pointers = $pointerRows; Failed = $failed }
}
if ($Strict -and $failed -gt 0) { exit 1 }
