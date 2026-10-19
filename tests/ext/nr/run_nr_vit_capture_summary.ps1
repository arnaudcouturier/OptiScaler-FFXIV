# Runner (tools\test.ps1 `Runner`): the capture format written by DlssNrVitCapture.h against the parser and the checks of
# tools\nr-vit-capture-summary.ps1. nr_vit_capture_synth.cpp writes synthetic captures with the real formatter (a clean one, and
# ones with a leaked sync counter, a foreign kernel inside the run, no outside launches, a rotating End output and a changed
# order); the script is run on each and must say what is wrong with them. Also exercises -Log and -Baseline.
# The per-feature part (sections at the end): Passes = 2 captures (pass 0 / pass 1 / pass 0), and the fixed go/no-go rule of the NR
# reuse study applied one condition at a time ("go" meets every condition, each "go_*" capture breaks exactly one of them).
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$script = Join-Path $Repo 'tools\nr-vit-capture-summary.ps1'
if (-not (Test-Path $script)) { throw "Missing $script" }

Invoke-Cl 'Capture synth' @('/nologo', '/std:c++20', '/EHsc', '/W4', "/I$Repo/OptiScaler", "/Fo$Out/synth.obj", "/Fe$Out/synth.exe", "$PSScriptRoot/nr_vit_capture_synth.cpp")
& "$Out/synth.exe" "$Out/caps"
if ($LASTEXITCODE -ne 0) { throw 'Synthetic capture writer failed' }

function Run-Summary([string[]]$Arguments) {
    $text = & pwsh -NoProfile -File $script @Arguments 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $text }
}
function Assert-Match($r, [string]$pattern, [string]$what) {
    if ($r.Text -notmatch $pattern) { Write-Output $r.Text; throw "Summarizer: $what (no match for /$pattern/)" }
}
function Assert-NoMatch($r, [string]$pattern, [string]$what) {
    if ($r.Text -match $pattern) { Write-Output $r.Text; throw "Summarizer: $what (unexpected /$pattern/)" }
}

# ---- clean capture ----
$good = Run-Summary @('-Capture', "$Out/caps/good/capture.txt", '-Strict')
if ($good.Code -ne 0) { Write-Output $good.Text; throw "clean capture exited $($good.Code)" }
Assert-Match $good '3 evaluation\(s\), run 20261016-135152' 'evaluations/run not read'
Assert-Match $good 'module 0x7000\s+1417240 bytes\s+sha256 2{64}\s+magic 7f454c46' 'MODULE record not read'
Assert-Match $good 'same cc_vit_\* order in every captured evaluation: names yes, names \+ psize yes' 'signature comparison'
Assert-Match $good 'sequence : repack_2d_to_1d ; ffn_expand_publish ; \[ffn_contract_chained ; qkv_chained ; attention_chained ; projection_chained ; ffn_expand_chained\] x2' 'sequence not printed or not compressed'
Assert-Match $good 'CHECK PASS\s+run-1 ' 'run check'
Assert-Match $good 'CHECK PASS\s+contig-3 ' 'contiguity check'
Assert-Match $good 'CHECK PASS\s+inv1-leak ' 'invariant 1 check'
Assert-Match $good 'CHECK PASS\s+end-stable ' 'End output stability check'
Assert-Match $good 'CHECK PASS\s+blob ' 'blob check'
Assert-Match $good 'CHECK INFO\s+ffn-slots .*all 3 pair\(s\) match' 'FFN slot hypothesis'
# The clean capture's decoder launch (cc_dec_out) legitimately carries End slot 1 after the run, so the end-users check says so
# (a WARN: whether it only reads the buffer is not in the files); nothing else may warn or fail.
Assert-NoMatch $good 'CHECK (FAIL|WARN)\s+(?!end-users\b)\S' 'clean capture produced warnings'
Assert-Match $good 'CHECK WARN\s+end-users .*cc_dec_out' 'the launch after End that carries End slot 1 was not reported'
# pointer table: sync counter private to the run, in/out buffers at the boundary, global only outside, y/x buffers inside
Assert-Match $good '0xc400300000\s+ptr\s+\d+\s+\d+\s+\d+\s+\d+\s+0\s+yes\s+run-private' 'sync counter not run-private'
Assert-Match $good '0xc400000000\s+ptr\s+\d+\s+\d+\s+0\s+1\s+\d+\s+yes\s+boundary' 'input buffer not at the boundary'
Assert-Match $good '0xc400500000\s+ptr\s+\d+\s+\d+\s+0\s+0\s+\d+\s+yes\s+outside the run only' 'global buffer not outside-only'
$signature = [regex]::Match($good.Text, 'SIGNATURE \(names\)\s+sha256: ([0-9a-f]{64})').Groups[1].Value
if ($signature.Length -ne 64) { throw 'signature hash missing' }
# The hash is of the names joined by newlines; recompute it independently.
$names = @('cc_vit_1d_repack_2d_to_1d_fp8')
foreach ($b in 0..2) {
    $names += $(if ($b -eq 0) { 'cc_vit_1d_ffn_expand_publish_fp8' } else { 'cc_vit_1d_ffn_expand_chained_fp8' })
    $names += 'cc_vit_1d_ffn_contract_chained_fp8', 'cc_vit_1d_qkv_chained_fp8', 'cc_vit_1d_attention_chained_fp8'
    $names += $(if ($b -eq 2) { 'cc_vit_1d_projection_wait_fp8' } else { 'cc_vit_1d_projection_chained_fp8' })
}
$names += 'cc_vit_1d_repack_1d_to_2d_fp8'
$expected = ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($names -join "`n")))).ToLowerInvariant()
if ($signature -ne $expected) { throw "signature $signature != $expected" }
Write-Output 'PASS: clean synthetic capture parsed, signature hash reproduced independently'

