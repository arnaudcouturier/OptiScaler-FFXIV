<#
.SYNOPSIS
  Builds and runs the tests under tests\ext. Each test has a recipe file next to its source.
.DESCRIPTION
  Recipe = tests\ext\<area>\<name>.test.psd1, a PowerShell data file:

    @{
        Class       = 'warp'                       # cpu: no GPU | warp: D3D12 software adapter | local: needs this machine, run only with
                                                   # -Local | manual: listed, never run
        Sources     = @('gpu_lifetime_smoke.cpp',  # relative to the recipe folder; '{repo}' = repository root
                        '{repo}/OptiScaler/ext/gpu/GpuLifetime.cpp')
        IncludeDirs = @('stubs', '{repo}/OptiScaler')   # optional, same path rules (stubs\pch.h replaces the product pch)
        Libs        = @('d3d12.lib', 'dxgi.lib', 'ole32.lib')   # optional; an entry with '{repo}' / '{env:NAME}' is expanded as a path
        Defines     = @('NOMINMAX')                # optional, "NAME" or "NAME=VALUE"
        Args        = @()                          # optional, command-line arguments of the test exe ('{repo}', '{out}', '{env:NAME}' expand)
        Requires    = @('SOME_ENV', '{repo}/x64/Release/a/OptiScaler.dll')
                                                   # optional, Class local only: every entry must exist, else the test is NOT-RUN (not a failure).
                                                   # An entry that looks like a name (letters, digits, underscore) is an environment variable that
                                                   # must be set and point at an existing file or folder; any other entry is a path ('{repo}',
                                                   # '{env:NAME}' expand) that must exist (a missing '{repo}' path reports "no product build here").
        Gpu         = $true                        # optional, Class local only: a real GPU is used; NOT-RUN when -GpuBlocked is given
        Runner      = 'run_x.ps1'                  # optional, replaces the build and run steps for tests that need several builds
                                                   # (negative controls, sliced production source): run under the vcvars environment as
                                                   #   pwsh -File <script> -Repo <repo root> -Out <build folder>
                                                   # exit code 0 = pass. Sources then only lists the .cpp files the script builds.
        Prepare     = 'prepare_x.ps1'              # optional, PowerShell script run before the build as
                                                   #   pwsh -File <script> -Repo <repo root> -Out <build folder>
                                                   # (stages generated files such as sliced production source; a non-zero exit fails the test)
        Std         = 'c++20'                      # optional
        Note        = 'one line for -List'         # optional (required for Class manual: what and why)
    }

  The test name is the recipe file name minus .test.psd1. Every test is a standalone cl.exe build into a temp folder
  (vcvars64 from the VS install is located with vswhere) followed by one run; exit code 0 = pass.
  '{out}' in IncludeDirs/Sources/Args is the test's own build folder (where Prepare writes).
  '{env:NAME}' in IncludeDirs/Sources/Args expands to the value of the environment variable NAME (empty when unset).
  Tests that link production source stub pch.h/Util.h (see tests\ext\gpu\stubs) instead of building the product.

  Classes: cpu and warp always run. local tests need this machine (a real GPU, the real NVIDIA / Streamline DLLs, a desktop session,
  a product build) and run only with -Local: without it each prints
    [test] <area>/<name>  LOCAL  <Note>
  and is counted as "local". With -Local a test whose Requires entry is missing prints
    [test] <area>/<name>  NOT-RUN (missing <ENV>)
  and is counted as "not-run"; that is not a failure. manual tests are listed, never run.

  Usage:  pwsh -File tools\test.ps1 [-Filter <wildcard on name or area>] [-List] [-Local] [-GpuBlocked <reason>] [-TestsDir <folder>]
    -Local       also run the local tests
    -GpuBlocked  with -Local: local tests marked Gpu are NOT-RUN with this reason (for example while the game is running)
    -TestsDir    recipe folder (default tests\ext); lets the runner be tested on fixture recipes
  Exit code: number of failed tests (0 = all passed). Manual, local and not-run tests are listed, not counted as failures.
  Last line: [test] passed X, failed Y, manual Z, local L, not-run N   (local = local tests skipped for lack of -Local)
