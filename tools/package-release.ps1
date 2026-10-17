<#
.SYNOPSIS
  Turns a folder made by tools\package.ps1 into the release archive dist\OptiScaler-FFXIV-<Version>.zip.
.DESCRIPTION
  Reads -Package (never writes to it), keeps only what this fork may redistribute and what the install needs, and writes:

    winmm.dll                    the build's OptiScaler.dll, installed under the name the game loads
    OptiScaler.ini               the shipped template with the fork's fragments merged in
    nvngx.dll_dlssnr.dll         the fork's NR forwarder (the odd name is deliberate, see docs\SEAMS.md)
    OptiScaler\...               the third-party libraries the build copies (Direct3D 12 Agility SDK core, FidelityFX, XeSS)
    Licenses\...                 the build's licence and notice files, plus the GPL text and docs\ATTRIBUTION.md
    INSTALL.txt, NOTICE.txt, SHA256SUMS.txt

  Rules, applied to every file found in -Package:
    * An allow-list decides what goes in. A file that is on neither the allow-list nor the block-list stops the script (exit 3):
      it refuses to ship what it cannot classify.
    * NVIDIA runtime and model files (nvngx_dlss*.dll, nvngx.dll, nvapi*.dll, Streamline sl.*, *.onnx models, the Ampere companion)
      are never packed, even when present. They are listed as "bring your own" in the report and in INSTALL.txt.
    * Build by-products (.pdb .exp .lib .ilk .obj) and upstream's setup scripts are left out; -IncludeSymbols adds the .pdb.
    * Every packed third-party binary must map to a licence file that is present under Licenses\ in -Package; a missing one stops
      the script. A binary whose licence file does not name it is packed with a warning that the maintainer has to confirm.
  After writing the archive the script extracts it to a scratch folder and checks every hash against SHA256SUMS.txt, and checks that
  -Package was not modified (its tree hash before and after).
.PARAMETER Package
  Folder produced by tools\package.ps1 (contains OptiScaler.dll, OptiScaler.ini, the forwarder).
.PARAMETER Version
  Release tag, for example ffxiv-2026.10.17-fd6c2870-rc1 (convention: ffxiv-<yyyy.mm.dd>-<upstream sha8>[-rcN]); the leading "ffxiv-" may be left out.
.PARAMETER Exclude
  File names or package-relative paths to leave out of the archive (for example a binary whose licence you could not confirm).
.PARAMETER OutDir
  Output folder, default <repo>\dist. A .gitignore containing "*" is written there so the archive can never be committed by accident.
.EXAMPLE
  pwsh -File tools\package.ps1 -Target C:\work\pkg ; pwsh -File tools\package-release.ps1 -Package C:\work\pkg -Version ffxiv-2026.10.17-fd6c2870-rc1
#>
#requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Version,
    [string]$OutDir,
    [switch]$IncludeSymbols,
    [string[]]$Exclude = @()
)

$ErrorActionPreference = 'Stop'
function Fail([string]$Message, [int]$Code) { Write-Host "[release] $Message" -ForegroundColor Red; exit $Code }
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }

if ($Version -notmatch '^(ffxiv-)?\d{4}\.\d{2}\.\d{2}-[0-9a-f]{8}(-rc\d+)?$') {
    Fail "Version '$Version' does not follow ffxiv-<yyyy.mm.dd>-<upstream sha8>[-rcN]" 2
}
if (-not (Test-Path -LiteralPath $Package -PathType Container)) { Fail "Package folder not found: $Package" 2 }
$Package = (Resolve-Path -LiteralPath $Package).Path
foreach ($need in 'OptiScaler.dll', 'OptiScaler.ini', 'nvngx.dll_dlssnr.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $Package $need) -PathType Leaf)) { Fail "$need is missing in $Package (run tools\package.ps1 first)" 2 }
}

