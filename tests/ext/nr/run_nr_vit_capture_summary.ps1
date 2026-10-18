# Runner (tools\test.ps1 `Runner`): the capture format written by DlssNrVitCapture.h against the parser and the checks of
# tools\nr-vit-capture-summary.ps1. nr_vit_capture_synth.cpp writes synthetic captures with the real formatter (a clean one, and
# ones with a leaked sync counter, a foreign kernel inside the run, no outside launches, a rotating End output and a changed
# order); the script is run on each and must say what is wrong with them. Also exercises -Log and -Baseline.
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
