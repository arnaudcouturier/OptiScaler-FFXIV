# Runner (tools\test.ps1 `Runner`): PreSR pan/cadence logic, four fragments of the production DeferredSr code.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$source = Read-Source $Repo $script:NrDeferredSr
$record = Read-Fragment $source '    const bool testMotion =' '    Use use(g, cmd);'
$arm = Read-Fragment $source '            p->Set(NVSDK_NGX_Parameter_Reset,' '            p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,'
$after = Read-Fragment $source '    if (evaluateSr)' '    if (g.reset)' ($source.IndexOf('void After('))
$fg = Read-Fragment $source '(split ? !h.split.schedule.HasEdit() : !h.havePrevious) || UInt(g.parameters,' ', h.anchorId++);'
$production = "void RecordProduction(Generation& g,bool privateJob,unsigned flags,bool wantsHalf,unsigned long long motionNow) {`n" +
    $record + "}`nvoid ArmProduction(Generation& g,Frame frame) { auto* p=g.parameters; bool panReset=false;`n" +
    $arm + "}`nvoid EvaluateProduction(Generation& g,Pair pair) { bool evaluateSr=!pair.skipNr; void* cmd=nullptr; struct { unsigned slot=0; } use;`n" +
    $after + "}`nbool ResetFgProduction(Generation& g) { bool split=false; auto& h=*g.half; return " + $fg + "; }`n"
[IO.File]::WriteAllText((Join-Path $Out 'nr_presr_pan_production.inl'), $production)
Invoke-Cl 'PreSR pan regression' @('/nologo', '/std:c++20', '/EHsc', '/UNDEBUG', "/I$Repo/OptiScaler", "/I$Out", "/Fo$Out/pan.obj", "/Fe$Out/pan.exe", "$PSScriptRoot/nr_presr_pan_cadence.cpp")
& "$Out/pan.exe"
if ($LASTEXITCODE -ne 0) { throw 'PreSR pan regression failed.' }