# ---- the same capture through -PassThru ----
$obj = & pwsh -NoProfile -Command "& '$script' -Capture '$Out/caps/good/capture.txt' -PassThru | Select-Object -Last 1 | ConvertTo-Json -Depth 6 -Compress"
$o = $obj | ConvertFrom-Json
if ($o.NamesHash -ne $signature -or $o.Evaluations -ne 3 -or -not $o.SameNames -or $o.Failed -ne 0) { throw "-PassThru object wrong: $obj" }
Write-Output 'PASS: -PassThru object agrees'

# ---- broken captures ----
$leak = Run-Summary @('-Capture', "$Out/caps/leak/capture.txt", '-Strict')
if ($leak.Code -ne 1) { Write-Output $leak.Text; throw "leaking capture exited $($leak.Code), expected 1" }
Assert-Match $leak '0xc400300000\s+ptr\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+yes\s+LEAK' 'leaked counter not classified'
Assert-Match $leak 'CHECK FAIL\s+inv1-leak ' 'leak not failed'
$foreign = Run-Summary @('-Capture', "$Out/caps/foreign/capture.txt", '-Strict')
if ($foreign.Code -ne 1) { throw "foreign capture exited $($foreign.Code), expected 1" }
Assert-Match $foreign 'CHECK FAIL\s+contig-1 .*cc_foreign' 'foreign kernel inside the run not reported'
$bare = Run-Summary @('-Capture', "$Out/caps/bare/capture.txt", '-Strict')
if ($bare.Code -ne 0) { throw "bare capture exited $($bare.Code), expected 0 (warning only)" }
Assert-Match $bare 'CHECK WARN\s+outside .*invariant 1' 'missing outside launches not warned'
Assert-Match $bare 'unknown \(no launches outside' 'pointer classes should be unknown without outside launches'
$rotate = Run-Summary @('-Capture', "$Out/caps/rotate/capture.txt")
Assert-Match $rotate 'CHECK WARN\s+end-stable ' 'rotating End output not warned'
$bad = Run-Summary @('-Capture', "$Out/caps/badorder/capture.txt", '-Strict')
if ($bad.Code -ne 1) { throw "changed order exited $($bad.Code), expected 1" }
Assert-Match $bad 'same cc_vit_\* order in every captured evaluation: names NO' 'order change not detected'
Assert-Match $bad 'CHECK FAIL\s+sig-names ' 'order change not failed'
Write-Output 'PASS: leaked counter, foreign kernel, missing outside launches, rotating output and changed order are all reported'

