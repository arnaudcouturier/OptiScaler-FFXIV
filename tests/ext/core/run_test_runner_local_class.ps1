# Runner (tools\test.ps1 `Runner`): tests the recipe runner itself on fixture recipes written into <Out> at run time (nothing in the tree
# matches *.test.psd1, so the normal run never sees them) and run with `test.ps1 -TestsDir`. Covers the `local` class:
#   - not run without -Local (printed as LOCAL and counted as "local"), no NOT-RUN line, no "SKIP:" text
#   - run with -Local; a missing Requires entry (unset variable, variable pointing nowhere, missing {repo} path) is NOT-RUN with its reason
#     and is not a failure; the exit code is the number of FAILED tests only
#   - {env:NAME} expands in Args, Sources and IncludeDirs
#   - -GpuBlocked turns Gpu local tests into NOT-RUN
#   - the summary line "[test] passed X, failed Y, manual Z, local L, not-run N"
#   - manual recipes are listed and never run, per-test PASS / FAIL line formats unchanged
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$testPs1 = Join-Path $Repo 'tools\test.ps1'
$fx = Join-Path $Out 'fixtures'
$area = Join-Path $fx 'fxa'
$srcDir = Join-Path $Out 'srcdir'
$childTemp = Join-Path $Out 'tmp'
New-Item -ItemType Directory -Force -Path $area, $srcDir, $childTemp | Out-Null

