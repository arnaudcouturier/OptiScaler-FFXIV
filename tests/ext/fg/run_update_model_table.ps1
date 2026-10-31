# Runner for tools/fg-probe/Update-ModelTable.ps1 (the data file and the generated rows the product compiles in).
#
# It feeds the script a saved host output that contains what a real run produces: a clean M1 row, a clean M2 row with the
# companion DLL its configuration needs, a row with the old "witness=not honoured" spelling, and a verdict=unknown line
# (the host's own answer for a noisy measurement or a library that could not create a chain). The unknown line must be
# skipped with a message and must not abort the run; the old spelling must land in the data file as the one spelling it
# takes; a row measured in the wrong configuration for its class, or an XeFG row without the companion build, must be
# refused.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }
$script:failures = 0
$tableScript = Join-Path $Repo 'tools/fg-probe/Update-ModelTable.ps1'

function Assert([bool]$condition, [string]$what, [string]$detail = '') {
    if ($condition) { Write-Host "[ok] $what" }
    else { Write-Host "[FAIL] $what $detail"; $script:failures++ }
}

$data = Join-Path $Out 'model-rows.txt'
$header = Join-Path $Out 'ModelRows.h'
$source = Join-Path $Out 'host-output.txt'

@(
    '# a comment line, as the host writes it'
    'fg-probe host begin runs=3 folder=C:\scratch pageheap=off'
    'fg-probe class=fsrfg run=1 createMs=12 windowAfterDestroy=free stable=1 heldMs=3010 windowAfterPin=bound stable=0 witnessAtPin=alive witnessAfterPin=died-with-object pinRelease=0 skipped=0'
    'fg-probe model class=fsrfg file=amd_fidelityfx_framegeneration_dx12.dll size=111 stamp=65F1A2B3 config=direct verdict=M1 witness=no order=destroyfirst refs=2 ctx=unknown orderB=clean vramDeltaMb=0 pageheap=off runs=3 evidence=the window was free after the destroy in every run'
    'fg-probe model class=xefg file=libxess_fg.dll size=222 stamp=1234ABCD config=xell verdict=M2 witness=honoured order=pinfirst refs=2 companionFile=libxell.dll companionSize=9012 companionStamp=1234AB12 ctx=refused orderB=leaked vramDeltaMb=-2 pageheap=off runs=3 evidence=the window stayed bound until the pin release in every run'
    'fg-probe model class=fsrfg file=amd_fidelityfx_framegeneration_dx12.dll size=333 stamp=65F1A2B4 config=direct verdict=M1 witness=not honoured order=destroyfirst refs=2 ctx=unknown orderB=clean vramDeltaMb=0 pageheap=off runs=3 evidence=the window stayed bound until the pin release in every run'
    'fg-probe model class=xefg file=libxess_fg.dll size=444 stamp=1234ABCE config=xell verdict=unknown witness=no pageheap=off runs=0 evidence=the library could not create a chain'
    'fg-probe host done rows=3'
) | Set-Content -Path $source -Encoding UTF8
@('# empty data file') | Set-Content -Path $data -Encoding UTF8
Remove-Item -Path $header -ErrorAction SilentlyContinue

$text = & pwsh -NoProfile -File $tableScript -Source $source -Data $data -Header $header 2>&1 | Out-String
$code = $LASTEXITCODE
Assert ($code -eq 0) 'a saved output with one unknown line is accepted' "$code`: $text"
Assert ($text -match 'skipped \(verdict=unknown') 'the unknown line is reported and skipped' $text
$rows = @(Get-Content $data -Encoding UTF8 | Where-Object { $_ -and -not $_.StartsWith('#') })
Assert (-not ($rows -match 'size=444')) 'the unknown line added no row' ($rows -join ' | ')
Assert ($rows.Count -eq 3) 'three rows were written (M1, M2 and the legacy spelling)' ($rows -join ' | ')
Assert (@($rows | Where-Object { $_ -match 'size=111 .*verdict=M1 witness=no order=destroyfirst refs=2' }).Count -eq 1) 'the M1 row keeps its verdict and its count' ($rows -join ' | ')
Assert (@($rows | Where-Object { $_ -match 'size=222 .*config=xell verdict=M2 witness=honoured order=pinfirst refs=2 .*companionFile=libxell.dll companionSize=9012 companionStamp=1234AB12' }).Count -eq 1) 'the M2 row keeps its witness and its companion build' ($rows -join ' | ')
Assert (@($rows | Where-Object { $_ -match 'size=222 .*ctx=refused orderB=leaked vramDeltaMb=-2' }).Count -eq 1) 'the run evidence survives the round trip through the script' ($rows -join ' | ')
Assert (@($rows | Where-Object { $_ -match 'size=333 .*witness=no' }).Count -eq 1) 'the old "not honoured" spelling became witness=no' ($rows -join ' | ')
Assert (-not ($rows -match 'not honoured')) 'no row carries the old spelling' ($rows -join ' | ')
Assert (-not ($rows -match 'verdict=unknown')) 'no row carries an unknown verdict' ($rows -join ' | ')

$headerText = Get-Content $header -Raw -Encoding UTF8
Assert ($headerText -match 'Class::FsrFg, "amd_fidelityfx_framegeneration_dx12.dll", 111ULL, 0x65F1A2B3u, "", 0ULL, 0x0u, "direct", Model::M1, false, false, 2, "unknown", "clean", 0') 'the generated FSR row has the identity, the config, the count and the evidence' $headerText
Assert ($headerText -match 'Class::XeFg, "libxess_fg.dll", 222ULL, 0x1234ABCDu, "libxell.dll", 9012ULL, 0x1234AB12u, "xell", Model::M2, true, true, 2, "refused", "leaked", -2') 'the generated XeFG row carries the companion build and the context answer' $headerText

