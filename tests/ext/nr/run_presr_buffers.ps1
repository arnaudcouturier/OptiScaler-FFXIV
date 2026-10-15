# Runner (tools\test.ps1 `Runner`): PreSR capture / rotate / split-guide / rejection-reference buffers and timing on
# D3D12 WARP, running production fragments of DeferredSr. The debug layer is used when Windows Graphics Tools exist.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$source = Read-Source $Repo $script:NrDeferredSr
$capture = (Read-Fragment $source '    auto* cleanTarget = half ?' '    ID3D12Resource* base = cleanTarget;').Replace('pair.output', 'output')
$rotate = Read-Fragment $source '        std::swap(h.motion, h.previousMotion);' '        h.havePrevious = ok;'
$production = "ID3D12Resource* CaptureProduction(Generation& g, ID3D12GraphicsCommandList* cmd, ID3D12Resource* output, bool half, bool split=false) {`n" +
    $capture + "`nreturn cleanTarget;`n}`nvoid RotateProduction(HalfRate& h) {`n" + $rotate + "`n}`n"
$production += Read-Fragment $source 'bool SnapshotSplitGuides(' 'bool PrepareHalfRate('
$selection = Read-Fragment $source '    ID3D12Resource* rejectionReference = nullptr;' '    bool ok = true;'
$production += "`nID3D12Resource* SelectRejectionProduction(Generation& g,bool split,unsigned mode,ID3D12Resource* cleanTarget,float scale,float& exposure) { struct {unsigned Mode; float ReferencePreExposure=0;unsigned ResidualRejection=0;} apply{mode}; struct {float scale;} pair{scale}; const unsigned DlssNrMode_ApplyInterpolatedResidual=10;`n" + $selection + "`nexposure=apply.ReferencePreExposure;return rejectionReference;}`n"
[IO.File]::WriteAllText((Join-Path $Out 'presr-buffer-production.inl'), $production)
Invoke-Cl 'PreSR buffer test' @('/nologo', '/std:c++20', '/EHsc', '/UNDEBUG', '/DNOMINMAX', "/I$Repo/OptiScaler", "/I$Out", "/Fo$Out/presr_buffers.obj", "/Fe$Out/presr_buffers.exe", "$PSScriptRoot/presr_buffers.cpp", 'd3d12.lib', 'dxgi.lib')
& "$Out/presr_buffers.exe"
if ($LASTEXITCODE -ne 0) { throw 'PreSR buffer/timestamp regression failed.' }