# ---- folders: a run folder and the nr-vit-capture folder pick the newest run ----
New-Item -ItemType Directory -Force "$Out/root/20261001-000000", "$Out/root/20261016-135152", "$Out/root/modules" | Out-Null
Copy-Item "$Out/caps/bare/capture.txt" "$Out/root/20261001-000000/capture.txt"
Copy-Item "$Out/caps/good/capture.txt" "$Out/root/20261016-135152/capture.txt"
$viaRoot = Run-Summary @('-Capture', "$Out/root")
Assert-Match $viaRoot '20261016-135152' 'the newest run was not chosen'
Assert-NoMatch $viaRoot 'CHECK WARN\s+outside' 'older run was read (it is the capture without outside launches)'
$viaRun = Run-Summary @('-Capture', "$Out/caps/good")
Assert-Match $viaRun 'CHECK PASS\s+run-1 ' 'run folder not accepted'
Write-Output 'PASS: capture file, run folder and nr-vit-capture folder inputs'

# ---- the plain-log input and -Baseline ----
$logLines = @(
    '[13:51:52.635880] [I] DlssNrNative::`anonymous-namespace''::CreateFunction NR reuse module: bytes=1417240, sha256=2a05b056783f74c602b7a7a53d48629547054cd9b23a4c75a92bc2ce77d6d9c3, verified=false, first kernel=cc_vit_1d_ffn_expand_fp8',
    '[13:51:52.908541] [I] DlssNrNative::`anonymous-namespace''::VitDrop NR reuse unverified launch: kernel=cc_vit_1d_repack_2d_to_1d_fp8, params=24, grid=2176x1x1, block=256x1x1, shared=0',
    '[13:51:52.908567] [I] DlssNrNative::`anonymous-namespace''::VitDrop NR reuse unverified launch: kernel=cc_vit_1d_ffn_expand_publish_fp8, params=72, grid=544x1x1, block=32x4x1, shared=0',
    '[13:51:52.908589] [I] DlssNrNative::`anonymous-namespace''::VitDrop NR reuse unverified launch: kernel=cc_vit_1d_ffn_contract_chained_fp8, params=72, grid=136x1x4, block=32x4x1, shared=0',
    '[13:51:52.909034] [I] DlssNrNative::`anonymous-namespace''::VitDrop NR reuse unverified launch: kernel=cc_vit_1d_repack_1d_to_2d_fp8, params=24, grid=2160x1x1, block=256x1x1, shared=0',
    '[13:51:52.909232] [I] DlssNrNative::EndEvaluate NR reuse: unverified NR kernel module; compatibility profile needed; evaluations=1',
    '[13:51:52.923432] [I] DlssNrNative::`anonymous-namespace''::VitDrop NR reuse unverified launch: kernel=cc_vit_1d_repack_2d_to_1d_fp8, params=24, grid=2176x1x1, block=256x1x1, shared=0',
    '[13:51:52.923450] [I] DlssNrNative::`anonymous-namespace''::VitDrop NR reuse unverified launch: kernel=cc_vit_1d_repack_1d_to_2d_fp8, params=24, grid=2160x1x1, block=256x1x1, shared=0'
)
$logPath = Join-Path $Out 'sample.log'
Set-Content -LiteralPath $logPath -Value $logLines
$fromLog = Run-Summary @('-Log', $logPath)
Assert-Match $fromLog '2 evaluation\(s\), taken from the plain log' 'log evaluations'
Assert-Match $fromLog 'module\s+1417240 bytes\s+sha256 2a05b056783f74c602b7a7a53d48629547054cd9b23a4c75a92bc2ce77d6d9c3' 'log module line'
Assert-Match $fromLog 'sequence : repack_2d_to_1d ; ffn_expand_publish ; ffn_contract_chained ; repack_1d_to_2d' 'log sequence'
Assert-Match $fromLog 'same cc_vit_\* order in every captured evaluation: names NO' 'the two logged evaluations differ here'
Assert-Match $fromLog 'not available from the plain log' 'pointer table must be unavailable from the log'
$cmp = Run-Summary @('-Capture', "$Out/caps/good/capture.txt", '-Baseline', $logPath)
Assert-Match $cmp 'CHECK FAIL\s+baseline .*first difference at position' 'baseline difference not reported'
$same = Run-Summary @('-Capture', "$Out/caps/good/capture.txt", '-Baseline', "$Out/caps/good/capture.txt")
Assert-Match $same 'CHECK PASS\s+baseline ' 'identical baseline not accepted'
# a log with the clean capture's own cc_vit_1d_* order is an identical baseline (this is how a capture is cross-checked against the log)
$matchLog = Join-Path $Out 'match.log'
Set-Content -LiteralPath $matchLog -Value @($names | ForEach-Object { "[13:51:52.9] [I] DlssNrNative::VitDrop NR reuse unverified launch: kernel=$_, params=24, grid=1x1x1, block=1x1x1, shared=0" })
$crossCheck = Run-Summary @('-Capture', "$Out/caps/good/capture.txt", '-Baseline', $matchLog)
Assert-Match $crossCheck 'CHECK PASS\s+baseline ' 'a log with the same cc_vit_1d_* order was not accepted as baseline'
Write-Output 'PASS: -Log reads the product log lines, -Baseline reports the first difference and accepts an identical log order'

