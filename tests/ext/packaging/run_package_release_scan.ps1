# Runner (tools\test.ps1 `Runner`): negative controls for the PE privacy scan of tools\package-release.ps1, on synthetic package folders
# made under -Out (never the real builds\ or dist\ folders). The scan looks at bytes only, so the "binaries" are plain filler files.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$script = Join-Path $Repo 'tools\package-release.ps1'
$version = 'ffxiv-2026.10.23-d9b62110'
$rc = 'ffxiv-2026.10.23-d9b62110-rc1'
# hermetic tag-check root: a scratch git repo under {out} with the release tag and the rc tag on two commits, so the
# tag-commit check runs without touching the real checkout. Every Invoke-Release below points -RepoRoot at it.
$tagRepo = Join-Path $Out 'tagroot'
Remove-Item -LiteralPath $tagRepo -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $tagRepo | Out-Null
& git -C $tagRepo -c init.defaultBranch=main init -q
& git -C $tagRepo config user.email 'test@localhost'
& git -C $tagRepo config user.name 'test'
Set-Content -LiteralPath (Join-Path $tagRepo 'f.txt') -Value 'one' -Encoding ascii
& git -C $tagRepo add f.txt
& git -C $tagRepo commit -qm 'first'
& git -C $tagRepo tag -a $version -m $version
Set-Content -LiteralPath (Join-Path $tagRepo 'f.txt') -Value 'two' -Encoding ascii
& git -C $tagRepo commit -qam 'second'
& git -C $tagRepo tag -a $rc -m $rc
$shaFinal = (& git -C $tagRepo rev-parse --verify "$version^{commit}" 2>$null | Select-Object -First 1)
$shaRc = (& git -C $tagRepo rev-parse --verify "$rc^{commit}" 2>$null | Select-Object -First 1)
if ($shaFinal -notmatch '^[0-9a-f]{40}$' -or $shaRc -notmatch '^[0-9a-f]{40}$') { Write-Host 'scratch tag repo setup failed'; exit 1 }
$long = "$version-0-g" + $shaFinal.Substring(0, 12)
$rcLong = "$rc-0-g" + $shaRc.Substring(0, 12)
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
# $embedded: the version string a build leaves in OptiScaler.dll, NUL-terminated on both sides like a C string literal (default: the
# long form of the tag of the run, so a package is what a tag-first build produces); '' = no version string at all
function New-Package([string]$embedded = $long) {
    $script:caseNo++
    $dir = Join-Path $Out ("pkg$script:caseNo")
    New-Filler "$dir\OptiScaler.dll" 'core'
    if ($embedded) { Add-Text "$dir\OptiScaler.dll" ("`0" + $embedded + "`0") 'ascii' }
    New-Filler "$dir\nvngx.dll_dlssnr.dll" 'forwarder'
    New-Filler "$dir\OptiScaler\D3D12_OptiScaler\D3D12Core.dll" 'd3d12core'
    $sections = Get-ChildItem (Join-Path $Repo 'OptiScaler\ext\ini') -Filter *.ini -File | ForEach-Object { "[$([IO.Path]::GetFileNameWithoutExtension($_.Name))]" }
    Set-Content -LiteralPath "$dir\OptiScaler.ini" -Value ($sections -join "`n") -Encoding ascii
    # the package script writes the effective settings schema next to the ini; the release script requires it
    Set-Content -LiteralPath "$dir\settings-schema.json" -Value '{ "format": "settings-schema-effective", "release": "", "sections": [] }' -Encoding ascii
    foreach ($n in 'DirectX_LICENSE.txt', 'RenoDX_ATTRIBUTION.txt', 'RenoDX_FFXIV_NOTICE.md', 'RenoDX_LICENSE.txt', 'ReShade_ATTRIBUTION.txt') {
        New-Item -ItemType Directory -Force -Path "$dir\Licenses" | Out-Null
        Set-Content -LiteralPath "$dir\Licenses\$n" -Value "licence text $n" -Encoding ascii
    }
    return $dir
}
function Invoke-Release([string]$pkg, [string[]]$extra = @(), [string]$ver = $version) {
    $outDir = Join-Path $Out ("dist" + $script:caseNo)
    $text = & pwsh -NoProfile -File $script -Package $pkg -Version $ver -OutDir $outDir -RepoRoot $tagRepo @extra 2>&1 | Out-String
    $zip = Join-Path $outDir "OptiScaler-FFXIV-$ver.zip"
    $code = $LASTEXITCODE
    $entries = @(); $install = ''
    if (Test-Path $zip) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $z = [IO.Compression.ZipFile]::OpenRead($zip); $entries = @($z.Entries | ForEach-Object { $_.FullName })
        $reader = [IO.StreamReader]::new($z.GetEntry('INSTALL.txt').Open()); $install = $reader.ReadToEnd(); $reader.Dispose(); $z.Dispose()
    }
    return [pscustomobject]@{ Code = $code; Text = $text; Zip = (Test-Path $zip); Entries = $entries; Install = $install; OutDirExists = (Test-Path $outDir) }
}

