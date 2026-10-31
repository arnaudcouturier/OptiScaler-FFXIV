<#
.SYNOPSIS
  Turns the probe host's output into the compiled release-model rows (OptiScaler\ext\fg\outswitch\ModelRows.h).
.DESCRIPTION
  Two files take part:

    model-rows.txt        the data: one line per class and library build, written by a person from the host's output
                          (class=, file=, size=, stamp=, config=, verdict=, witness=, refs=, and for XeFG the companion
                          DLL). This is the reviewed record.
    ModelRows.h           the generated C++ array the product compiles in. Only Seeded() reads it.

  With -Source, the host's saved output is read first: every "fg-probe model class=..." line replaces the matching row
  in the data file (same class, file, size and stamp) or appends a new one. Then the header is regenerated.

  With -Check, nothing is written; the script exits 1 when the header does not match the data file. The test
  fg_switch_model parses the data file and compares it with the compiled rows as well.

  Usage:
    pwsh -NoProfile -File tools\fg-probe\Update-ModelTable.ps1 -Source <saved probe output>
    pwsh -NoProfile -File tools\fg-probe\Update-ModelTable.ps1 -Check
#>
[CmdletBinding()]
param(
    [string]$Source = '',
    [string]$Data = '',
    [string]$Header = '',
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Data) { $Data = Join-Path $PSScriptRoot 'model-rows.txt' }
if (-not $Header) { $Header = Join-Path $root 'OptiScaler\ext\fg\outswitch\ModelRows.h' }

function Normalise-RowText([string]$line) {
    # One answer, one spelling: the host used to print 'witness=not honoured', which the token split below reads as
    # 'witness=not' plus a stray word. Saved output from that build still has to be usable, so the two spellings collapse
    # into the one the data file takes.
    return ($line -replace 'witness=not(\s+honoured)?\b', 'witness=no')
}

function Parse-Hex([string]$text, [string]$what, [string]$line) {
    $value = 0
    if (-not [uint32]::TryParse(($text -replace '^0[xX]', ''), [System.Globalization.NumberStyles]::HexNumber, $null, [ref]$value) -or $value -eq 0) {
        throw "row $what is not a hex PE timestamp: $line"
    }
    return $value
}