# =====================================================================================================================
# Per-feature judgement and the fixed go/no-go rule (STUDY-nr-flicker.md, Step A). Each capture is read in-process with
# -PassThru; the PassThru object carries every check (Id, Status, Feature, Text) and the rule result per feature.
# =====================================================================================================================
function Get-Summary([string]$Name) {
    $all = @(& $script -Capture "$Out/caps/$Name/capture.txt" -PassThru)
    $obj = @($all | Where-Object { $_ -isnot [string] }) | Select-Object -Last 1
    if ($null -eq $obj) { throw "$Name : no -PassThru object" }
    return [pscustomobject]@{ Name = $Name; Text = (@($all | Where-Object { $_ -is [string] }) -join "`n"); Obj = $obj }
}
function Get-Check($s, [int]$feature, [string]$id) {
    $c = @($s.Obj.Checks | Where-Object { $_.Id -eq $id -and $_.Feature -eq $feature })
    if ($c.Count -ne 1) { Write-Output $s.Text; throw "$($s.Name): expected exactly one check '$id' for feature $feature, found $($c.Count)" }
    return $c[0]
}
function Assert-CheckStatus($s, [int]$feature, [string]$id, [string]$status, [string]$textPattern = '') {
    $c = Get-Check $s $feature $id
    if ($c.Status -ne $status) { Write-Output $s.Text; throw "$($s.Name): check '$id' (feature $feature) is $($c.Status), expected $status. Text: $($c.Text)" }
    if ($textPattern -and $c.Text -notmatch $textPattern) { throw "$($s.Name): check '$id' (feature $feature) text does not match /$textPattern/: $($c.Text)" }
}
function Get-Rule($s, [int]$feature) {
    $r = @($s.Obj.Rules | Where-Object { $_.Feature -eq $feature })
    if ($r.Count -ne 1) { Write-Output $s.Text; throw "$($s.Name): no rule result for feature $feature" }
    return $r[0]
}
function Assert-Verdict($s, [int]$feature, [string]$verdict) {
    $r = Get-Rule $s $feature
    if ($r.Verdict -ne $verdict) { Write-Output $s.Text; throw "$($s.Name): feature $feature verdict is $($r.Verdict), expected $verdict. Conditions: $(($r.Conditions | ForEach-Object { "$($_.Id)=$($_.Status)" }) -join ', ')" }
    if ($s.Text -notmatch "RULE $([regex]::Escape($verdict))\s+feature $feature ") { throw "$($s.Name): the printed RULE line for feature $feature is missing or differs from $verdict" }
}
function Assert-Cond($s, [int]$feature, [string]$id, [string]$status, [string]$textPattern = '') {
    $r = Get-Rule $s $feature
    $c = @($r.Conditions | Where-Object { $_.Id -eq $id })
    if ($c.Count -ne 1) { Write-Output $s.Text; throw "$($s.Name): feature $feature has no single rule row '$id' (rows: $(($r.Conditions | ForEach-Object { $_.Id }) -join ', '))" }
    if ($c[0].Status -ne $status) { Write-Output $s.Text; throw "$($s.Name): rule row '$id' (feature $feature) is $($c[0].Status), expected $status. $($c[0].Text)" }
    if ($textPattern -and $c[0].Text -notmatch $textPattern) { throw "$($s.Name): rule row '$id' text does not match /$textPattern/: $($c[0].Text)" }
}
function Assert-NoCond($s, [int]$feature, [string]$id) {
    $r = Get-Rule $s $feature
    if (@($r.Conditions | Where-Object { $_.Id -eq $id }).Count -ne 0) { throw "$($s.Name): rule row '$id' (feature $feature) should not exist here" }
}
# every rule row PASS except the listed ones (the one the capture was built to break)
function Assert-OnlyBroken($s, [int]$feature, [string[]]$broken) {
    $r = Get-Rule $s $feature
    $bad = @($r.Conditions | Where-Object { $_.Status -ne 'PASS' } | ForEach-Object { $_.Id })
    $extra = @($bad | Where-Object { $broken -notcontains $_ })
    $missing = @($broken | Where-Object { $bad -notcontains $_ })
    if ($extra.Count -gt 0 -or $missing.Count -gt 0) { Write-Output $s.Text; throw "$($s.Name): rule rows not PASS are [$($bad -join ', ')], expected exactly [$($broken -join ', ')]" }
}

