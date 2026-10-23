# Runner (tools\test.ps1 `Runner`): negative controls for the PE privacy scan of tools\package-release.ps1, on synthetic package folders
# made under -Out (never the real builds\ or dist\ folders). The scan looks at bytes only, so the "binaries" are plain filler files.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$script = Join-Path $Repo 'tools\package-release.ps1'
$version = 'ffxiv-2026.10.23-d9b62110'
$failures = 0
function Check([bool]$ok, [string]$what, [string]$detail = '') {
    if (-not $ok) { Write-Host "FAIL: $what $detail"; $script:failures++ }
}

# (1) the forwarder keeps only the pdb file name in its debug directory
$vcx = Get-Content -LiteralPath (Join-Path $Repo 'OptiScaler\ext\nr\forwarder\dlssnr_forwarder.vcxproj') -Raw
$release = [regex]::Match($vcx, '(?s)Condition="[^"]*Configuration\)''==''Release''">.*?</ItemDefinitionGroup>').Value
Check ($release -match '(?s)<Link>.*<AdditionalOptions>[^<]*/PDBALTPATH:%_PDB%') 'forwarder Release <Link> has /PDBALTPATH:%_PDB%'

# (2) synthetic package
function New-Filler([string]$path, [string]$seed) {
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    [IO.File]::WriteAllBytes($path, [Text.Encoding]::ASCII.GetBytes("MZ filler $seed " + ('x' * 4000)))
}
function Add-Text([string]$path, [string]$text, [string]$encoding) {
    $bytes = if ($encoding -eq 'utf16') { [Text.Encoding]::Unicode.GetBytes($text) } else { [Text.Encoding]::ASCII.GetBytes($text) }
    $fs = [IO.File]::Open($path, 'Append'); $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
}
$caseNo = 0
function New-Package {
    $script:caseNo++
    $dir = Join-Path $Out ("pkg$script:caseNo")
    New-Filler "$dir\OptiScaler.dll" 'core'
    New-Filler "$dir\nvngx.dll_dlssnr.dll" 'forwarder'
    New-Filler "$dir\OptiScaler\D3D12_OptiScaler\D3D12Core.dll" 'd3d12core'
    $sections = Get-ChildItem (Join-Path $Repo 'OptiScaler\ext\ini') -Filter *.ini -File | ForEach-Object { "[$([IO.Path]::GetFileNameWithoutExtension($_.Name))]" }
    Set-Content -LiteralPath "$dir\OptiScaler.ini" -Value ($sections -join "`n") -Encoding ascii
    foreach ($n in 'DirectX_LICENSE.txt', 'RenoDX_ATTRIBUTION.txt', 'RenoDX_FFXIV_NOTICE.md', 'RenoDX_LICENSE.txt', 'ReShade_ATTRIBUTION.txt') {
        New-Item -ItemType Directory -Force -Path "$dir\Licenses" | Out-Null
        Set-Content -LiteralPath "$dir\Licenses\$n" -Value "licence text $n" -Encoding ascii
    }
    return $dir
}
function Invoke-Release([string]$pkg) {
    $outDir = Join-Path $Out ("dist" + $script:caseNo)
    $text = & pwsh -NoProfile -File $script -Package $pkg -Version $version -OutDir $outDir 2>&1 | Out-String
    $zip = Join-Path $outDir "OptiScaler-FFXIV-$version.zip"
    $code = $LASTEXITCODE
    $entries = @(); $install = ''
    if (Test-Path $zip) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $z = [IO.Compression.ZipFile]::OpenRead($zip); $entries = @($z.Entries | ForEach-Object { $_.FullName })
        $reader = [IO.StreamReader]::new($z.GetEntry('INSTALL.txt').Open()); $install = $reader.ReadToEnd(); $reader.Dispose(); $z.Dispose()
    }
    return [pscustomobject]@{ Code = $code; Text = $text; Zip = (Test-Path $zip); Entries = $entries; Install = $install }
}

# clean package: a zip, exit 0, no privacy complaint. Paths that merely look similar must not trip the scan.
$p = New-Package
Add-Text "$p\OptiScaler.dll" 'C:\Windows\System32\kernel32.dll \Users\ no drive Users\x D:\Program Files\Users' 'ascii'
Add-Text "$p\OptiScaler.dll" 'C' 'ascii'   # a one-byte letter in front of wide text is not a wide drive letter
Add-Text "$p\OptiScaler.dll" ':\Users\x' 'utf16'
$r = Invoke-Release $p
Check ($r.Code -eq 0 -and $r.Zip) 'clean package packs' $r.Text
Check ($r.Entries -contains 'winmm.dll' -and $r.Entries -contains 'nvngx.dll_dlssnr.dll') 'clean package has the fork binaries'