# clean package: a zip, exit 0, no privacy complaint. Paths that merely look similar must not trip the scan.
$p = New-Package
Add-Text "$p\OptiScaler.dll" 'C:\Windows\System32\kernel32.dll \Users\ no drive Users\x D:\Program Files\Users' 'ascii'
Add-Text "$p\OptiScaler.dll" 'C' 'ascii'   # a one-byte letter in front of wide text is not a wide drive letter
Add-Text "$p\OptiScaler.dll" ':\Users\x' 'utf16'
Add-Text "$p\OptiScaler.dll" 'C:/Usersx/y C:\\Userz\\x C:/Windows/Users C:/Users' 'ascii'   # not a Users folder
Add-Text "$p\OptiScaler.dll" 'C:\\Usersx\\y' 'utf16'
# Licenses\ accepts .txt, .md (any case) and names without an extension
Set-Content -LiteralPath "$p\Licenses\REST_LICENSE" -Value 'licence text' -Encoding ascii
Set-Content -LiteralPath "$p\Licenses\Extra_NOTICE.TXT" -Value 'licence text' -Encoding ascii
Set-Content -LiteralPath "$p\Licenses\Extra_NOTICE.Md" -Value 'licence text' -Encoding ascii
$r = Invoke-Release $p
Check ($r.Code -eq 0 -and $r.Zip) 'clean package packs' $r.Text
Check (($r.Entries -contains 'Licenses/REST_LICENSE') -and ($r.Entries -contains 'Licenses/Extra_NOTICE.TXT') -and ($r.Entries -contains 'Licenses/Extra_NOTICE.Md')) 'Licenses: .txt, .md and extensionless names are packed' ($r.Entries -join ',')
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
Check ($r.Install -match 'OptiScaler\.log[\s\S]{0,300}file paths and\s+hashes of the NVIDIA runtimes[\s\S]{0,200}Windows user name') 'log advice names OptiScaler.log and says it holds the runtime paths and hashes'
Check ($uninstall -match 'OptiScaler\.log') 'uninstall text mentions OptiScaler.log'
# the update check: the fork has none, the upstream key still works and asks upstream's project
Check ($r.Install -match 'Updates:\s+this fork does not check for new versions; its releases are at ') 'INSTALL.txt says the fork does not check for updates'
Check ($r.Install -match "CheckForUpdate=true turns on upstream OptiScaler's check, which asks upstream's project, not this one") 'INSTALL.txt says what CheckForUpdate=true does'
Check ($uninstall -notmatch '\.pdb') 'no symbol names in the uninstall text without -IncludeSymbols'

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

# Licenses\ takes only .txt, .md and names without an extension: a binary there stops the script (exit 3), naming the file
$p = New-Package
New-Filler "$p\Licenses\tool.dll" 'tool'
$r = Invoke-Release $p
Check ($r.Code -eq 3 -and -not $r.Zip) 'a .dll in Licenses\ gives exit 3 and no zip' "code $($r.Code)"
Check ($r.Text -match 'Licenses\\tool\.dll') 'the message names the file in Licenses\' $r.Text
$p = New-Package
New-Filler "$p\Licenses\sub\helper.exe" 'tool'
$r = Invoke-Release $p
Check ($r.Code -eq 3 -and -not $r.Zip) 'an .exe in a Licenses\ subfolder gives exit 3' "code $($r.Code)"

# a forward-slash path (ASCII) in the forwarder
$p = New-Package
Add-Text "$p\nvngx.dll_dlssnr.dll" 'C:/Users/x/a.pdb' 'ascii'
$r = Invoke-Release $p
Check ($r.Code -eq 5 -and -not $r.Zip) 'forward-slash user path in the forwarder gives exit 5' "code $($r.Code)"
Check ($r.Text -match 'nvngx\.dll_dlssnr\.dll' -and $r.Text.Contains('C:/Users/x/a.pdb')) 'message names the file and the forward-slash string' $r.Text