# ---- Passes = 2: pass 0 / pass 1 / pass 0, pass 1 with its own buffers ----
$p2 = Get-Summary 'passes2'
Assert-Match $p2 'eval 2: feature 1, reset 0, every 1, override 0, watched only' 'evaluation 2 should be the watched pass 1'
Assert-CheckStatus $p2 0 'end-stable' 'PASS' 'evaluations 1, 3'
Assert-CheckStatus $p2 1 'end-stable' 'INFO' 'only 1 evaluation'
Assert-NoMatch $p2 'CHECK WARN\s+end-stable' 'a feature with other buffers must not warn another feature'
Assert-CheckStatus $p2 0 'start-end-link' 'INFO' 'in 2 of 2 evaluation'
Assert-CheckStatus $p2 0 'epoch' 'PASS'
Assert-CheckStatus $p2 1 'epoch' 'INFO' 'only 1 evaluation'
Assert-Match $p2 '-- feature 0: evaluation\(s\) 1, 3' 'per-feature section header'
Assert-Match $p2 '-- feature 1: evaluation\(s\) 2; VitEvery as applied 1, watched only' 'per-feature section header for pass 1'
# the End kernel slots of feature 0 are both stable although evaluation 2 (feature 1) differs
Assert-Match $p2 'pointer slots of the End kernel of feature 0[^\n]*\n(?:[^\n]*\n){3}\s*0\s+0\s+True\s+0xc400100000 0xc400100000\s*\n\s*1\s+8\s+True\s+0xc400400000 0xc400400000' 'End slots of feature 0 should be stable across its own evaluations'
Assert-Verdict $p2 0 'NO-GO'
Assert-Cond $p2 0 '1 end-stable' 'PASS' 'in all 2 evaluations'
Assert-Cond $p2 0 '3 clear-before' 'N/A'
Assert-OnlyBroken $p2 0 @('3 clear-before', '5b end-users-after')
Assert-Verdict $p2 1 'NOT-JUDGED'
Write-Output 'PASS: Passes = 2 capture is judged per feature (pass 1 with other buffers does not disturb pass 0)'