# ---------------------------------------------------------------- classification tables
# Never packed: not redistributable from this repository (NVIDIA runtime/model files, Streamline, community companions).
$blockPatterns = @(
    '^nvngx(_dlss[a-z]*)?\.dll$', '^nvapi(64)?\.dll$', '^sl\..+', '^dlssg_sm86', '^nvngx_dlssnr',
    '\.(onnx|plan|engine|trt|safetensors)$', '^dlss(g|d|nr)?[_\.]'
)
# Build by-products and upstream installer files: left out of the archive (not a licensing question).
$skipPatterns = @('\.(pdb|exp|lib|ilk|obj|ipdb|iobj)$', '^setup_windows\.bat$', '^setup_linux\.sh$', '^!! EXTRACT ALL FILES TO GAME FOLDER !!$')
# Third-party binaries the build copies, and the licence file under Licenses\ that has to accompany each one.
$thirdParty = @(
    @{ Pattern = '^D3D12Core\.dll$';                   Licence = 'DirectX_LICENSE.txt';       Names = $true  }
    @{ Pattern = '^amd_fidelityfx_[a-z0-9_]+\.dll$';   Licence = 'FidelityFX_v2_LICENSE.md';  Names = $true  }
    @{ Pattern = '^libxess(_dx11|_fg)?\.dll$';         Licence = 'XeSS_LICENSE.txt';          Names = $true  }
    @{ Pattern = '^libxell\.dll$';                     Licence = 'XeSS_LICENSE.txt';          Names = $false }
)
# Notice files that must be present in the package's Licenses\ (RenoDX notice, ReShade header attribution).
$requiredNotices = @('RenoDX_ATTRIBUTION.txt', 'RenoDX_FFXIV_NOTICE.md', 'RenoDX_LICENSE.txt', 'ReShade_ATTRIBUTION.txt')

function Get-TreeHash([string]$dir) {
    $lines = Get-ChildItem -LiteralPath $dir -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
        '{0} {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.FullName.Substring($dir.Length)
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    return [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($bytes)).Replace('-', '')
}

$treeBefore = Get-TreeHash $Package

# ---------------------------------------------------------------- classify
$include = [System.Collections.Generic.List[object]]::new()   # @{ Source; Dest }
$blocked = [System.Collections.Generic.List[string]]::new()
$skipped = [System.Collections.Generic.List[string]]::new()
$unknown = [System.Collections.Generic.List[string]]::new()
$warnings = [System.Collections.Generic.List[string]]::new()
$licensesDir = Join-Path $Package 'Licenses'