# an ASCII path with a lower-case drive and upper-case folder name
$p = New-Package
Add-Text "$p\OptiScaler.dll" 'e:\USERS\bob\x.pdb' 'ascii'
$r = Invoke-Release $p
Check ($r.Code -eq 5 -and -not $r.Zip) 'case-insensitive ASCII user path in winmm.dll gives exit 5' "code $($r.Code)"

# a doubled-backslash path (C-escaped), ASCII, in winmm.dll
$p = New-Package
Add-Text "$p\OptiScaler.dll" 'C:\\Users\\bob\\x.pdb' 'ascii'
$r = Invoke-Release $p
Check ($r.Code -eq 5 -and -not $r.Zip) 'doubled-backslash ASCII user path in winmm.dll gives exit 5' "code $($r.Code)"

# a doubled-backslash path (C-escaped), UTF-16LE, in winmm.dll
$p = New-Package
Add-Text "$p\OptiScaler.dll" 'C:\\Users\\x\\a.pdb' 'utf16'
$r = Invoke-Release $p
Check ($r.Code -eq 5 -and -not $r.Zip) 'doubled-backslash UTF-16LE user path in winmm.dll gives exit 5' "code $($r.Code)"
Check ($r.Text -match 'winmm\.dll' -and $r.Text.Contains('C:\\Users\\x\\a.pdb')) 'message names winmm.dll and the doubled-backslash string' $r.Text

# a UTF-16LE hit in a third-party library under OptiScaler\: warning, still packed
$p = New-Package
Add-Text "$p\OptiScaler\D3D12_OptiScaler\D3D12Core.dll" 'C:\Users\vendor\build\core.pdb' 'utf16'
$r = Invoke-Release $p
Check ($r.Code -eq 0 -and $r.Zip) 'third-party UTF-16LE path only warns' "code $($r.Code)"
Check ($r.Text -match 'WARNING' -and $r.Text.Contains('C:\Users\vendor\build\core.pdb') -and $r.Text -match 'D3D12Core') 'warning names the library and the wide string' $r.Text

# -IncludeSymbols: the packed .pdb names are in the uninstall text (generated from the include list)
$p = New-Package
New-Filler "$p\OptiScaler.pdb" 'pdb'
New-Filler "$p\nvngx.dll_dlssnr.pdb" 'pdb'
$r = Invoke-Release $p @('-IncludeSymbols')
Check ($r.Code -eq 0 -and $r.Zip) '-IncludeSymbols package packs' $r.Text
Check (($r.Entries -contains 'OptiScaler.pdb') -and ($r.Entries -contains 'nvngx.dll_dlssnr.pdb')) '-IncludeSymbols packs the pdb files' ($r.Entries -join ',')
$uninstall = $r.Install.Substring($r.Install.IndexOf("`nUNINSTALL"))
Check ($uninstall.Contains('OptiScaler.pdb') -and $uninstall.Contains('nvngx.dll_dlssnr.pdb')) 'uninstall text names the packed .pdb files'

# ---------------------------------------------------------------- tag-first: the DLL has to carry the tag's name and tree (exit 6)
# The build embeds `git describe --long --dirty` of the commit it was built from as a NUL-terminated string in OptiScaler.dll (the future winmm.dll).
# On the tagged commit that reads <tag>-0-g<sha>; built later it reads <tag>-<n>-g<sha> with n > 0, from a dirty tree with -dirty
# appended. The release script refuses anything but <tag>-0-g<sha> for the tag of -Version, and the tag's commit has to start with
# <sha> (checked with -RepoRoot against the scratch repo above).
function Check-Refused($r, [string]$what) {
    Check ($r.Code -eq 6) "$what gives exit 6" "code $($r.Code): $($r.Text)"
    Check (-not $r.Zip) "$what writes no zip"
    Check (-not $r.OutDirExists) "$what leaves no output folder behind" ($r.Text)
}

# the string equals -Version: packs, and the script prints what it found
$p = New-Package
$r = Invoke-Release $p
Check ($r.Code -eq 0 -and $r.Zip) 'embedded string equals -Version: packs' $r.Text
Check ($r.Text -match ('embedded version string[^\r\n]*: ' + [regex]::Escape($long) + '\r?\n')) 'the embedded string is printed when it matches' $r.Text
# the -Version argument may leave the ffxiv- prefix out
$p = New-Package
$r = Invoke-Release $p @() '2026.10.23-d9b62110'
Check ($r.Code -eq 0 -and $r.Zip) 'a -Version without the ffxiv- prefix names the same tag' $r.Text