function Parse-Row([string]$line) {
    # class=<fsrfg|xefg> file=<name> size=<bytes> stamp=<hex> config=<direct|xell> verdict=<M1|M2> witness=<honoured|no>
    # order=<destroyfirst|pinfirst> refs=<count> [companionFile=<name> companionSize=<bytes> companionStamp=<hex> for xefg]
    # [ctx=<destroyed|refused|unknown> orderB=<clean|leaked|unknown> vramDeltaMb=<signed>]
    $fields = @{}
    foreach ($token in ($line -split '\s+')) {
        if ($token -match '^([a-zA-Z]+)=(.+)$') { $fields[$Matches[1]] = $Matches[2] }
    }
    foreach ($key in 'class', 'file', 'size', 'stamp', 'config', 'verdict', 'witness', 'order', 'refs') {
        if (-not $fields.ContainsKey($key)) { throw "row field '$key' missing: $line" }
    }
    if ($fields['class'] -notin @('fsrfg', 'xefg')) { throw "row class must be fsrfg or xefg: $line" }
    if ($fields['verdict'] -notin @('M1', 'M2')) { throw "row verdict must be M1 or M2 (unknown is not a row): $line" }
    if ($fields['witness'] -notin @('honoured', 'no')) { throw "row witness must be honoured or no: $line" }
    # M2 says the context holds a reference and the pin release ends the object. Without a witness the library accepted,
    # the row carries no proof that the object outlived the destroy, and a switch would release the pin on the table's
    # word alone: the one direction that touches freed memory. Such a row is not admitted.
    if ($fields['verdict'] -eq 'M2' -and $fields['witness'] -ne 'honoured') {
        throw "M2 row without a witness the library accepted (verdict=M2 needs witness=honoured): $line"
    }
    # The product runs XeFG with an XeLL context attached and FSR FG directly; a row measured in the other configuration
    # describes a setup the product never runs and cannot authorise a release rule.
    $wantedConfig = if ($fields['class'] -eq 'xefg') { 'xell' } else { 'direct' }
    if ($fields['config'] -ne $wantedConfig) {
        throw "row config for $($fields['class']) must be $wantedConfig (the configuration the product runs): $line"
    }
    # The order the row was measured with. A row is only consumed for its own order: the two free the same object
    # differently, and a pin-first row releases the caller's reference before the destroy.
    if ($fields['order'] -notin @('destroyfirst', 'pinfirst')) { throw "row order must be destroyfirst or pinfirst: $line" }
    if ($fields['order'] -eq 'pinfirst' -and ($fields['verdict'] -ne 'M2' -or $fields['witness'] -ne 'honoured')) {
        throw "a pin-first row needs verdict=M2 and witness=honoured: $line"
    }
    $size = 0
    if (-not [uint64]::TryParse($fields['size'], [ref]$size) -or $size -eq 0) { throw "row size is not a positive number: $line" }
    $stamp = Parse-Hex $fields['stamp'] 'stamp' $line

    # The reference count at standstill is part of the row: the preflight compares the live chain against it when the
    # in-game probe did not run, and it is how a switch notices a holder that appeared after the measurement.
    $refs = 0
    if (-not [uint32]::TryParse($fields['refs'], [ref]$refs) -or $refs -eq 0) { throw "row refs is not a positive count: $line" }

    # XeFG runs with a companion DLL (the XeLL context). Its build is part of the pair the row measured: another
    # libxell.dll next to the same libxess_fg.dll is a different measurement, not the same row.
    $companionFile = ''
    $companionSize = 0
    $companionStamp = 0
    if ($fields['class'] -eq 'xefg') {
        foreach ($key in 'companionFile', 'companionSize', 'companionStamp') {
            if (-not $fields.ContainsKey($key)) { throw "row field '$key' missing for xefg: $line" }
        }
        $companionFile = $fields['companionFile']
        if (-not [uint64]::TryParse($fields['companionSize'], [ref]$companionSize) -or $companionSize -eq 0) {
            throw "row companionSize is not a positive number: $line"
        }
        $companionStamp = Parse-Hex $fields['companionStamp'] 'companionStamp' $line
    }

    # Informational answers from the same run, never a decision: what the library answered to a destroy of the context
    # once the pin was released (refused means the handle was dropped and nothing measured the memory it still holds),
    # whether the release-then-destroy order leaves the window free, and how much local video memory the run held
    # afterwards. The words are what was observed, so a reader never takes a guess for a measurement.
    $ctx = if ($fields.ContainsKey('ctx')) { $fields['ctx'] } else { 'unknown' }
    if ($ctx -notin @('destroyed', 'refused', 'unknown')) { throw "row ctx must be destroyed, refused or unknown: $line" }
    $orderB = if ($fields.ContainsKey('orderB')) { $fields['orderB'] } else { 'unknown' }
    if ($orderB -notin @('clean', 'leaked', 'unknown')) { throw "row orderB must be clean, leaked or unknown: $line" }
    $vram = 0
    if ($fields.ContainsKey('vramDeltaMb') -and -not [int]::TryParse($fields['vramDeltaMb'], [ref]$vram)) {
        throw "row vramDeltaMb is not a signed number: $line"
    }

    return [pscustomobject]@{
        Class          = $fields['class']
        File           = $fields['file']
        Size           = $size
        Stamp          = $stamp
        CompanionFile  = $companionFile
        CompanionSize  = $companionSize
        CompanionStamp = $companionStamp
        Config         = $fields['config']
        Verdict        = $fields['verdict']
        Witness        = ($fields['witness'] -eq 'honoured')
        PinFirst       = ($fields['order'] -eq 'pinfirst')
        Refs           = $refs
        Ctx            = $ctx
        OrderB         = $orderB
        VramDeltaMb    = $vram
    }
}

function Row-Text($row) {
    $text = 'class={0} file={1} size={2} stamp={3:X} config={4} verdict={5} witness={6} order={7} refs={8}' -f $row.Class,
        $row.File, $row.Size, $row.Stamp, $row.Config, $row.Verdict, $(if ($row.Witness) { 'honoured' } else { 'no' }),
        $(if ($row.PinFirst) { 'pinfirst' } else { 'destroyfirst' }), $row.Refs
    if ($row.Class -eq 'xefg') {
        $text += ' companionFile={0} companionSize={1} companionStamp={2:X}' -f $row.CompanionFile, $row.CompanionSize, $row.CompanionStamp
    }
    $text += ' ctx={0} orderB={1} vramDeltaMb={2}' -f $row.Ctx, $row.OrderB, $row.VramDeltaMb
    return $text
}