# ---- the capture that meets every condition ----
$go = Get-Summary 'go'
Assert-Verdict $go 0 'GO'
Assert-OnlyBroken $go 0 @()
foreach ($row in '1 end-stable', '2 leaks', '3 clear-before', '4 epoch', '5a end-users-before', '5b end-users-after') { Assert-Cond $go 0 $row 'PASS' }
Assert-Verdict $go 1 'NOT-JUDGED'
Assert-CheckStatus $go 0 'end-stable' 'PASS'
Assert-CheckStatus $go 0 'inv1-leak' 'WARN' 'cc_cb_clear only'
Assert-CheckStatus $go 0 'waiters-outside' 'PASS'
Assert-CheckStatus $go 0 'clear-before' 'PASS' 'every evaluation \(1, 3\)'
Assert-CheckStatus $go 0 'epoch' 'PASS'
Assert-CheckStatus $go 0 'end-users' 'PASS'
Assert-CheckStatus $go 0 'end-xfeature' 'PASS'
Assert-Match $go '0xc400700000\s+\[sentinel-clear\]\s+outside launches 2 in 1 kernel\(s\); used inside the run by ffn_expand_publish@p8 x1 \(launches per evaluation\)' 'the shared block should be listed as sentinel-clear with its Inner user'
Assert-Match $go '0xc400700000\s+ptr\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+yes\s+LEAK[^\n]*sentinel-clear' 'the LEAK table row should carry the sentinel-clear label'
Assert-Match $go 'RULE GO\s+feature 0 \(evaluation\(s\) 1, 3; VitEvery as applied 2\)' 'GO verdict line'
$goStrict = Run-Summary @('-Capture', "$Out/caps/go/capture.txt", '-Strict')
if ($goStrict.Code -ne 0) { Write-Output $goStrict.Text; throw "the go capture exited $($goStrict.Code) under -Strict" }
Write-Output 'PASS: a capture that meets every condition of the fixed rule gets GO (and no FAIL)'

# ---- each condition broken on its own ----
# (1) End slot 1 / Start slot 1 stable
$s = Get-Summary 'go_endmoves'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('1 end-stable'); Assert-Cond $s 0 '1 end-stable' 'FAIL' 'End slot 1 CHANGES'
Assert-CheckStatus $s 0 'end-stable' 'WARN' 'End slot 1 change'
Assert-CheckStatus $s 1 'end-stable' 'INFO'
$s = Get-Summary 'go_startmoves'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('1 end-stable'); Assert-Cond $s 0 '1 end-stable' 'FAIL' 'Start slot 1 CHANGES'
Assert-CheckStatus $s 0 'end-stable' 'WARN' 'Start slot 1'
Assert-CheckStatus $s 0 'epoch' 'PASS' '[1-9]\d* moved pointer slot'
Assert-CheckStatus $s 0 'start-end-link' 'INFO' 'in 2 of 2 evaluation'
$s = Get-Summary 'go_endptrs'
Assert-Verdict $s 0 'NO-GO'; Assert-Cond $s 0 '1 end-stable' 'UNKNOWN' 'the End launch has no readable first two 8-byte parameters \(pmode ptrs'
Assert-Cond $s 0 '5a end-users-before' 'UNKNOWN' 'End slot 1 is not readable'
Assert-CheckStatus $s 0 'end-stable' 'WARN' 'cannot be judged, missing field\(s\): eval 1: the End launch has no readable'
Assert-CheckStatus $s 0 'end-users' 'WARN' 'cannot be evaluated, missing field'
Write-Output 'PASS: (1) a moving End slot 1, a moving Start slot 1 and an unreadable End launch each stop GO and are named'

