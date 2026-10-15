# Runner (tools\test.ps1 `Runner`): PreSR frame metadata for padded guides, with a negative control that drops the two
# assignments the original caller omitted.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$source = Read-Source $Repo $script:NrDeferredSr
$start = $source.IndexOf('    frame.RenderSubrectWidth = g.w;')
if ($start -lt 0) { throw 'Production PreSR frame metadata moved.' }
$end = $source.IndexOf('    frame.ColourIsLinearHdr =', $start)
if ($end -lt 0) { throw 'Production PreSR metadata terminator moved.' }
$fragment = $source.Substring($start, $end - $start)
$production = "DlssNrFrameInfo BuildProduction(unsigned w,unsigned h,unsigned outW,unsigned outH,unsigned flags,unsigned guideSourceWidth=0,unsigned guideSourceHeight=0) {`n" +
    "struct { unsigned w,h,outW,outH; } g{w,h,outW,outH}; DlssNrFrameInfo frame {};`n" + $fragment + "return frame; }`n"
$include = Join-Path $Out 'nr_presr_guides_production.inl'
function Build-Test([string]$name) {
    Invoke-Cl 'PreSR guide regression' @('/nologo', '/std:c++20', '/EHsc', "/I$Repo/OptiScaler", "/I$Out", "/Fo$Out/$name.obj", "/Fe$Out/$name.exe", "$PSScriptRoot/nr_presr_guide_metadata.cpp")
}
[IO.File]::WriteAllText($include, $production)
Build-Test 'current'
& "$Out/current.exe"
if ($LASTEXITCODE -ne 0) { throw 'PreSR guide regression failed.' }
# Negative control: the actual old caller omitted these metadata assignments.
$legacy = $production.Replace('    frame.MotionVectorsLowResolution = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;', '').Replace('    frame.OutputWidth = g.outW; frame.OutputHeight = g.outH;', '')
if ($legacy -ceq $production) { throw 'Negative control no longer matches production.' }
[IO.File]::WriteAllText($include, $legacy)
Build-Test 'legacy'
& "$Out/legacy.exe"
if ($LASTEXITCODE -ne 1) { throw 'Negative control did not detect the original padded-MV bug.' }
Write-Output 'Negative control reproduced the original PreSR padded-motion metadata error.'