function Read-Data([string]$path) {
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($line in (Get-Content $path -Encoding UTF8)) {
        $clean = Normalise-RowText (($line -replace '#.*$', '').Trim())
        if (-not $clean) { continue }
        $rows.Add((Parse-Row $clean))
    }
    return , $rows
}

$rows = Read-Data $Data

if ($Source) {
    if (-not (Test-Path $Source)) { throw "probe output not found: $Source" }
    foreach ($line in (Get-Content $Source -Encoding UTF8)) {
        if ($line -notmatch 'fg-probe\s+model\s+(class=\S+.*)$') { continue }
        # The $Matches from the line match is overwritten by the token matches below, so keep the row text aside.
        $modelText = Normalise-RowText $Matches[1]
        # The host's row line carries the fields the data file takes, in the same order, plus pageheap/runs/evidence.
        # Rebuild it from the tokens: a field that comes later in the host line (refs, the companion, ctx, orderB,
        # vramDeltaMb) must survive the optional trailing text.
        $tokens = @{}
        foreach ($token in ($modelText -split '\s+')) {
            if ($token -match '^([a-zA-Z]+)=(.+)$') { $tokens[$Matches[1]] = $Matches[2] }
        }
        if ($tokens.ContainsKey('verdict') -and $tokens['verdict'] -eq 'unknown') {
            Write-Host "[fg-probe] skipped (verdict=unknown, no row): $modelText"
            continue
        }
        $text = 'class={0} file={1} size={2} stamp={3} config={4} verdict={5} witness={6} order={7} refs={8}' -f
            $tokens['class'], $tokens['file'], $tokens['size'], $tokens['stamp'], $tokens['config'], $tokens['verdict'],
            $tokens['witness'], $tokens['order'], $tokens['refs']
        if ($tokens['class'] -eq 'xefg') {
            $text += ' companionFile={0} companionSize={1} companionStamp={2}' -f $tokens['companionFile'],
                $tokens['companionSize'], $tokens['companionStamp']
        }
        foreach ($optional in 'ctx', 'orderB', 'vramDeltaMb') {
            if ($tokens.ContainsKey($optional)) { $text += " $optional=$($tokens[$optional])" }
        }
        $row = Parse-Row $text
        # The companion build is part of the row's identity (another libxell.dll is another pair), so a row that differs
        # only in its companion is a second row, not a replacement of the first.
        $existing = $rows | Where-Object { $_.Class -eq $row.Class -and $_.File -eq $row.File -and $_.Size -eq $row.Size -and
            $_.Stamp -eq $row.Stamp -and $_.CompanionFile -eq $row.CompanionFile -and
            $_.CompanionSize -eq $row.CompanionSize -and $_.CompanionStamp -eq $row.CompanionStamp }
        if ($existing) {
            $existing.Verdict = $row.Verdict
            $existing.Witness = $row.Witness
            $existing.PinFirst = $row.PinFirst
            $existing.Refs = $row.Refs
            $existing.CompanionFile = $row.CompanionFile
            $existing.CompanionSize = $row.CompanionSize
            $existing.CompanionStamp = $row.CompanionStamp
            $existing.Ctx = $row.Ctx
            $existing.OrderB = $row.OrderB
            $existing.VramDeltaMb = $row.VramDeltaMb
            Write-Host "[fg-probe] replaced $($row.Class) $($row.File)"
        } else {
            $rows.Add($row)
            Write-Host "[fg-probe] added $($row.Class) $($row.File)"
        }
    }
}

# The data file keeps one line per row, sorted the way a person reads it: fsrfg first, then xefg, then file.
$sorted = $rows | Sort-Object -Property @{ Expression = { @('fsrfg', 'xefg').IndexOf($_.Class) } }, File
$dataText = @(
    '# The measured release-model rows: one line per class and library build.'
    '#'
    '# Written by a person from the output of the probe host (tools/fg-probe/build.ps1, then fg-probe.exe):'
    '#   class=<fsrfg|xefg> file=<library file name> size=<bytes> stamp=<PE TimeDateStamp, hex> config=<direct|xell>'
    '#   verdict=<M1|M2> witness=<honoured|no> order=<destroyfirst|pinfirst> refs=<standstill reference count>'
    '#   [companionFile=<file> companionSize=<bytes> companionStamp=<hex>, required for xefg]'
    '#   [ctx=<destroyed|refused|unknown> orderB=<clean|leaked|unknown> vramDeltaMb=<signed>]'
    '# config=direct for FSR FG (the library called directly) and config=xell for XeFG (with the XeLL context the product'
    '# always attaches); a row in any other configuration is refused, because it measures a setup the product never runs.'
    '# order= is the teardown order the row was measured with, and the product uses a row only in that order.'
    '# refs= is the count the chain carried at standstill; the preflight compares the live chain with it. An XeFG row'
    '# carries the build of the companion DLL the measurement needed (libxell.dll): another companion is another pair.'
    '# Then run Update-ModelTable.ps1, which regenerates OptiScaler/ext/fg/outswitch/ModelRows.h from this file.'
    '#'
    '# A row is admitted only from three probe runs that agreed, with a witness the library accepted; a build that is not'
    '# listed here is refused at run time.'
)
foreach ($row in $sorted) { $dataText += (Row-Text $row) }
$dataOut = ($dataText -join "`n") + "`n"

