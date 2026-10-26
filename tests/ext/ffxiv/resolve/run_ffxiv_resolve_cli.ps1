# Builds tools\ffxiv-resolve\main.cpp (the patch-day CLI) with the test toolchain and runs it. tools\test.ps1 starts this under the vcvars64
# environment as:  pwsh -File run_ffxiv_resolve_cli.ps1 -Repo <repo root> -Out <build folder>   (exit code 0 = pass)
#
#   always:         the CLI builds without warnings, refuses a missing argument, a missing file and a file that is not an x64 PE
#   with the exe:   analysis\patch-20261016\ffxiv_dx11.exe (never committed): --strict --compare-table --margin must exit 0, find every
#                   seam, print the entry and agree with the table; without the exe the line "SKIP: exe not found: ..." is printed
#                   (tools\test.ps1 has no skip state: a writer's gate log must not contain it)
param(
    [Parameter(Mandatory = $true)][string]$Repo,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'
$failed = $false
function Fail([string]$why) { Write-Host "FAIL: $why"; $script:failed = $true }

$src = Join-Path $Repo 'tools\ffxiv-resolve\main.cpp'
$exe = Join-Path $Out 'ffxiv-resolve.exe'
$build = & cl.exe /nologo /std:c++20 /EHsc /W4 /DNOMINMAX "/Fo$Out\" "/Fe$exe" "/I$Repo\OptiScaler" "/I$Repo\tools\ffxiv-resolve" $src 2>&1
if ($LASTEXITCODE -ne 0) {
    $build | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
    Fail 'the CLI does not build'
    exit 1
}
$warnings = @($build | Where-Object { $_ -match 'warning' })
if ($warnings.Count -ne 0) { $warnings | ForEach-Object { Write-Host "    $_" }; Fail "the CLI build has $($warnings.Count) warning(s)" }

# error paths
& $exe *> $null
if ($LASTEXITCODE -ne 1) { Fail "no argument: expected exit 1, got $LASTEXITCODE" }
& $exe (Join-Path $Out 'does-not-exist.exe') *> $null
if ($LASTEXITCODE -ne 1) { Fail "missing file: expected exit 1, got $LASTEXITCODE" }
$notPe = Join-Path $Out 'not-a-pe.exe'
Set-Content -Path $notPe -Value ('this is not a portable executable ' * 40)
& $exe $notPe *> $null
if ($LASTEXITCODE -ne 1) { Fail "not a PE: expected exit 1, got $LASTEXITCODE" }
& $exe $notPe --bogus *> $null
if ($LASTEXITCODE -ne 1) { Fail "unknown option: expected exit 1, got $LASTEXITCODE" }

# --emit-gamebuild, always: argument errors, and the source header is never overwritten (a COPY of the committed header stands in for it)
function Invoke-Capture([string]$Program, [string[]]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Program
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $ms = New-Object System.IO.MemoryStream
    $p.StandardOutput.BaseStream.CopyTo($ms)
    $p.WaitForExit()
    [pscustomobject]@{ Code = $p.ExitCode; Out = $ms.ToArray(); Err = $errTask.Result }
}
function Same-Bytes([byte[]]$A, [byte[]]$B) { ($A.Length -eq $B.Length) -and ([System.Linq.Enumerable]::SequenceEqual($A, $B)) }
$headerSrc = Join-Path $Repo 'OptiScaler\ext\ffxiv\GameBuild.h'
$hdr = Join-Path $Out 'GameBuild.copy.h'
Copy-Item $headerSrc $hdr -Force
$hdrBytes = [IO.File]::ReadAllBytes($hdr)
$hdrHash = (Get-FileHash $hdr -Algorithm SHA256).Hash
$hdrTime = (Get-Item $hdr).LastWriteTimeUtc.Ticks
& $exe $notPe --emit-gamebuild *> $null
if ($LASTEXITCODE -ne 1) { Fail "--emit-gamebuild without a path: expected exit 1, got $LASTEXITCODE" }
& $exe $notPe --out (Join-Path $Out 'x.h') *> $null
if ($LASTEXITCODE -ne 1) { Fail "--out without --emit-gamebuild: expected exit 1, got $LASTEXITCODE" }
& $exe $notPe --allow-partial *> $null
if ($LASTEXITCODE -ne 1) { Fail "--allow-partial without --emit-gamebuild: expected exit 1, got $LASTEXITCODE" }
& $exe $notPe --emit-gamebuild (Join-Path $Out 'no-such-header.h') *> $null
if ($LASTEXITCODE -ne 1) { Fail "--emit-gamebuild with a missing header: expected exit 1, got $LASTEXITCODE" }
$r = Invoke-Capture $exe @($notPe, '--emit-gamebuild', $hdr, '--out', $hdr)
if ($r.Code -ne 1 -or -not $r.Err.Contains('never overwritten')) { Fail "--out naming the source header: expected exit 1 and the refusal text, got $($r.Code)" }
$sameFileViaDots = Join-Path $Out ('.\' + 'GameBuild.copy.h')
$r = Invoke-Capture $exe @($notPe, '--emit-gamebuild', $hdr, '--out', $sameFileViaDots)
if ($r.Code -ne 1) { Fail "--out naming the source header through another spelling of its path: expected exit 1, got $($r.Code)" }
$r = Invoke-Capture $exe @($notPe, '--emit-gamebuild', $hdr)   # a header that is fine but an exe that is not a PE: nothing on stdout
if ($r.Code -ne 1 -or $r.Out.Length -ne 0) { Fail "--emit-gamebuild over a file that is not a PE: expected exit 1 and an empty stdout, got $($r.Code) / $($r.Out.Length) bytes" }
# --out naming the INPUT executable (a typo in the runbook command) is refused up front, before anything is read: the write at the end would truncate it.
# A copy of the CLI itself stands in for an executable (a valid x64 PE the CLI can load: with --allow-partial the emit would reach the write); the text
# file stands in for one that cannot be loaded (the refusal must still be the --out one, not the load error).
$inputCopy = Join-Path $Out 'input-copy.exe'
Copy-Item $exe $inputCopy -Force
$inputHash = (Get-FileHash $inputCopy -Algorithm SHA256).Hash
$notPeHash = (Get-FileHash $notPe -Algorithm SHA256).Hash
foreach ($input in @($inputCopy, $notPe)) {
    $name = Split-Path $input -Leaf
    $spellings = @($input, (Join-Path $Out ('.\' + $name)))
    foreach ($spelling in $spellings) {
        foreach ($extra in @(@(), @('--allow-partial'))) {
            $r = Invoke-Capture $exe (@($input, '--emit-gamebuild', $hdr, '--out', $spelling) + $extra)
            if ($r.Code -ne 1 -or $r.Out.Length -ne 0 -or -not $r.Err.Contains('--out is the input executable')) { Fail "--out naming the input executable ($spelling $($extra -join ' ')): expected exit 1, an empty stdout and the refusal text, got $($r.Code): $($r.Err)" }
        }
    }
}
if ((Get-FileHash $inputCopy -Algorithm SHA256).Hash -ne $inputHash) { Fail 'the input executable was truncated or changed by --out naming it' }
if ((Get-FileHash $notPe -Algorithm SHA256).Hash -ne $notPeHash) { Fail 'the non-PE input file was changed by --out naming it' }
# a different --out next to the same input is not refused for that reason (it fails on the load error or goes on; never the --out refusal)
$r = Invoke-Capture $exe @($notPe, '--emit-gamebuild', $hdr, '--out', (Join-Path $Out 'elsewhere.h'))
if ($r.Err.Contains('--out is the input executable')) { Fail 'a --out that names another file was refused as the input' }
if (Test-Path (Join-Path $Out 'elsewhere.h')) { Fail 'an emit over a file that is not a PE wrote a file' }
if ((Get-FileHash $hdr -Algorithm SHA256).Hash -ne $hdrHash -or (Get-Item $hdr).LastWriteTimeUtc.Ticks -ne $hdrTime) { Fail 'the source header was touched by an error path' }

$gameExe = Join-Path $Repo '..\analysis\patch-20261016\ffxiv_dx11.exe'
if (-not (Test-Path $gameExe)) {
    Write-Host "SKIP: exe not found: $gameExe"
    if ($failed) { exit 1 }
    Write-Host 'PASS: ffxiv_resolve_cli (build and error paths; the executable run was skipped)'
    exit 0
}

$text = (& $exe $gameExe --strict --compare-table --margin 2>&1 | Out-String)
$code = $LASTEXITCODE
$text -split "`r?`n" | ForEach-Object { if ($_ -ne '') { Write-Host "    $_" } }
if ($code -ne 0) { Fail "--strict --compare-table --margin exited $code" }
foreach ($needle in @(
        '[ffxiv-resolve] chara.accessor Found rva=0x26abd0 hits=1 table=0x26abd0 agree',
        '[ffxiv-resolve] chara.managerPointer Found rva=0x290e490 hits=1 table=0x290e490 agree',
        'charaView.accessorRva=0x26abd0',
        'charaView.managerPointerRva=0x290e490',
        'timeDateStamp=0x6ad10bfd',
        'COMPARE: the generated entry equals the table',
        '0 seams disagree',
        'knownBuild=yes')) {
    if (-not $text.Contains($needle)) { Fail "output lacks: $needle" }
}
if ($text.Contains('LOW ')) { Fail 'an anchor is below its required margin' }
# EVERY seam line (not only the two CharaView ones) must be a Found that agrees with the table or has no table binding; nothing DISAGREEs
$seamLines = @($text -split "`r?`n" | Where-Object { $_ -match '^\s*(WARN\s+|ERROR\s+)?\[ffxiv-resolve\] [A-Za-z0-9_]+\.[A-Za-z0-9_]+ [A-Za-z]+ rva=' })
if ($seamLines.Count -eq 0) { Fail 'no seam line in the output' }
foreach ($line in $seamLines) {
    if ($line -notmatch ' Found rva=') { Fail "a seam is not plain Found: $line" }
    if ($line -cmatch 'DISAGREE') { Fail "a seam DISAGREES with the table: $line" }
}
if ($text -cmatch 'DISAGREE') { Fail 'the output mentions a DISAGREE' }

# Negative control, end to end: a copy of the executable whose accessor loads a DIFFERENT bss global (the 4 masked displacement bytes of the
# CharaView accessor moved by +8). The stamp and size are unchanged, so the table still counts, the pattern still matches uniquely and the
# manager pointer resolves to 0x290e498 while the table says 0x290e490: a real DISAGREE. --strict must exit 2 and --compare-table 3.
$patched = Join-Path $Out 'ffxiv_dx11_patched.exe'
Copy-Item $gameExe $patched -Force
$fs = [IO.File]::Open($patched, 'Open', 'ReadWrite')
try {
    $fileOffset = 0x269fd0   # RVA 0x26abd0 in .text (VA 0x1000, raw 0x400)
    $buf = New-Object byte[] 18
    $null = $fs.Seek($fileOffset, 'Begin'); $null = $fs.Read($buf, 0, 18)
    $hex = ($buf | ForEach-Object { $_.ToString('x2') }) -join ''
    if (-not ($hex.StartsWith('488b05') -and $hex.Substring(14) -eq '8bca488b84c8c8020000c3')) { Fail "the accessor bytes are not where the negative control expects them: $hex" }
    else {
        $disp = [BitConverter]::ToInt32($buf, 3) + 8
        $null = $fs.Seek($fileOffset + 3, 'Begin'); $fs.Write([BitConverter]::GetBytes($disp), 0, 4)
    }
}
finally { $fs.Dispose() }
$negStrict = (& $exe $patched --strict 2>&1 | Out-String); $codeStrict = $LASTEXITCODE
if ($codeStrict -ne 2) { Fail "negative control: --strict on a disagreeing executable exited $codeStrict, expected 2" }
if (-not $negStrict.Contains('DISAGREE') -or -not $negStrict.Contains('STRICT: seam chara.managerPointer DISAGREES')) { Fail 'negative control: --strict did not name the disagreeing seam' }
$negCompare = (& $exe $patched --compare-table 2>&1 | Out-String); $codeCompare = $LASTEXITCODE
if ($codeCompare -ne 3) { Fail "negative control: --compare-table on a disagreeing executable exited $codeCompare, expected 3" }
if (-not $negCompare.Contains('COMPARE: seam chara.managerPointer DISAGREES')) { Fail 'negative control: --compare-table did not name the disagreeing seam' }
if (-not $negCompare.Contains('DIFFERS from')) { Fail 'negative control: the entry resolved without the table should differ from the table' }

# --emit-gamebuild on the real executable ----------------------------------------------------------------------------------------------------
# argument errors that need a real executable to prove they are caught BEFORE any work (a missing --emit-gamebuild must not run the report)
foreach ($bad in @(@('--out', (Join-Path $Out 'x.h')), @('--allow-partial'))) {
    $r = Invoke-Capture $exe (@($gameExe) + $bad)
    if ($r.Code -ne 1 -or [Text.Encoding]::ASCII.GetString($r.Out).Contains('== image ==')) { Fail "'$($bad -join ' ')' without --emit-gamebuild on the real exe: expected exit 1 (usage) and nothing run, got $($r.Code)" }
}
# (1) the current exe and the current header: stdout and --out are byte-identical to the header, the source is untouched, the report is on stderr
$strictEmit = Invoke-Capture $exe @($gameExe, '--emit-gamebuild', $hdr, '--strict', '--compare-table')
if ($strictEmit.Code -ne 0) { Fail "--emit-gamebuild --strict --compare-table on the current exe exited $($strictEmit.Code)" }
if (-not (Same-Bytes $strictEmit.Out $hdrBytes)) { Fail "--emit-gamebuild on the current exe: stdout ($($strictEmit.Out.Length) bytes) is not the header ($($hdrBytes.Length) bytes) byte for byte" }
foreach ($needle in @('result: EMITTED (every bound field re-verified)', '== NOT SELF-HEALING / NOT RE-VERIFIED ==', '== SHADER CATALOGS: NOT VERIFIABLE OFFLINE ==', "UI 6/7 and 'UI boundary ok'", '-- seam status table', 'emit: wrote')) {
    if (-not $strictEmit.Err.Contains($needle)) { Fail "--emit-gamebuild report lacks: $needle" }
}
if ($strictEmit.Err -notmatch 'bound fields: \d+ \(rewritten from the resolver: 0 changed entries') { Fail '--emit-gamebuild on the current exe reports changed entries' }
$outFile = Join-Path $Out 'GameBuild.new.h'
$viaFile = Invoke-Capture $exe @($gameExe, '--emit-gamebuild', $hdr, '--out', $outFile)
if ($viaFile.Code -ne 0 -or $viaFile.Out.Length -ne 0) { Fail "--emit-gamebuild --out: expected exit 0 and an empty stdout, got $($viaFile.Code) / $($viaFile.Out.Length) bytes" }
if (-not (Test-Path $outFile) -or -not (Same-Bytes ([IO.File]::ReadAllBytes($outFile)) $hdrBytes)) { Fail '--emit-gamebuild --out: the written file is not the header byte for byte' }
$r = Invoke-Capture $exe @($gameExe, '--emit-gamebuild', $hdr, '--out', $hdr)
if ($r.Code -ne 1 -or $r.Out.Length -ne 0) { Fail "--emit-gamebuild --out <the source> on the real exe: expected exit 1, got $($r.Code)" }
if ((Get-FileHash $hdr -Algorithm SHA256).Hash -ne $hdrHash -or (Get-Item $hdr).LastWriteTimeUtc.Ticks -ne $hdrTime) { Fail 'the source header was touched by a successful emit' }

# (2) a header that does not fit the field table (a bound member renamed): exit 1, nothing written
$badHeader = Join-Path $Out 'GameBuild.bad.h'
[IO.File]::WriteAllText($badHeader, ([IO.File]::ReadAllText($hdr)).Replace('querySizeRva', 'querySizeRvx'))
$badOut = Join-Path $Out 'GameBuild.bad.out.h'
$r = Invoke-Capture $exe @($gameExe, '--emit-gamebuild', $badHeader, '--out', $badOut)
if ($r.Code -ne 1 -or (Test-Path $badOut) -or -not $r.Err.Contains('querySizeRva')) { Fail "a header that does not fit: expected exit 1, no file and the field named, got $($r.Code)" }

# (3) the patched copy (accessor displacement +8: a different manager global): the emitted header differs from the source in exactly one line,
# the CharaView manager pointer, although the same value 0x290e498 already stands in the native quality group (substitution goes by name and struct)
$patchedOut = Join-Path $Out 'GameBuild.patched.h'
$r = Invoke-Capture $exe @($patched, '--emit-gamebuild', $hdr, '--out', $patchedOut)
if ($r.Code -ne 0) { Fail "--emit-gamebuild on the patched copy exited $($r.Code), expected 0 (every seam still resolves)" }
else {
    $a = [IO.File]::ReadAllLines($hdr); $b = [IO.File]::ReadAllLines($patchedOut)
    $diff = @(); if ($a.Length -ne $b.Length) { Fail 'the emitted header has another line count' } else { for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $diff += $i } } }
    if ($diff.Count -ne 1) { Fail "patched copy: expected exactly one changed line, got $($diff.Count)" }
    elseif (-not ($a[$diff[0]].Contains('managerPointerRva = 0x290e490;') -and $b[$diff[0]].Contains('managerPointerRva = 0x290e498;') -and $a[$diff[0]].Replace('0x290e490', '0x290e498') -eq $b[$diff[0]])) { Fail "patched copy: the changed line is not the CharaView manager pointer: $($b[$diff[0]])" }
    if (([regex]::Matches(([IO.File]::ReadAllText($patchedOut)), '0x290e498;')).Count -ne 2) { Fail 'patched copy: expected the value 0x290e498 on exactly two lines (native quality and CharaView)' }
}

# (4) the same copy with the accessor pattern broken (byte +7, the `mov ecx,edx` of the 18-byte pattern, overwritten): the CharaView seams are
# unresolved. Without --allow-partial the codegen refuses (exit 2, nothing written); with it the old text is kept and marked (exit 4).
$fs = [IO.File]::Open($patched, 'Open', 'ReadWrite')
try {
    $fileOffset = 0x269fd0
    $buf = New-Object byte[] 18
    $null = $fs.Seek($fileOffset, 'Begin'); $null = $fs.Read($buf, 0, 18)
    $disp = [BitConverter]::ToInt32($buf, 3) - 8   # undo the +8 of the negative control: the copy is the pristine exe again, then break one fixed byte
    $null = $fs.Seek($fileOffset + 3, 'Begin'); $fs.Write([BitConverter]::GetBytes($disp), 0, 4)
    $null = $fs.Seek($fileOffset + 7, 'Begin'); $fs.WriteByte(0x90)
}
finally { $fs.Dispose() }
$refuseOut = Join-Path $Out 'GameBuild.refused.h'
$r = Invoke-Capture $exe @($patched, '--emit-gamebuild', $hdr, '--out', $refuseOut)
if ($r.Code -ne 2) { Fail "--emit-gamebuild with an unresolved bound seam: expected exit 2, got $($r.Code)" }
if (Test-Path $refuseOut) { Fail 'the refused codegen wrote a file' }
if ($r.Out.Length -ne 0) { Fail 'the refused codegen wrote to stdout' }
foreach ($needle in @('REFUSED', 'UNRESOLVED bound fields', 'charaView.accessorRva  <- seam chara.accessor: NotFound', 'emit: nothing written')) { if (-not $r.Err.Contains($needle)) { Fail "the refusal report lacks: $needle" } }
$r = Invoke-Capture $exe @($patched, '--emit-gamebuild', $hdr)
if ($r.Code -ne 2 -or $r.Out.Length -ne 0) { Fail "the refusal on stdout: expected exit 2 and an empty stdout, got $($r.Code) / $($r.Out.Length) bytes" }
$partialOut = Join-Path $Out 'GameBuild.partial.h'
$r = Invoke-Capture $exe @($patched, '--emit-gamebuild', $hdr, '--out', $partialOut, '--allow-partial')
if ($r.Code -ne 4) { Fail "--allow-partial with an unresolved seam: expected exit 4, got $($r.Code)" }
elseif (-not (Test-Path $partialOut)) { Fail '--allow-partial wrote no file' }
else {
    $a = [IO.File]::ReadAllLines($hdr); $b = [IO.File]::ReadAllLines($partialOut)
    $marked = @(); if ($a.Length -ne $b.Length) { Fail 'the partial header has another line count' } else { for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $marked += $i } } }
    if ($marked.Count -lt 2) { Fail "partial: expected at least the two CharaView lines marked, got $($marked.Count)" }
    foreach ($i in $marked) { if ($b[$i] -ne ($a[$i] + ' // NOT re-verified for this build') -or $b[$i] -notmatch 'accessorRva|managerPointerRva|targetsOffset') { Fail "partial: a changed line is not an old line plus the mark: $($b[$i])" } }
    if (-not ($r.Err.Contains('EMITTED PARTIAL') -and $r.Err.Contains('== NOT SELF-HEALING / NOT RE-VERIFIED =='))) { Fail 'the partial report lacks its verdict or the not-re-verified section' }
}
# the worst code wins: a strict failure (2) beats the partial emit (4); a table disagreement (3) beats it too
$r = Invoke-Capture $exe @($patched, '--emit-gamebuild', $hdr, '--out', (Join-Path $Out 'GameBuild.partial2.h'), '--allow-partial', '--strict')
if ($r.Code -ne 2) { Fail "--strict with a partial emit: expected exit 2 (strict beats partial), got $($r.Code)" }
$r = Invoke-Capture $exe @($patched, '--emit-gamebuild', $hdr, '--out', (Join-Path $Out 'GameBuild.partial3.h'), '--allow-partial', '--compare-table')
if ($r.Code -ne 3) { Fail "--compare-table with a partial emit: expected exit 3 (disagreement beats partial), got $($r.Code)" }
$r = Invoke-Capture $exe @($patched, '--emit-gamebuild', $hdr, '--strict')
if ($r.Code -ne 2 -or $r.Out.Length -ne 0) { Fail "--strict with a refused emit: expected exit 2 and nothing on stdout, got $($r.Code)" }
if ((Get-FileHash $hdr -Algorithm SHA256).Hash -ne $hdrHash -or (Get-Item $hdr).LastWriteTimeUtc.Ticks -ne $hdrTime) { Fail 'the source header was touched' }
Remove-Item $patched -Force -ErrorAction SilentlyContinue
if ($failed) { exit 1 }
Write-Host 'PASS: ffxiv_resolve_cli'
exit 0