# INSTALL.txt: the uninstall step names what the archive installed (not the whole OptiScaler folder, which holds the user's own files),
# and the log advice mentions the game path
$uninstall = $r.Install.Substring($r.Install.IndexOf("`nUNINSTALL"))
foreach ($name in 'winmm.dll', 'nvngx.dll_dlssnr.dll', 'OptiScaler.ini', 'SHA256SUMS.txt', 'OptiScaler\D3D12_OptiScaler\D3D12Core.dll') {
    Check $uninstall.Contains($name) "uninstall text names $name"
}
Check ($uninstall -match 'Do NOT delete the OptiScaler folder') 'uninstall text says not to delete the OptiScaler folder'
Check ($uninstall -notmatch 'the OptiScaler folder, Licenses') 'uninstall text no longer deletes the whole OptiScaler folder'
foreach ($mine in 'nvngx_dlssg.dll', 'streamline', 'NR.<version>', 'dlssg_sm86') { Check $uninstall.Contains($mine) "uninstall text keeps $mine" }
Check ($r.Install -match 'LogToFile=true[\s\S]{0,300}game folder path[\s\S]{0,200}Windows user name') 'log advice mentions the game folder path and the user name'

# the forwarder with its pdb path (ASCII), exit 5, no zip, the message names the file and the string
$p = New-Package
Add-Text "$p\nvngx.dll_dlssnr.dll" 'C:\Users\x\a.pdb' 'ascii'
$r = Invoke-Release $p
Check ($r.Code -eq 5) 'ASCII user path in the forwarder gives exit 5' "code $($r.Code)"
Check (-not $r.Zip) 'no zip when the scan fails'
Check ($r.Text -match 'nvngx\.dll_dlssnr\.dll' -and $r.Text.Contains('C:\Users\x\a.pdb')) 'message names the file and the string' $r.Text

# the core dll (shipped as winmm.dll) with a UTF-16LE path and a lower-case drive letter and folder
$p = New-Package
Add-Text "$p\OptiScaler.dll" 'd:\users\bob\src\x.pdb' 'utf16'
$r = Invoke-Release $p
Check ($r.Code -eq 5 -and -not $r.Zip) 'UTF-16LE user path in winmm.dll gives exit 5' "code $($r.Code)"
Check ($r.Text -match 'winmm\.dll' -and $r.Text.Contains('d:\users\bob\src\x.pdb')) 'message names winmm.dll and the UTF-16 string' $r.Text

# a third-party library under OptiScaler\ with a vendor build path: warning, still packed
$p = New-Package
Add-Text "$p\OptiScaler\D3D12_OptiScaler\D3D12Core.dll" 'C:\Users\vendor\build\core.pdb' 'ascii'
$r = Invoke-Release $p
Check ($r.Code -eq 0 -and $r.Zip) 'third-party path only warns' "code $($r.Code)"
Check ($r.Text -match 'WARNING' -and $r.Text.Contains('C:\Users\vendor\build\core.pdb') -and $r.Text -match 'D3D12Core') 'warning names the library and the string' $r.Text

# F8: Licenses\ entries are classified too
$p = New-Package
New-Filler "$p\Licenses\nvngx_dlss.dll" 'nvidia'
$r = Invoke-Release $p
Check ($r.Code -eq 0 -and $r.Zip) 'blocked file in Licenses\ is left out, not fatal' "code $($r.Code)"
Check (-not ($r.Entries | Where-Object { $_ -match 'nvngx_dlss\.dll$' })) 'NVIDIA file in Licenses\ is not packed' ($r.Entries -join ',')
Check ($r.Text -match 'nvngx_dlss\.dll') 'the report lists it as not packed' $r.Text
$p = New-Package
New-Filler "$p\Licenses\tool.dll" 'tool'
Add-Text "$p\Licenses\tool.dll" 'C:\Users\x\tool.pdb' 'ascii'
$r = Invoke-Release $p
Check ($r.Code -eq 5 -and -not $r.Zip) 'a PE in Licenses\ is scanned too' "code $($r.Code)"

if ($failures) { Write-Host "package_release_scan: $failures check(s) failed"; exit 1 }
Write-Host 'package_release_scan: privacy scan, third-party warning, Licenses classification passed'
exit 0