$headerText = @(
    '#pragma once'
    '// The measured release-model rows, compiled in. Generated by tools/fg-probe/Update-ModelTable.ps1 from'
    '// tools/fg-probe/model-rows.txt, which a person fills from the probe host''s output (three consistent runs, and the'
    '// library identity of the build that was measured). Do not edit by hand: run the script, and let the test'
    '// fg_switch_model compare this file''s rows with the data file.'
    '//'
    '// An empty data file generates the placeholder row below. An empty file name marks it, and LibraryModel.h skips it:'
    '// no provider class may switch until a probe run has been written down here.'
    ''
    '#include "ext/fg/outswitch/SwitchTypes.h"'
    ''
    '#include <cstddef>'
    '#include <cstdint>'
    ''
    'namespace ext::fg::outswitch::rows'
    '{'
    'struct Row'
    '{'
    '    Class cls;'
    '    const char* file;'
    '    uint64_t size;'
    '    uint32_t stamp;'
    '    const char* companionFile;'
    '    uint64_t companionSize;'
    '    uint32_t companionStamp;'
    '    const char* config;'
    '    Model model;'
    '    bool witnessHonoured;'
    '    bool pinFirst;'
    '    uint32_t standstillRefs;'
    '    const char* ctx;'
    '    const char* orderB;'
    '    int32_t vramDeltaMb;'
    '};'
    ''
    'inline constexpr Row kRows[] = {'
)
if ($sorted.Count -eq 0) {
    $headerText += '    // No measured rows: a placeholder that LibraryModel.h skips.'
    $headerText += '    { Class::Plain, "", 0, 0, "", 0, 0, "", Model::Unknown, false, false, 0, "unknown", "unknown", 0 },'
} else {
    foreach ($row in $sorted) {
        $cls = if ($row.Class -eq 'fsrfg') { 'Class::FsrFg' } else { 'Class::XeFg' }
        $headerText += ('    {{ {0}, "{1}", {2}ULL, 0x{3:X}u, "{4}", {5}ULL, 0x{6:X}u, "{7}", Model::{8}, {9}, {10}, {11}, "{12}", "{13}", {14} }},' -f
            $cls, $row.File, $row.Size, $row.Stamp, $row.CompanionFile, $row.CompanionSize, $row.CompanionStamp, $row.Config,
            $row.Verdict, $(if ($row.Witness) { 'true' } else { 'false' }), $(if ($row.PinFirst) { 'true' } else { 'false' }),
            $row.Refs, $row.Ctx, $row.OrderB, $row.VramDeltaMb)
    }
}
$headerText += '};'
$headerText += 'inline constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);'
$headerText += '} // namespace ext::fg::outswitch::rows'
$headerOut = ($headerText -join "`n") + "`n"

if ($Check) {
    $currentHeader = if (Test-Path $Header) { (Get-Content $Header -Raw -Encoding UTF8) -replace "`r`n", "`n" } else { '' }
    $currentData = if (Test-Path $Data) { (Get-Content $Data -Raw -Encoding UTF8) -replace "`r`n", "`n" } else { '' }
    if ($currentHeader -ne $headerOut) { Write-Host "[fg-probe] ModelRows.h is out of date: run Update-ModelTable.ps1"; exit 1 }
    if ($currentData -ne $dataOut) { Write-Host "[fg-probe] model-rows.txt is not normalised: run Update-ModelTable.ps1"; exit 1 }
    Write-Host "[fg-probe] rows and header agree ($($sorted.Count) row(s))"
    exit 0
}

[IO.File]::WriteAllText($Data, $dataOut, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($Header, $headerOut, [Text.UTF8Encoding]::new($false))
Write-Host "[fg-probe] wrote $($sorted.Count) row(s) to $Header"
exit 0