# (2) LEAK pointers: only cc_cb_clear may share them; names that poll or publish are the nvdisasm trigger
$s = Get-Summary 'go_leakdata'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('2a leaks-not-clear'); Assert-NoCond $s 0 '2b waiters-outside'
Assert-CheckStatus $s 0 'waiters-outside' 'PASS'; Assert-CheckStatus $s 0 'inv1-leak' 'FAIL'
Assert-CheckStatus $s 0 'clear-before' 'PASS'
Assert-Match $s '0xc400700000\s+\[not sentinel-clear\]\s+outside launches 4 in 2 kernel\(s\)' 'outside sharers not listed'
Assert-Match $s 'cc_after_data x2\s+\(after End; ordinary kernel by name\)' 'the ordinary outside sharer should be named'
Assert-Match $s 'cc_cb_clear x2\s+\(before Start; the clear kernel\)' 'the clear kernel should be named'
# a name that merely contains "wait" is not a waiter: the test is on the end of the name (after the precision suffix)
$s = Get-Summary 'go_leakwaiter'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('2a leaks-not-clear'); Assert-CheckStatus $s 0 'waiters-outside' 'PASS'
foreach ($v in @(@('go_wait', 'cc_after_wait_fp8'), @('go_chained', 'cc_after_chained'), @('go_tilesync', 'cc_after_tilesync_fp8'), @('go_publish', 'cc_after_publish_fp8'), @('go_unknown', '\?'))) {
    $s = Get-Summary $v[0]
    Assert-Verdict $s 0 'NEEDS-NVDISASM'
    Assert-OnlyBroken $s 0 @('2b waiters-outside')
    Assert-NoCond $s 0 '2a leaks-not-clear'
    Assert-Cond $s 0 '2b waiters-outside' 'FAIL' 'nvdisasm trigger'
    Assert-CheckStatus $s 0 'waiters-outside' 'FAIL' $v[1]
    Assert-CheckStatus $s 0 'inv1-leak' 'FAIL'
    $strict = Run-Summary @('-Capture', "$Out/caps/$($v[0])/capture.txt", '-Strict')
    if ($strict.Code -ne 1) { throw "$($v[0]) exited $($strict.Code) under -Strict, expected 1 (waiters-outside FAIL)" }
}
Write-Output 'PASS: (2) an ordinary outside sharer is NO-GO, waiter/publisher/unknown names (with and without the _fp8 suffix) fire the nvdisasm trigger, a name that only contains the word does not'

# (3) cc_cb_clear before Start in every evaluation, with a pointer that is a LEAK pointer
$s = Get-Summary 'go_clearafter'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('3 clear-before'); Assert-CheckStatus $s 0 'clear-before' 'FAIL' 'no cc_cb_clear launch before Start in eval 1, 3'
$s = Get-Summary 'go_clearlast'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('3 clear-before'); Assert-CheckStatus $s 0 'clear-before' 'FAIL' 'no cc_cb_clear launch before Start in eval 3'
$s = Get-Summary 'go_clearother'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('3 clear-before'); Assert-CheckStatus $s 0 'clear-before' 'FAIL' 'its pointer is not one of the 1 LEAK pointer'
Assert-CheckStatus $s 0 'waiters-outside' 'PASS'
Write-Output 'PASS: (3) a clear after End, a clear missing from the last evaluation, and a clear with another pointer each stop GO'

# (4) no counter or epoch in the Inner kernels' parameters; a moved pointer is not one
$s = Get-Summary 'go_epoch'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('4 epoch'); Assert-CheckStatus $s 0 'epoch' 'WARN' 'qkv_chained_fp8 \(inner #2\) \+24: 0x1 vs 0x3'
$s = Get-Summary 'go_movedscratch'
Assert-Verdict $s 0 'GO'; Assert-CheckStatus $s 0 'epoch' 'PASS' '[1-9]\d* moved pointer slot'
Write-Output 'PASS: (4) an evaluation counter in the parameters is an epoch WARN (NO-GO), a moved scratch pointer is not'

