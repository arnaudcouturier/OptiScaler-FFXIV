# Runner (tools\test.ps1 `Runner`): slices struct HiddenHooks out of OptiScaler\ext\present\FramePacingTrace.cpp and drives it
# through the real hidden::SeamPresent against a fake swapchain (the test supplies the key and the log lines). A negative control
# makes the hook answer "the switch is off" whatever the key says: the test must reject it.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../nr/slice_common.ps1"
$source = Read-Source $Repo 'OptiScaler\ext\present\FramePacingTrace.cpp'
$hooks = Read-Function $source 'struct HiddenHooks' # through the closing brace of the struct
$keyRead = 'ext::Cfg().framePacing.hiddenPresentDoNotWait.value_or_default()'
$slice = Join-Path $Out 'hidden_present_hooks_slice.inl'

function Build-Test([string]$name) {
    Invoke-Cl 'hidden present hooks test' @('/nologo', '/std:c++20', '/EHsc', '/W4', '/DNOMINMAX', "/I$Out", "/I$Repo/OptiScaler", "/Fo$Out/$name.obj", "/Fe$Out/$name.exe", "$PSScriptRoot/hidden_present_hooks.cpp")
}

[IO.File]::WriteAllText($slice, $hooks + ';' + "`n")
Build-Test 'test'
& "$Out/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'The hidden-present hooks do not follow the switch: a present that should carry DO_NOT_WAIT did not, or one that should not did.' }

# Negative control: the hook never adds the flag, whatever the key says (the state before the switch existed). The key-on checks must
# fail; exit code 1 is the test's "some check failed".
$off = $hooks.Replace($keyRead, 'false')
if ($off -ceq $hooks) { throw 'Production HiddenHooks no longer reads the switch: the key-on path would go untested.' }
[IO.File]::WriteAllText($slice, $off + ';' + "`n")
Build-Test 'off'
& "$Out/off.exe" | Out-Null
if ($LASTEXITCODE -ne 1) { throw 'The key-on path is unchecked: a hook that always answers "off" passed.' }
Write-Output 'PASS: negative control detects a hook that never adds DO_NOT_WAIT'