# The only test source: passes when its first argument is exactly "ok".
[IO.File]::WriteAllText((Join-Path $srcDir 'ok.cpp'), "#include <cstring>`nint main(int argc, char** argv) { return argc > 1 && std::strcmp(argv[1], `"ok`") == 0 ? 0 : 1; }`n")
# loc_ok only: the expected argument comes from a header that is found through IncludeDirs alone (angle-bracket include, own folder)
New-Item -ItemType Directory -Force -Path (Join-Path $srcDir 'inc') | Out-Null
[IO.File]::WriteAllText((Join-Path (Join-Path $srcDir 'inc') 't1fx_marker.h'), "#define T1FX_EXPECTED `"ok`"`n")
[IO.File]::WriteAllText((Join-Path $srcDir 'inc.cpp'), "#include <cstring>`n#include <t1fx_marker.h>`nint main(int argc, char** argv) { return argc > 1 && std::strcmp(argv[1], T1FX_EXPECTED) == 0 ? 0 : 1; }`n")

function Write-Recipe([string]$name, [string]$body) {
    [IO.File]::WriteAllText((Join-Path $area "$name.test.psd1"), "@{`n$body`n}`n")
}
# cpu: always runs
Write-Recipe 'cpu_ok' "Class = 'cpu'`nNote = 'cpu fixture'`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('ok')"
# local, requirements met, {env:} in Sources, IncludeDirs and Args
Write-Recipe 'loc_ok' "Class = 'local'`nNote = 'ok fixture'`nRequires = @('T1FX_FILE')`nSources = @('{env:T1FX_DIR}/inc.cpp')`nIncludeDirs = @('{env:T1FX_DIR}/inc')`nArgs = @('{env:T1FX_VAL}')"
# local, Requires names an unset variable
Write-Recipe 'loc_unset' "Class = 'local'`nNote = 'unset fixture'`nRequires = @('T1FX_UNSET')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('ok')"
# local, Requires names a variable that points at a path that does not exist
Write-Recipe 'loc_nopath' "Class = 'local'`nNote = 'nopath fixture'`nRequires = @('T1FX_GONE')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('ok')"
# local, Requires names a repo-relative file that is not there (no product build)
Write-Recipe 'loc_repo' "Class = 'local'`nNote = 'repo fixture'`nRequires = @('{repo}/no/such/OptiScaler.dll')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('ok')"
# local, Requires is an {env:} reference to an unset variable (must not expand to the repo root and pass)
Write-Recipe 'loc_envunset' "Class = 'local'`nNote = 'envunset fixture'`nRequires = @('{env:T1FX_UNSET}')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('ok')"
# local, Requires is a plain repo-relative path that exists (resolved against the repo root, not the recipe folder)
Write-Recipe 'loc_xrel' "Class = 'local'`nNote = 'xrel fixture'`nRequires = @('tools/test.ps1')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('ok')"
# local, Gpu
Write-Recipe 'loc_gpu' "Class = 'local'`nGpu = `$true`nNote = 'gpu fixture'`nRequires = @('T1FX_FILE')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('ok')"
# local, runs and fails
Write-Recipe 'loc_fail' "Class = 'local'`nNote = 'fail fixture'`nRequires = @('T1FX_FILE')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('bad')"
# local, runs and fails (a second one: the exit code must be the NUMBER of failures)
Write-Recipe 'loc_fail2' "Class = 'local'`nNote = 'fail2 fixture'`nRequires = @('T1FX_FILE')`nSources = @('{env:T1FX_DIR}/ok.cpp')`nArgs = @('bad2')"
# manual
Write-Recipe 'man' "Class = 'manual'`nNote = 'manual fixture'`nSources = @()"

$env:T1FX_DIR = $srcDir
$env:T1FX_FILE = Join-Path $srcDir 'ok.cpp'
$env:T1FX_VAL = 'ok'
$env:T1FX_GONE = Join-Path $Out 'does-not-exist'
Remove-Item Env:\T1FX_UNSET -ErrorAction SilentlyContinue

$script:problems = @()
function Fail([string]$why) { $script:problems += $why; Write-Host "  ASSERT FAILED: $why" }
function Run-Case([string]$label, [string[]]$extra) {
    $oldTemp = $env:TEMP; $oldTmp = $env:TMP
    $env:TEMP = $childTemp; $env:TMP = $childTemp
    try {
        $text = (& pwsh -NoProfile -File $testPs1 -TestsDir $fx @extra 2>&1 | ForEach-Object { "$_" }) -join "`n"
        $code = $LASTEXITCODE
    }
    finally { $env:TEMP = $oldTemp; $env:TMP = $oldTmp }
    Write-Host "--- case: $label (exit $code)"
    $text -split "`n" | Where-Object { $_ -match '^\[test\]' } | ForEach-Object { Write-Host "    $_" }
    return [pscustomobject]@{ Text = $text; Code = $code }
}
function Has($r, [string]$pattern, [string]$why) { if ($r.Text -cnotmatch $pattern) { Fail "$why (expected /$pattern/)" } }
function Lacks($r, [string]$pattern, [string]$why) { if ($r.Text -cmatch $pattern) { Fail "$why (unexpected /$pattern/)" } }
function Summary($r, [string]$expected, [string]$why) {
    $line = ($r.Text -split "`n" | Where-Object { $_ -match '^\[test\] passed ' } | Select-Object -Last 1)
    if ($line -ne $expected) { Fail "${why}: summary '$line' expected '$expected'" }
}
function ExitIs($r, [int]$n, [string]$why) { if ($r.Code -ne $n) { Fail "${why}: exit code $($r.Code) expected $n" } }

# Case 1: no -Local
$r = Run-Case 'no -Local' @()
Has $r '(?m)^\[test\] fxa/cpu_ok  PASS$' 'cpu test runs and prints the unchanged PASS line'
foreach ($n in 'loc_ok', 'loc_unset', 'loc_envunset', 'loc_xrel', 'loc_nopath', 'loc_repo', 'loc_gpu', 'loc_fail', 'loc_fail2') {
    Has $r "(?m)^\[test\] fxa/$n  LOCAL  \w+ fixture$" "$n is listed as LOCAL with its note"
}
Lacks $r 'NOT-RUN' 'no NOT-RUN without -Local'
Lacks $r 'SKIP:' 'never prints SKIP:'
Lacks $r '(?m)^\[test\] fxa/loc_\w+  (PASS|FAIL)' 'local tests do not run without -Local'
Has $r '(?m)^\[test\] fxa/man  MANUAL  manual fixture$' 'manual is listed'
Summary $r '[test] passed 1, failed 0, manual 1, local 9, not-run 0' 'case 1'
ExitIs $r 0 'case 1'

# Case 2: -Local with the GPU blocked
$r = Run-Case '-Local -GpuBlocked' @('-Local', '-GpuBlocked', 'game running')
Has $r '(?m)^\[test\] fxa/loc_ok  PASS$' 'loc_ok runs (Requires met; {env:} expanded in Sources, IncludeDirs and Args, else it would fail to build or exit 1)'
Has $r '(?m)^\[test\] fxa/loc_xrel  PASS$' 'a plain repo-relative Requires path is found from the repo root'
Has $r '(?m)^\[test\] fxa/loc_fail  FAIL \(exit 1, kept ' 'loc_fail runs and fails with the unchanged FAIL format'
Has $r '(?m)^\[test\] fxa/loc_fail2  FAIL \(exit 1, kept ' 'loc_fail2 runs and fails'
Has $r '(?m)^\[test\] fxa/loc_unset  NOT-RUN \(missing T1FX_UNSET\)$' 'unset variable'
Has $r '(?m)^\[test\] fxa/loc_envunset  NOT-RUN \(missing T1FX_UNSET\)$' '{env:} reference to an unset variable'
Has $r '(?m)^\[test\] fxa/loc_nopath  NOT-RUN \(missing T1FX_GONE: no such path\)$' 'variable pointing at a missing path'
Has $r '(?m)^\[test\] fxa/loc_repo  NOT-RUN \(no product build here: no/such/OptiScaler\.dll\)$' 'missing repo path'
Has $r '(?m)^\[test\] fxa/loc_gpu  NOT-RUN \(game running\)$' 'Gpu test blocked'
Lacks $r '(?m)^\[test\] fxa/\w+  LOCAL' 'nothing is listed as LOCAL with -Local'
Lacks $r 'SKIP:' 'never prints SKIP:'
Has $r '(?m)^  FAILED: fxa/loc_fail \(exit 1\)$' 'loc_fail is listed as failed'
Has $r '(?m)^  FAILED: fxa/loc_fail2 \(exit 1\)$' 'loc_fail2 is listed as failed'
Lacks $r '(?m)^  FAILED: fxa/(loc_unset|loc_envunset|loc_nopath|loc_repo|loc_gpu)' 'NOT-RUN is not a failure'
Summary $r '[test] passed 3, failed 2, manual 1, local 0, not-run 5' 'case 2'
ExitIs $r 2 'case 2 (exit code = number of failed tests, not-run excluded)'

# Case 3: -Local, GPU allowed, the failing one filtered out
$r = Run-Case '-Local -Filter' @('-Local', '-Filter', 'loc_[go]*')
Has $r '(?m)^\[test\] fxa/loc_gpu  PASS$' 'Gpu test runs when not blocked'
Has $r '(?m)^\[test\] fxa/loc_ok  PASS$' 'loc_ok runs'
Lacks $r 'fxa/(loc_fail|loc_unset|loc_nopath|loc_repo|cpu_ok|man)' 'filter excludes the rest'
Summary $r '[test] passed 2, failed 0, manual 0, local 0, not-run 0' 'case 3'
ExitIs $r 0 'case 3'

# Case 4: all requirements met but only not-run tests selected: exit 0
$r = Run-Case '-Local only not-run' @('-Local', '-Filter', 'loc_[unr]*')
Has $r 'NOT-RUN' 'not-run tests reported'
Summary $r '[test] passed 0, failed 0, manual 0, local 0, not-run 3' 'case 4'
ExitIs $r 0 'case 4 (not-run only: exit code 0)'

if ($script:problems.Count -gt 0) {
    Write-Host "test_runner_local_class: $($script:problems.Count) problem(s)"
    exit 1
}
Write-Host 'test_runner_local_class: all cases pass'
exit 0
