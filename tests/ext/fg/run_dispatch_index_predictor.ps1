# Runner (tools\test.ps1 `Runner`): slices the production GetIndexWillBeDispatched and GetDispatchIndex out of
# framegen\IFGFeature.cpp, compiles them against a fake IFGFeature and checks that both name the same slot. A negative
# control restores the old predictor (Depth only) and must be rejected.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../nr/slice_common.ps1"
$source = Read-Source $Repo 'OptiScaler\framegen\IFGFeature.cpp'
$predictor = Read-Function $source 'int IFGFeature::GetIndexWillBeDispatched()'
$consumer = Read-Function $source 'int IFGFeature::GetDispatchIndex(UINT64& willDispatchFrame)'
$include = Join-Path $Out 'dispatch_index_production.inl'
function Build-Test([string]$name) {
    Invoke-Cl 'dispatch index test' @('/nologo', '/std:c++20', '/EHsc', '/W4', "/I$Out", "/Fo$Out/$name.obj", "/Fe$Out/$name.exe", "$PSScriptRoot/dispatch_index_predictor.cpp")
}
[IO.File]::WriteAllText($include, $predictor + "`n" + $consumer)
Build-Test 'test'
& "$Out/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Dispatch index predictor does not mirror the consumer' }
# Negative control: the predictor as it was before the fix (Depth only).
$old = [regex]::Replace($predictor, 'if \(HasResource\(FG_ResourceType::Depth\)[^{]*\{', "if (HasResource(FG_ResourceType::Depth))`n        {")
if ($old -ceq $predictor) { throw 'Predictor negative control no longer matches production.' }
[IO.File]::WriteAllText($include, $old + "`n" + $consumer)
Build-Test 'legacy'
& "$Out/legacy.exe" | Out-Null   # prints the mismatches it is supposed to find
if ($LASTEXITCODE -ne 1) { throw 'The old Depth-only predictor was not rejected' }
Write-Output 'PASS: negative control detects the Depth-only predictor'