# built after the tag: the describe string carries the commit count and the sha
$late = 'ffxiv-2026.10.23-d9b62110-2-g171bdb06'
$p = New-Package $late
$r = Invoke-Release $p
Check-Refused $r 'a DLL built two commits after the tag'
Check ($r.Text.Contains($late) -and $r.Text.Contains($version) -and $r.Text -match 'commits after the tag') 'the message prints the embedded string and the tag, and says the DLL was built after the tag' $r.Text

# candidates: -rc1 is a different name from the final tag and from a later describe of itself
$rc = 'ffxiv-2026.10.23-d9b62110-rc1'
$p = New-Package "$rc-2-g171bdb06"
$r = Invoke-Release $p @() $rc
Check-Refused $r 'an rc DLL built after its tag'
Check ($r.Text.Contains("$rc-2-g171bdb06")) 'the message prints the rc describe string' $r.Text
$p = New-Package $rcLong
$r = Invoke-Release $p @() $rc
Check ($r.Code -eq 0 -and $r.Zip) 'an rc DLL built on its tag packs' $r.Text
$p = New-Package $rc
$r = Invoke-Release $p
Check-Refused $r 'an rc DLL packaged as the final tag'
$p = New-Package
$r = Invoke-Release $p @() $rc
Check-Refused $r 'a final-tag DLL packaged as the rc'

# another tag
$p = New-Package 'ffxiv-2026.10.22-d9b62110-0-g171bdb06'
$r = Invoke-Release $p
Check-Refused $r 'a DLL that carries another tag'

# no version string at all: a bare sha (git describe --always with no tag reachable), "unknown", or nothing
foreach ($s in 'f2a5723e', 'unknown', '') {
    $p = New-Package $s
    $r = Invoke-Release $p
    Check-Refused $r "a DLL with the version string '$s'"
    Check ($r.Text -match 'no long-form version string') "the message says there is no version string for '$s'" $r.Text
    Check ($r.Text -notmatch 'it carries') "the no-string refusal stops there, it does not fall through to the tag comparison for '$s'" $r.Text
}
# the tag inside a longer string is not the embedded version string (the build embeds it on its own, NUL-terminated)
$p = New-Package ''
Add-Text "$p\OptiScaler.dll" 'banner ffxiv-2026.10.23-d9b62110 on upstream' 'ascii'
$r = Invoke-Release $p
Check-Refused $r 'the tag inside a longer string'
# two different version strings: ambiguous, refused with both printed
$p = New-Package
Add-Text "$p\OptiScaler.dll" ("`0" + 'ffxiv-2026.10.22-d9b62110-0-g171bdb06' + "`0") 'ascii'
$r = Invoke-Release $p
Check-Refused $r 'two different embedded version strings'
Check ($r.Text.Contains('ffxiv-2026.10.22-d9b62110') -and $r.Text.Contains($version)) 'the message prints both strings' $r.Text
# the same string twice is one string
$p = New-Package
Add-Text "$p\OptiScaler.dll" ("`0" + $long + "`0") 'ascii'
$r = Invoke-Release $p
Check ($r.Code -eq 0 -and $r.Zip) 'the same version string twice still packs' $r.Text
# the refusal comes before anything is classified: a late DLL in a package that also holds an unclassified file is exit 6, not 3
$p = New-Package $late
New-Filler "$p\mystery.bin" 'unknown'
$r = Invoke-Release $p
Check-Refused $r 'a late DLL in a package that also holds an unclassified file'
# a dirty tree: the describe string ends in -dirty, refused before anything is written
$p = New-Package ($long + '-dirty')
$r = Invoke-Release $p
Check-Refused $r 'a DLL built from a dirty tree'
Check ($r.Text.Contains($long + '-dirty') -and $r.Text -match 'dirty tree') 'the message prints the dirty string and says the tree was dirty' $r.Text
# a moved tag: the string is well-formed with N=0, but the tag now points at another commit
$p = New-Package ($version + '-0-gdeadbee1')
$r = Invoke-Release $p
Check-Refused $r 'a DLL built before the tag moved'
Check ($r.Text.Contains($version + '-0-gdeadbee1') -and $r.Text -match 'moved since the build') 'the message prints the string and says the tag moved' $r.Text