foreach ($f in (Get-ChildItem -LiteralPath $Package -Recurse -File -Force | Sort-Object FullName)) {
    $rel = $f.FullName.Substring($Package.Length).TrimStart('\', '/')
    $name = $f.Name
    $top = ($rel -split '[\\/]')[0]

    if (($Exclude -contains $name) -or ($Exclude -contains $rel)) { $skipped.Add("$rel (excluded by request)"); continue }
    if ($top -eq 'Licenses') { $include.Add(@{ Source = $f.FullName; Dest = $rel }); continue }
    if ($blockPatterns | Where-Object { $name -match $_ }) { $blocked.Add($rel); continue }
    if ($skipPatterns | Where-Object { $name -match $_ }) {
        if ($IncludeSymbols -and $name -match '\.pdb$') { $include.Add(@{ Source = $f.FullName; Dest = $rel }) } else { $skipped.Add($rel) }
        continue
    }
    if ($rel -ceq 'OptiScaler.dll') { $include.Add(@{ Source = $f.FullName; Dest = 'winmm.dll' }); continue }
    if ($rel -ceq 'OptiScaler.ini' -or $rel -ceq 'nvngx.dll_dlssnr.dll') { $include.Add(@{ Source = $f.FullName; Dest = $rel }); continue }

    $tp = $thirdParty | Where-Object { $name -match $_.Pattern } | Select-Object -First 1
    if ($tp -and $top -eq 'OptiScaler') {
        if (-not (Test-Path -LiteralPath (Join-Path $licensesDir $tp.Licence))) {
            Fail "$rel needs Licenses\$($tp.Licence), which is missing in the package" 3
        }
        if (-not $tp.Names) {
            $warnings.Add("$rel : $($tp.Licence) does not name this file; it is packed on the assumption that the same Intel licence covers it. The maintainer must confirm it, or leave the file out with -Exclude $name.")
        }
        $include.Add(@{ Source = $f.FullName; Dest = $rel }); continue
    }
    $unknown.Add($rel)
}

if ($unknown.Count -gt 0) {
    Write-Host '[release] files that are neither allowed nor blocked (classify them in tools\package-release.ps1):' -ForegroundColor Red
    $unknown | ForEach-Object { Write-Host "  $_" }
    exit 3
}
foreach ($n in $requiredNotices) {
    if (-not (Test-Path -LiteralPath (Join-Path $licensesDir $n))) { Fail "Licenses\$n is missing in the package (required notice)" 3 }
}
$gpl = Join-Path $root 'LICENSE'
$attrib = Join-Path $root 'docs\ATTRIBUTION.md'
foreach ($p in $gpl, $attrib) { if (-not (Test-Path -LiteralPath $p)) { Fail "$p not found; run the script from a checkout of the fork" 2 } }

# the shipped ini must contain every fork fragment's section (proof that tools\package.ps1 ran)
$ini = Get-Content -LiteralPath (Join-Path $Package 'OptiScaler.ini') -Raw
$fragDir = Join-Path $root 'OptiScaler\ext\ini'
if (Test-Path $fragDir) {
    foreach ($frag in (Get-ChildItem $fragDir -Filter *.ini -File)) {
        $section = [IO.Path]::GetFileNameWithoutExtension($frag.Name)
        if ($ini -notmatch "(?im)^\s*\[$([regex]::Escape($section))\]\s*$") { Fail "OptiScaler.ini has no [$section] section: the package was not made by tools\package.ps1" 3 }
    }
}

# ---------------------------------------------------------------- source URL (from the checkout, nothing hard-coded)
$srcUrl = ''
try {
    $u = (git -C $root remote get-url origin 2>$null | Select-Object -First 1)
    if ($u) { $srcUrl = ($u -replace '^https://[^@/]+@', 'https://' -replace '\.git$', '').Trim() }
} catch { }
if (-not $srcUrl) { $srcUrl = '(the repository this archive was built from)' }
$tag = if ($Version.StartsWith('ffxiv-')) { $Version } else { "ffxiv-$Version" }

# ---------------------------------------------------------------- stage
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Set-Content -LiteralPath (Join-Path $OutDir '.gitignore') -Value '*' -Encoding ascii
$stage = Join-Path $OutDir ("stage-" + $Version)
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($i in $include) {
    $dest = Join-Path $stage $i.Dest
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Copy-Item -LiteralPath $i.Source -Destination $dest
}
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Licenses') | Out-Null
Copy-Item -LiteralPath $gpl -Destination (Join-Path $stage 'Licenses\OptiScaler-FFXIV_LICENSE_GPL-3.0.txt')
Copy-Item -LiteralPath $attrib -Destination (Join-Path $stage 'Licenses\ATTRIBUTION.md')

$byo = @(
    'nvngx_dlss.dll          NVIDIA DLSS super resolution runtime. The game normally ships its own copy next to the executable; OptiScaler uses it.'
    'nvngx_dlssnr.dll        NVIDIA DLSS Neural Rendering model, needed for [DlssNr] Enabled. Obtain it from NVIDIA (the DLSS SDK or the NVIDIA app/driver distribution). The ini comment in [DlssNr] names the version and, for RTX 20/30/40, the community compatibility runtime it expects.'
    'nvngx_dlssg.dll         NVIDIA DLSS frame generation runtime, needed for DLSS-G output.'
    'sl.interposer.dll, sl.common.dll, sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll   NVIDIA Streamline plugins, placed in OptiScaler\streamline\, needed for DLSS-G output. Use the versions that match nvngx_dlssg.dll.'
    'OptiScaler\dlssg_sm86\  Community companion DLSS-G runtime, only for the experimental RTX 30 path ([DLSSG] AmpereMfgUnlock).'
    'ReShade 6.8.0           Your own ReShade install, only if you want presets applied to the HUD-less image ([HudLess] ReShadePresets).'
)

$install = @"
OptiScaler-FFXIV $tag
=====================================================================

Unofficial OptiScaler fork for FINAL FANTASY XIV (DirectX 11). Not affiliated with the OptiScaler team or with Square Enix.
Use at your own risk. Source code: $srcUrl (tag $tag).

WHAT IS IN THIS ARCHIVE
  winmm.dll                  OptiScaler-FFXIV (the game loads it by this name)
  nvngx.dll_dlssnr.dll       NR forwarder. Keep this exact name: NVIDIA's model only answers a caller whose path contains "nvngx.dll".
  OptiScaler.ini             settings; every value is "auto" (= built-in default)
  OptiScaler\                third-party libraries (Direct3D 12 core, FidelityFX, XeSS)
  Licenses\                  licences and notices of everything above, the fork's GPL-3.0 text and ATTRIBUTION.md
  NOTICE.txt, SHA256SUMS.txt

INSTALL
  1. Close the game and the launcher.
  2. Back up first. In the folder that contains ffxiv_dx11.exe, copy these to a new folder if they exist:
       winmm.dll  nvngx.dll_dlssnr.dll  OptiScaler.ini  OptiScaler\
     Example (PowerShell, run in that folder):
       `$b = "OptiScaler-backup-`$(Get-Date -Format yyyyMMdd-HHmm)"; New-Item -ItemType Directory `$b | Out-Null
       'winmm.dll','nvngx.dll_dlssnr.dll','OptiScaler.ini','OptiScaler' | Where-Object { Test-Path `$_ } | ForEach-Object { Copy-Item `$_ `$b -Recurse }
  3. Optional: check the archive. In PowerShell, in the extracted folder:  Get-Content SHA256SUMS.txt  and compare with  Get-FileHash <file>.
  4. Extract everything into the folder that contains ffxiv_dx11.exe (overwrite when asked).
  5. Add the files in "BRING YOUR OWN" for the features you want.
  6. Start the game. Press Insert to open the OptiScaler menu. To get a log file for bug reports set [Log] LogToFile=true
     (OptiScaler.log is then written next to winmm.dll).

SETTINGS (OptiScaler.ini, all of them can also be changed in the menu; restart the game after changing most of them)
  DLSS upscaling on the DirectX 11 game:   [Upscalers] Dx11Upscaler=dlss_12
  Neural Rendering (needs nvngx_dlssnr.dll): [DlssNr] Enabled=true
  Frame generation (needs DLSS-G files):   [FrameGen] Enabled=true, FGInput=upscaler, FGOutput=dlssg
  HDR10 output (Windows HDR on first):     [HDR] FfxivHDR=true      ([HDR] Mode=1 = RenoDX scene HDR)
  HUD-less frame generation (opt-in):      [HudLess] Mode=ui_free   (needs DLSS-G output)
  Defaults: Neural Rendering, HDR, HUD-less, the DLSS-G unlock and the quality override are all OFF until you switch them on.

BRING YOUR OWN (not included: not redistributable from this repository)
$(($byo | ForEach-Object { "  $_" }) -join "`n")

KNOWN LIMITS
  * Only the settings marked "tested" in the project README were played in game, on one RTX 4090. Everything else is untested.
  * HUD-less is opt-in and off by default; SDR with DLSS-G 2x-4x was tested, HDR was not.
  * 5x and 6x multi frame generation on RTX 40 are experimental; one GPU hang happened at 6x. Use 4x or lower.
  * The FFXIV-specific features (native quality, nameplates, lighting, HDR, HUD-less) are tied to one game build. After a game
    patch they switch themselves off (see the log) until the fork is updated; the rest keeps working.

UNINSTALL
  Close the game, delete winmm.dll, nvngx.dll_dlssnr.dll, OptiScaler.ini, the OptiScaler folder, Licenses, INSTALL.txt, NOTICE.txt
  and SHA256SUMS.txt from the game folder, then copy back whatever you saved in step 2.
"@

$notice = @"
OptiScaler-FFXIV $tag - notices
=====================================================================

This archive contains OptiScaler-FFXIV, a fork of OptiScaler (https://github.com/optiscaler/OptiScaler), licensed GPL-3.0.
The licence text is Licenses\OptiScaler-FFXIV_LICENSE_GPL-3.0.txt. The corresponding source code is at $srcUrl (tag $tag).

Credits (details in Licenses\ATTRIBUTION.md):
  * The OptiScaler team.
  * Dagherbou (DLSS Neural Rendering base), wilsjo2 (multipass, pre-SR and the MFG unlock), SirenBrink (FFXIV integration),
    byunjoe and Nickle (contributions to the fork).
  * Nickle: three behaviours of an unpublished build were reconstructed by reverse engineering and re-implemented
    (the NR shader edit for the "Linear light" proxy, the NR HDR mapping "Linear light + composed", the HDR PNG option
    "Include OptiScaler menu"). No source code of that build was used. See Licenses\ATTRIBUTION.md.
  * RenoDX (clshortfuse): HDR shaders behind scene HDR. Licenses\RenoDX_LICENSE.txt, RenoDX_ATTRIBUTION.txt, RenoDX_FFXIV_NOTICE.md.
  * ReShade API headers (BSD-3-Clause OR MIT), used unmodified for the built-in add-on. Licenses\ReShade_ATTRIBUTION.txt.

Third-party binaries in OptiScaler\ and their licences:
$(($include | Where-Object { $_.Dest -like 'OptiScaler\*' } | ForEach-Object { $n = Split-Path $_.Dest -Leaf; $tp = $thirdParty | Where-Object { $n -match $_.Pattern } | Select-Object -First 1; "  {0,-48} Licenses\{1}" -f $_.Dest, $tp.Licence }) -join "`n")

NVIDIA files (nvngx_dlss*.dll, Streamline) are not part of this archive; they remain under NVIDIA's own terms.
"@

Set-Content -LiteralPath (Join-Path $stage 'INSTALL.txt') -Value $install -Encoding utf8NoBOM
Set-Content -LiteralPath (Join-Path $stage 'NOTICE.txt') -Value $notice -Encoding utf8NoBOM

# ---------------------------------------------------------------- checksums, archive
$sumLines = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), ($_.FullName.Substring($stage.Length).TrimStart('\', '/') -replace '\\', '/')
}
# LF line endings, so that GNU sha256sum -c reads the file as it is
[IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS.txt'), (($sumLines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))

$zip = Join-Path $OutDir "OptiScaler-FFXIV-$Version.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)

# ---------------------------------------------------------------- verify: extract and re-hash; source folder untouched
$check = Join-Path ([IO.Path]::GetTempPath()) ("optiscaler-release-check-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
[IO.Compression.ZipFile]::ExtractToDirectory($zip, $check)
$bad = 0
foreach ($l in (Get-Content -LiteralPath (Join-Path $check 'SHA256SUMS.txt'))) {
    $h, $p = $l -split '  ', 2
    $fp = Join-Path $check $p
    if (-not (Test-Path -LiteralPath $fp) -or (Get-FileHash -LiteralPath $fp -Algorithm SHA256).Hash.ToLowerInvariant() -ne $h) { Write-Host "[release] HASH MISMATCH: $p" -ForegroundColor Red; $bad++ }
}
$inZip = @(Get-ChildItem -LiteralPath $check -Recurse -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' }).Count
if ($inZip -ne @($sumLines).Count) { Write-Host "[release] file count mismatch: $inZip in the archive, $(@($sumLines).Count) listed" -ForegroundColor Red; $bad++ }
Remove-Item -LiteralPath $check -Recurse -Force
Remove-Item -LiteralPath $stage -Recurse -Force
$treeAfter = Get-TreeHash $Package
if ($treeAfter -ne $treeBefore) { Write-Host '[release] the package folder changed while packing (it must not)' -ForegroundColor Red; $bad++ }

# ---------------------------------------------------------------- report
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host ''
Write-Host ("[release] {0}  ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
Write-Host "[release] sha256 $zipHash"
Write-Host "[release] packed $(@($sumLines).Count) files (+ SHA256SUMS.txt)"
if ($blocked.Count) { Write-Host '[release] NOT packed, not redistributable (listed under BRING YOUR OWN):'; $blocked | ForEach-Object { Write-Host "    $_" } }
else { Write-Host '[release] package contained no NVIDIA / model / Streamline files (nothing had to be excluded)' }
if ($skipped.Count) { Write-Host "[release] left out (by-products, upstream installer files): $($skipped -join ', ')" }
foreach ($w in $warnings) { Write-Host "[release] WARNING $w" -ForegroundColor Yellow }
if ($bad) { Write-Host "[release] verification FAILED ($bad)" -ForegroundColor Red; exit 4 }
Write-Host '[release] verification OK: extracted archive matches SHA256SUMS.txt, package folder unchanged'
exit 0
