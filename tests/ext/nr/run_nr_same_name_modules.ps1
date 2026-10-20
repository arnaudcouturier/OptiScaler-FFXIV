# Runner (tools\test.ps1 `Runner`): builds two stand-in DLLs with the same file name in two folders and the probe that loads both.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/slice_common.ps1"
$a = Join-Path $Out 'A'; $b = Join-Path $Out 'B'
New-Item -ItemType Directory -Force $a, $b | Out-Null
Invoke-Cl 'DLL A' @('/nologo', '/LD', '/DPROBE_ID=1', "/Fo$a\", "/Fe$a\nvngx_dlssnr.dll", "$PSScriptRoot/nr_same_name_dll.cpp")
Invoke-Cl 'DLL B' @('/nologo', '/LD', '/DPROBE_ID=2', "/Fo$b\", "/Fe$b\nvngx_dlssnr.dll", "$PSScriptRoot/nr_same_name_dll.cpp")
Invoke-Cl 'probe' @('/nologo', '/std:c++20', '/EHsc', '/W4', "/Fo$Out\", "/Fe$Out\probe.exe", "$PSScriptRoot/nr_same_name_modules.cpp")
& "$Out\probe.exe" "$a\nvngx_dlssnr.dll" "$b\nvngx_dlssnr.dll"
if ($LASTEXITCODE -ne 0) { throw 'same-name modules test failed' }