# a refused run adds nothing to an output folder that already exists (no archive, no stage folder, no .gitignore)
$p = New-Package $late
$existing = Join-Path $Out ('dist' + $script:caseNo)
New-Item -ItemType Directory -Force -Path $existing | Out-Null
Set-Content -LiteralPath (Join-Path $existing 'keep.txt') -Value 'earlier file' -Encoding ascii
$r = Invoke-Release $p
Check ($r.Code -eq 6 -and -not $r.Zip) 'a refused run into an existing output folder gives exit 6 and no zip' $r.Text
$left = @(Get-ChildItem -LiteralPath $existing -Force | ForEach-Object Name)
Check (($left -join ',') -eq 'keep.txt') 'the existing output folder holds only what it held before' ($left -join ',')

# a malformed -Version is exit 2, whatever the DLL carries: the DLL is not read before the argument is valid
$p = New-Package $late
$r = Invoke-Release $p @() 'not-a-version'
Check ($r.Code -eq 2) 'a malformed -Version is still exit 2' "code $($r.Code)"
# the tag is case-sensitive everywhere (git, the embedded string): an upper-case -Version is a malformed argument, not a late "tag mismatch"
$r = Invoke-Release $p @() 'FFXIV-2026.10.25-d9b62110'
Check ($r.Code -eq 2) 'an upper-case ffxiv- prefix in -Version is exit 2' "code $($r.Code)"
$r = Invoke-Release $p @() 'ffxiv-2026.10.25-D9B62110'
Check ($r.Code -eq 2) 'an upper-case sha in -Version is exit 2' "code $($r.Code)"
# the argument check is anchored at both ends: text glued to a well-formed -Version is exit 2, not a late "tag mismatch"
$p = New-Package
foreach ($bad in ($version + '-x'), ($version + '0'), ('x' + $version)) {
    $r = Invoke-Release $p @() $bad
    Check ($r.Code -eq 2) "-Version '$bad' is exit 2" "code $($r.Code)"
}
# a missing file is still exit 2 and writes nothing: the file checks come before the DLL is read
$p = New-Package
Remove-Item -LiteralPath "$p\OptiScaler.dll"
$r = Invoke-Release $p
Check ($r.Code -eq 2 -and -not $r.OutDirExists) 'a package without OptiScaler.dll is exit 2 and writes nothing' "code $($r.Code): $($r.Text)"
# the version string has to stand on its own (a NUL on each side). The longer-string case above has plain text on both sides, so it cannot
# tell which half of the rule works: one case per side
$p = New-Package ''
Add-Text "$p\OptiScaler.dll" ('prefix ' + $long + "`0") 'ascii'    # NUL behind it, text in front
$r = Invoke-Release $p
Check-Refused $r 'the tag glued to text in front of it (NUL behind only)'
$p = New-Package ''
Add-Text "$p\OptiScaler.dll" ("`0" + $long + ' trailing') 'ascii'   # NUL in front, text behind
$r = Invoke-Release $p
Check-Refused $r 'the tag glued to text behind it (NUL in front only)'
# a string at offset 0 has no byte in front of it; the last byte of the file (a NUL here) must not stand in for it
$p = New-Package ''
[IO.File]::WriteAllBytes("$p\OptiScaler.dll", [Text.Encoding]::ASCII.GetBytes($long + "`0" + ('x' * 200) + "`0"))
$r = Invoke-Release $p
Check-Refused $r 'the tag at the very start of the file'

# INSTALL.txt after a game patch: it follows docs\FFXIV-PATCH-RUNBOOK.md (name- and shader-based features keep running; the features that
# read or hook game code stay off unless FfxivUnknownBuildHooks=true)
$p = New-Package
$r = Invoke-Release $p
Check ($r.Install -match 'keep running' -and $r.Install -match 'FfxivUnknownBuildHooks=true') 'INSTALL.txt: after a patch name-based features keep running, code-reading ones need FfxivUnknownBuildHooks=true'
Check ($r.Install -notmatch 'switch themselves off') 'INSTALL.txt no longer says every FFXIV feature switches itself off after a patch'

if ($failures) { Write-Host "package_release_scan: $failures check(s) failed"; exit 1 }
Write-Host 'package_release_scan: privacy scan (backslash, forward slash, doubled backslash, UTF-16LE), third-party warning, Licenses classification, tag-first long-form version string with hermetic -RepoRoot tag checks (exit 6: late, dirty, moved tag) passed'
exit 0