#>
[CmdletBinding()]
param(
    [string]$Filter = '*',
    [switch]$List,
    [switch]$Local,
    [string]$GpuBlocked = '',
    [string]$TestsDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = if ($TestsDir) { [IO.Path]::GetFullPath($TestsDir) } else { Join-Path $root 'tests\ext' }
if (-not (Test-Path $dir)) { Write-Host '[test] tests\ext: no tests yet (nothing to run)'; exit 0 }

# '{env:NAME}' -> value of the environment variable (empty when unset)
function Expand-Env([string]$p) {
    return [regex]::Replace($p, '\{env:([A-Za-z0-9_]+)\}', { param($m) [string][Environment]::GetEnvironmentVariable($m.Groups[1].Value) })
}

function Expand-Path([string]$p, [string]$base, [string]$out = '') {
    $p = (Expand-Env $p).Replace('{repo}', $root).Replace('{out}', $out)
    if ([IO.Path]::IsPathRooted($p)) { return [IO.Path]::GetFullPath($p) }
    return [IO.Path]::GetFullPath((Join-Path $base $p))
}

$recipes = @(Get-ChildItem $dir -Recurse -File -Filter *.test.psd1 | Sort-Object FullName)
$known = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($r in $recipes) { foreach ($s in @((Import-PowerShellDataFile $r.FullName).Sources)) { [void]$known.Add((Split-Path $s -Leaf)) } }
$orphans = @(Get-ChildItem $dir -Recurse -File -Filter *.cpp | Where-Object { -not $known.Contains($_.Name) -and $_.FullName -notmatch '[\\/]stubs[\\/]' })

if ($List) {
    foreach ($r in $recipes) {
        $t = Import-PowerShellDataFile $r.FullName
        '{0,-8} {1}  {2}' -f $t.Class, $r.Name.Replace('.test.psd1', ''), $t.Note
    }
    return
}

$vcvars = $env:OPTISCALER_VCVARS
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $install = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 }
    $vcvars = if ($install) { Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat' }
    if (-not $vcvars -or -not (Test-Path $vcvars)) { $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat' }
}
if (-not (Test-Path $vcvars)) { Write-Error 'vcvars64.bat not found (set OPTISCALER_VCVARS)'; exit 2 }

# Local-test requirements: returns '' when all are met, else the text for "NOT-RUN (...)".
function Get-MissingRequirement($t) {
    foreach ($req in @($t.Requires)) {
        if (-not $req) { continue }
        # an {env:NAME} inside a path must be set, else the path collapses (e.g. to the repo root) and would pass
        foreach ($m in [regex]::Matches($req, '\{env:([A-Za-z0-9_]+)\}')) {
            if (-not [Environment]::GetEnvironmentVariable($m.Groups[1].Value)) { return "missing $($m.Groups[1].Value)" }
        }
        if ($req -match '^[A-Za-z_][A-Za-z0-9_]*$') {
            $v = [Environment]::GetEnvironmentVariable($req)
            if (-not $v) { return "missing $req" }
            if (-not (Test-Path -LiteralPath $v)) { return "missing ${req}: no such path" }
        }
        elseif (-not (Test-Path -LiteralPath (Expand-Path $req $root))) {
            if ($req.StartsWith('{repo}')) { return 'no product build here: ' + $req.Substring(6).TrimStart('/', '\') }
            return "missing $req"
        }
    }
    return ''
}

$passed = 0; $failed = @(); $manual = 0; $localListed = 0; $notRun = 0
foreach ($r in $recipes) {
    $name = $r.Name.Replace('.test.psd1', '')
    $area = Split-Path (Split-Path $r.FullName -Parent) -Leaf
    if ($name -notlike $Filter -and $area -notlike $Filter) { continue }
    $t = Import-PowerShellDataFile $r.FullName
    $base = $r.DirectoryName
    if ($t.Class -eq 'manual') { Write-Host "[test] $area/$name  MANUAL  $($t.Note)"; $manual++; continue }
    if ($t.Class -eq 'local') {
        if (-not $Local) { Write-Host "[test] $area/$name  LOCAL  $($t.Note)"; $localListed++; continue }
        if ($GpuBlocked -and $t.Gpu) { Write-Host "[test] $area/$name  NOT-RUN ($GpuBlocked)"; $notRun++; continue }
        $missing = Get-MissingRequirement $t
        if ($missing) { Write-Host "[test] $area/$name  NOT-RUN ($missing)"; $notRun++; continue }
    }

    $out = Join-Path ([IO.Path]::GetTempPath()) ("optiscaler-test-$name-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $out | Out-Null
    if ($t.Runner) {
        Write-Host "[test] $area/$name  ($($t.Class)) running..."
        $rcmd = 'call "{0}" >nul && pwsh -NoProfile -File "{1}" -Repo "{2}" -Out "{3}"' -f $vcvars, (Join-Path $base $t.Runner), $root, $out
        $runnerOut = & cmd.exe /c $rcmd 2>&1
        $rcode = $LASTEXITCODE
        $runnerOut | ForEach-Object { Write-Host "    $_" }
        if ($rcode -eq 0) { Write-Host "[test] $area/$name  PASS"; $passed++; Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue }
        else { Write-Host "[test] $area/$name  FAIL (exit $rcode, kept $out)"; $failed += "$area/$name (exit $rcode)" }
        continue
    }
    if ($t.Prepare) {
        Write-Host "[test] $area/$name  preparing..."
        $prepOut = & pwsh -NoProfile -File (Join-Path $base $t.Prepare) -Repo $root -Out $out 2>&1
        if ($LASTEXITCODE -ne 0) {
            $prepOut | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
            Write-Host "[test] $area/$name  FAIL (prepare)"; $failed += "$area/$name (prepare)"; continue
        }
    }
    $std = if ($t.Std) { $t.Std } else { 'c++20' }
    $cl = @('/nologo', "/std:$std", '/EHsc', '/W4', "/Fo$out\", "/Fe$out\$name.exe")
    foreach ($d in @($t.Defines)) { if ($d) { $cl += "/D$d" } }
    foreach ($i in @($t.IncludeDirs)) { if ($i) { $cl += ('/I"{0}"' -f (Expand-Path $i $base $out)) } }
    foreach ($s in @($t.Sources)) { $cl += ('"{0}"' -f (Expand-Path $s $base $out)) }
    foreach ($l in @($t.Libs)) { if ($l) { $cl += $(if ($l.Contains('{')) { '"{0}"' -f (Expand-Path $l $base $out) } else { $l }) } }
    $cmd = 'call "{0}" >nul && cl.exe {1}' -f $vcvars, ($cl -join ' ')

    Write-Host "[test] $area/$name  ($($t.Class)) building..."
    $buildOut = & cmd.exe /c $cmd 2>&1
    if ($LASTEXITCODE -ne 0) {
        $buildOut | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
        Write-Host "[test] $area/$name  FAIL (build)"; $failed += "$area/$name (build)"; continue
    }
    $testArgs = @(@($t.Args) | Where-Object { $_ } | ForEach-Object { (Expand-Env $_).Replace('{repo}', $root).Replace('{out}', $out) })
    $runOut = & "$out\$name.exe" @testArgs 2>&1
    $code = $LASTEXITCODE
    $runOut | ForEach-Object { Write-Host "    $_" }
    if ($code -eq 0) { Write-Host "[test] $area/$name  PASS"; $passed++; Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue }
    else { Write-Host "[test] $area/$name  FAIL (exit $code, kept $out)"; $failed += "$area/$name (exit $code)" }
}

foreach ($o in $orphans) { Write-Host ("[test] no recipe: " + $o.FullName.Substring($root.Length + 1)) }
Write-Host ("[test] passed {0}, failed {1}, manual {2}, local {3}, not-run {4}" -f $passed, $failed.Count, $manual, $localListed, $notRun)
$failed | ForEach-Object { Write-Host "  FAILED: $_" }
exit $failed.Count