# (5) End slot 1: no user before Start; users after End must be decoded
$s = Get-Summary 'go_endbefore'
Assert-Verdict $s 0 'NO-GO'; Assert-OnlyBroken $s 0 @('5a end-users-before'); Assert-CheckStatus $s 0 'end-users' 'FAIL' 'BEFORE Start carry End slot 1: cc_before_user'
Assert-Match $s 'End\.p1\s+0xc400400000\s+\d+\s+before Start\s+cc_before_user' 'end-users table row'
$s = Get-Summary 'go_endafter'
Assert-Verdict $s 0 'NEEDS-DECODE'; Assert-OnlyBroken $s 0 @('5b end-users-after'); Assert-Cond $s 0 '5b end-users-after' 'DECODE' 'cc_dec_out'
Assert-CheckStatus $s 0 'end-users' 'WARN' 'after End, End slot 1 is also used by: cc_dec_out'
Assert-Match $s 'End\.p1\s+0xc400400000\s+\d+\s+after End\s+cc_dec_out' 'end-users table row'
$s = Get-Summary 'go_xfeature'
Assert-Verdict $s 0 'GO'   # not part of the fixed rule: reported by end-xfeature only
Assert-CheckStatus $s 0 'end-xfeature' 'WARN' 'cc_other_pass'
Assert-CheckStatus $s 1 'end-xfeature' 'PASS'
Assert-Match $s 'End\.p1\s+0xc400400000\s+\d+\s+evaluation 2 \(feature 1\)\s+cc_other_pass' 'cross-feature user row'
Write-Output 'PASS: (5) a user of End slot 1 before Start is NO-GO, one after End needs a decode, a use by another pass is reported'

# ---- what the real captures look like ----
$real = Get-Summary 'realistic'
Assert-Verdict $real 0 'NO-GO'
Assert-OnlyBroken $real 0 @('2a leaks-not-clear', '2b waiters-outside', '3 clear-before', '5b end-users-after')
Assert-CheckStatus $real 0 'clear-before' 'FAIL' 'its pointer is not one of the 1 LEAK pointer'
Assert-CheckStatus $real 0 'waiters-outside' 'FAIL' 'cc_after_chained_fp8'
Assert-CheckStatus $real 0 'clear-range' 'INFO' '\b([1-9]\d*) of \d+ distinct pointer values of the Inner kernels lie inside, 0 of those are also used outside'
Assert-Match $real 'cc_cb_clear \(eval 1, ord 0\): pointer 0xc4002ff000, second parameter 16384;' 'clear range line'
Write-Output 'PASS: a clear kernel whose pointer is a region base is reported as clear-before FAIL, with the region coverage as context'

# ---- the original single-feature checks keep their meaning with the per-feature code ----
$rot = Get-Summary 'rotate'
Assert-CheckStatus $rot 0 'end-stable' 'WARN' 'End slot 1'
$leakS = Get-Summary 'leak'
Assert-CheckStatus $leakS 0 'inv1-leak' 'FAIL' '1 pointer value'
Assert-CheckStatus $leakS 0 'waiters-outside' 'PASS'
Assert-CheckStatus $leakS 0 'clear-before' 'FAIL' 'no cc_cb_clear launch before Start in eval 1, 2, 3'
$bareS = Get-Summary 'bare'
Assert-CheckStatus $bareS 0 'outside' 'WARN'
Assert-CheckStatus $bareS 0 'waiters-outside' 'WARN' 'cannot be evaluated'
Assert-Verdict $bareS 0 'NO-GO'; Assert-Cond $bareS 0 '2 leaks' 'UNKNOWN'
Assert-NoMatch $good 'feature 0:' 'single-feature captures keep the old check texts (no feature prefix)'
Write-Output 'PASS: single-feature captures keep their check texts and statuses'

# ---- -PassThru carries the rule ----
$obj = & pwsh -NoProfile -Command "& '$script' -Capture '$Out/caps/go/capture.txt' -PassThru | Select-Object -Last 1 | ConvertTo-Json -Depth 5 -Compress"
$o = $obj | ConvertFrom-Json
if (@($o.Rules).Count -ne 2 -or @($o.Rules)[0].Verdict -ne 'GO' -or @($o.Rules)[1].Verdict -ne 'NOT-JUDGED') { throw "-PassThru Rules wrong: $obj" }
Write-Output 'PASS: -PassThru returns the rule result per feature'