$checkText = & pwsh -NoProfile -File $tableScript -Check -Data $data -Header $header 2>&1 | Out-String
Assert ($LASTEXITCODE -eq 0) 'the pair the script wrote passes -Check' $checkText

# A row measured in the configuration the product does not run is refused, not silently dropped: the data file is the
# reviewed record, so a wrong line is an error a person has to fix.
@(
    'fg-probe model class=xefg file=libxess_fg.dll size=555 stamp=1234ABCF config=direct verdict=M2 witness=honoured order=destroyfirst refs=2 pageheap=off runs=3 evidence=x'
) | Set-Content -Path $source -Encoding UTF8
$badText = & pwsh -NoProfile -File $tableScript -Source $source -Data $data -Header $header 2>&1 | Out-String
Assert ($LASTEXITCODE -ne 0) 'an XeFG row without the XeLL context is refused' $badText
Assert ($badText -match 'config for xefg must be xell') 'the message names the configuration' $badText

# An XeFG row without the companion build is refused too: another libxell.dll is another pair than the one measured.
@(
    'fg-probe model class=xefg file=libxess_fg.dll size=666 stamp=1234ABD0 config=xell verdict=M2 witness=honoured order=pinfirst refs=2 pageheap=off runs=3 evidence=x'
) | Set-Content -Path $source -Encoding UTF8
$noCompanion = & pwsh -NoProfile -File $tableScript -Source $source -Data $data -Header $header 2>&1 | Out-String
Assert ($LASTEXITCODE -ne 0) 'an XeFG row without its companion build is refused' $noCompanion
Assert ($noCompanion -match "companionFile.*missing for xefg") 'the message names the companion field' $noCompanion

# A row without the standstill count cannot answer the preflight's comparison, so it is refused.
@(
    'fg-probe model class=fsrfg file=amd_fidelityfx_framegeneration_dx12.dll size=777 stamp=65F1A2B5 config=direct verdict=M2 witness=honoured order=destroyfirst pageheap=off runs=3 evidence=x'
) | Set-Content -Path $source -Encoding UTF8
$noRefs = & pwsh -NoProfile -File $tableScript -Source $source -Data $data -Header $header 2>&1 | Out-String
Assert ($LASTEXITCODE -ne 0) 'a row without refs is refused' $noRefs
Assert ($noRefs -match 'refs') 'the message names the count' $noRefs

# Another companion build of the same frame-generation library is another pair, not a replacement: both rows stay.
@(
    'fg-probe model class=xefg file=libxess_fg.dll size=222 stamp=1234ABCD config=xell verdict=M2 witness=honoured order=pinfirst refs=2 companionFile=libxell_next.dll companionSize=9013 companionStamp=1234AB13 ctx=unknown orderB=clean vramDeltaMb=0 pageheap=off runs=3 evidence=x'
) | Set-Content -Path $source -Encoding UTF8
$secondCompanion = & pwsh -NoProfile -File $tableScript -Source $source -Data $data -Header $header 2>&1 | Out-String
Assert ($LASTEXITCODE -eq 0) 'a second companion build of the same library is accepted' $secondCompanion
$rows = @(Get-Content $data -Encoding UTF8 | Where-Object { $_ -and -not $_.StartsWith('#') })
Assert (@($rows | Where-Object { $_ -match 'companionFile=libxess?_?n?e?x?t?\.dll|companionFile=libxell' }).Count -eq 2) 'both companion builds are rows of their own' ($rows -join ' | ')

# An M2 row without a witness the library accepted would let a switch release the pin on the table's word alone, so the
# script refuses it instead of writing a row.
@(
    'fg-probe model class=fsrfg file=amd_fidelityfx_framegeneration_dx12.dll size=888 stamp=65F1A2B6 config=direct verdict=M2 witness=no order=destroyfirst refs=2 pageheap=off runs=3 evidence=x'
) | Set-Content -Path $source -Encoding UTF8
$m2NoWitness = & pwsh -NoProfile -File $tableScript -Source $source -Data $data -Header $header 2>&1 | Out-String
Assert ($LASTEXITCODE -ne 0) 'an M2 row without a witness is refused' $m2NoWitness
Assert ($m2NoWitness -match 'M2 row without a witness') 'the message names the rule' $m2NoWitness

# A pin-first row releases a reference before the destroy, so the row has to be M2 with a witness the library accepted.
@(
    'fg-probe model class=xefg file=libxess_fg.dll size=999 stamp=1234ABD1 config=xell verdict=M1 witness=honoured order=pinfirst refs=2 companionFile=libxell.dll companionSize=9012 companionStamp=1234AB12 pageheap=off runs=3 evidence=x'
) | Set-Content -Path $source -Encoding UTF8
$pinFirstNotM2 = & pwsh -NoProfile -File $tableScript -Source $source -Data $data -Header $header 2>&1 | Out-String
Assert ($LASTEXITCODE -ne 0) 'a pin-first row that is not M2 is refused' $pinFirstNotM2
Assert ($pinFirstNotM2 -match 'pin-first row needs verdict=M2') 'the message names the rule' $pinFirstNotM2
Assert (@(Get-Content $data -Encoding UTF8 | Where-Object { $_ -match 'size=999' }).Count -eq 0) 'the refused pin-first row was not written' (Get-Content $data -Encoding UTF8) -join ' | '
Assert (@(Get-Content $data -Encoding UTF8 | Where-Object { $_ -match 'size=888' }).Count -eq 0) 'the refused row was not written' (Get-Content $data -Encoding UTF8) -join ' | '

if ($script:failures -eq 0) {
    Write-Host '[fg-probe] the model table script passes'
    exit 0
}
Write-Host "[fg-probe] $script:failures check(s) failed"
exit 1
