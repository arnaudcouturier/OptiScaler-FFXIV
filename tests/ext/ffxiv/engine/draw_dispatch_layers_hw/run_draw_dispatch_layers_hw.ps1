param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$results = $env:OPTI_DRAW_DISPATCH_RESULTS
if (-not $results) { throw 'Set OPTI_DRAW_DISPATCH_RESULTS to the requested analysis/draw-dispatch-layers-hw directory.' }
New-Item -ItemType Directory -Force -Path $results | Out-Null
$manifestPath = $env:OPTI_DRAW_DISPATCH_MANIFEST
if (-not $manifestPath) { throw 'Set OPTI_DRAW_DISPATCH_MANIFEST to the supplied 75-file frozen manifest.' }
$manifest = @(Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json)
if ($manifest.Count -ne 75) { throw "Expected 75 frozen source files, manifest has $($manifest.Count)." }
function Get-ManifestState {
    $state = @()
    foreach ($entry in $manifest) {
        $path = Join-Path $Repo $entry.path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Guard file missing: $($entry.path)" }
        $item = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $state += [pscustomobject]@{ path = $entry.path; bytes = $item.Length; sha256 = $hash }
        if ($item.Length -ne $entry.bytes -or $hash -ne $entry.sha256) { throw "Frozen manifest mismatch before/after: $($entry.path)" }
    }
    return ,$state
}
$before = Get-ManifestState
Set-Content -LiteralPath (Join-Path $results 'manifest-pre.json') -Value ($before | ConvertTo-Json -Depth 3) -Encoding utf8

& pwsh -NoProfile -File (Join-Path $Repo 'tests\ext\ffxiv\engine\prepare_ssr.ps1') -Repo $Repo -Out $Out
if ($LASTEXITCODE -ne 0) { throw 'Production SSR shader preparation failed.' }
$source = Join-Path $PSScriptRoot 'draw_dispatch_layers_hw.cpp'
$sources = @($source, (Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\SsrPass.cpp'),
    (Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\SsrApply.cpp'),
    (Join-Path $Repo 'OptiScaler\ext\ffxiv\engine\SsrSwap.cpp'),
    (Join-Path $Repo 'OptiScaler\library\detours\detours.lib'))
$includes = @((Join-Path $Repo 'tests\ext\ffxiv\engine'), (Join-Path $Repo 'tests\ext\ffxiv\engine\stubs'),
    (Join-Path $Repo 'OptiScaler'), (Join-Path $Repo 'OptiScaler\include'), $Out)
$clArgs = @('/nologo', '/std:c++latest', '/EHsc', '/W4', '/DNOMINMAX', '/DUNDEBUG', "/Fo$Out\", "/Fe$Out\draw_dispatch_layers_hw.exe")
foreach ($inc in $includes) { $clArgs += "/I$inc" }
$clArgs += $sources
$clArgs += @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'user32.lib')
& cl.exe @clArgs 2>&1 | Tee-Object -FilePath (Join-Path $results 'build.log')
if ($LASTEXITCODE -ne 0) { throw "Native GPU test build failed: $LASTEXITCODE" }
$exe = Join-Path $Out 'draw_dispatch_layers_hw.exe'
$runs = @(); $failures = @()
foreach ($order in @('APP_FIRST', 'RESOURCE_FIRST')) {
    foreach ($mode in @('N', 'L', 'P', 'ALL')) {
        foreach ($copy in @(0, 1)) {
            $name = "${order}_${mode}_diagCopy${copy}"
            $log = Join-Path $results "$name.log"
            Write-Host "[hardware] $name"
            & $exe $mode "$copy" $order 2>&1 | Tee-Object -FilePath $log
            $code = $LASTEXITCODE
            $runs += [pscustomobject]@{ case = $name; exit = $code; log = $log }
            if ($code -ne 0) { $failures += "$name exit=$code" }
        }
    }
}
foreach ($mode in @('L', 'P')) {
    $name = "LEGACY_CACHE_${mode}_diagCopy0"
    $log = Join-Path $results "$name.log"
    Write-Host "[hardware] $name (expected blue negative control)"
    & $exe $mode '0' 'LEGACY_CACHE' 2>&1 | Tee-Object -FilePath $log
    $code = $LASTEXITCODE
    $runs += [pscustomobject]@{ case = $name; exit = $code; expected = 'blue-0-coverage'; log = $log }
    if ($code -ne 0) { $failures += "$name exit=$code" }
}
$runs | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $results 'runs.json') -Encoding utf8
$after = Get-ManifestState
Set-Content -LiteralPath (Join-Path $results 'manifest-post.json') -Value ($after | ConvertTo-Json -Depth 3) -Encoding utf8
if ($before.Count -ne $after.Count) { throw 'Frozen manifest record count changed.' }
Write-Host "[guard] $($after.Count)/75 frozen files match byte-for-byte and SHA-256 before/after."
if ($failures.Count) { throw ($failures -join ', ') }
Write-Host "[hardware] all $($runs.Count) GPU scenarios passed; logs: $results"
