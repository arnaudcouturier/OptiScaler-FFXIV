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
    * Entries under Licenses\ go through the same block-list as everything else (a blocked file there is left out and reported).
      Of the rest, only .txt and .md files (any case) and names without an extension are packed; anything else there
      (a .dll, an .exe, ...) stops the script with exit 3 naming the file.
    * Privacy scan: every packed PE (.dll, .exe, .addon*) is searched for a user profile path (a drive letter, a colon, one or more
      backslashes or slashes, "Users", a separator: C:\Users\, C:/Users/ and C:\\Users\\ all match; in ASCII and in UTF-16LE), which a build leaves in the debug directory or in assertion strings and which would publish the
      Windows user name of the build machine. One in a binary this fork builds (winmm.dll, nvngx.dll_dlssnr.dll) or in anything
      outside OptiScaler\ stops the script with exit 5 before any archive is written; there is no switch to ignore it. One in a
      third-party library under OptiScaler\ (FidelityFX, XeSS, Direct3D 12 core) is a vendor build path: it is reported as a
      warning with the string, and the file is packed. The scan does not look at .pdb files (-IncludeSymbols packs them knowingly).
  After writing the archive the script extracts it to a scratch folder and checks every hash against SHA256SUMS.txt, and checks that
  -Package was not modified (its tree hash before and after).

  Exit codes:
    0  archive written and verified
    2  bad arguments, or -Package / the checkout is missing something it needs
    3  a file in -Package is unclassified, a licence or notice file is missing, or the ini lacks a fork section
    4  verification of the written archive failed (hash or file count mismatch, or -Package changed)
    5  a packed binary contains a user profile path (the privacy scan above); nothing is written
.PARAMETER Package
  Folder produced by tools\package.ps1 (contains OptiScaler.dll, OptiScaler.ini, the forwarder).
.PARAMETER Version
  Release tag, for example ffxiv-2026.10.23-d9b62110 (convention: ffxiv-<yyyy.mm.dd>-<upstream sha8>[-rcN]); the leading "ffxiv-" may be left out.
.PARAMETER Exclude
  File names or package-relative paths to leave out of the archive. Default: libxell.dll (Intel's XeLL library: its licence file does not name it, so it is not redistributed; pass -Exclude @() to pack it anyway).
.PARAMETER OutDir
  Output folder, default <repo>\dist. A .gitignore containing "*" is written there so the archive can never be committed by accident.
.EXAMPLE
  pwsh -File tools\package.ps1 -Target C:\work\pkg ; pwsh -File tools\package-release.ps1 -Package C:\work\pkg -Version ffxiv-2026.10.23-d9b62110
#>
#requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Version,
    [string]$OutDir,
    [switch]$IncludeSymbols,
    [string[]]$Exclude = @('libxell.dll')
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
    if ($top -eq 'Licenses') {
        if ($blockPatterns | Where-Object { $name -match $_ }) { $blocked.Add($rel) }
        elseif ($name -match '(?i)\.(txt|md)$' -or $name -notmatch '\.') { $include.Add(@{ Source = $f.FullName; Dest = $rel }) }
        else { $unknown.Add("$rel  (only .txt, .md and names without an extension are allowed under Licenses\)") }
        continue
    }
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

# ---------------------------------------------------------------- privacy scan of the packed binaries
# A user profile path (drive letter + :\Users\) in a binary is the build machine's Windows user name; see the header.
# One pattern per encoding: a drive letter, ':', one or more '\' or '/' (so C:/Users/, C:\\Users\\ and \\?\C:\Users\ all match), "Users"
# (any case), a separator. The wide form needs the zero byte after every character, so a lone letter in front of wide text is no drive letter.
$userPathAscii = [regex]::new('[A-Za-z]:[\\/]+Users[\\/]', 'IgnoreCase, CultureInvariant, Compiled')
$userPathWide = [regex]::new('[A-Za-z]\x00:\x00(?:[\\/]\x00)+U\x00s\x00e\x00r\x00s\x00[\\/]\x00', 'IgnoreCase, CultureInvariant, Compiled')
function Find-UserPath([string]$file) {
    $bytes = [IO.File]::ReadAllBytes($file)
    $text = [Text.Encoding]::Latin1.GetString($bytes)   # one char per byte, so the regex walks the raw bytes
    $found = [System.Collections.Generic.List[string]]::new()
    foreach ($m in $userPathAscii.Matches($text)) {
        $end = $m.Index + $m.Length
        while ($end -lt $text.Length -and ($end - $m.Index) -lt 100 -and $text[$end] -ge ' ' -and $text[$end] -le '~') { $end++ }
        $found.Add($text.Substring($m.Index, $end - $m.Index))
        if ($found.Count -ge 20) { break }
    }
    foreach ($m in $userPathWide.Matches($text)) {
        $chars = [System.Text.StringBuilder]::new()
        for ($j = $m.Index; $j + 1 -lt $text.Length -and $chars.Length -lt 100 -and $text[$j + 1] -eq [char]0 -and $text[$j] -ge ' ' -and $text[$j] -le '~'; $j += 2) { [void]$chars.Append($text[$j]) }
        $found.Add($chars.ToString())
        if ($found.Count -ge 40) { break }
    }
    return @($found | Select-Object -Unique)
}
$userPathBinaries = [System.Collections.Generic.List[string]]::new()
foreach ($i in $include) {
    if ($i.Dest -notmatch '(?i)\.(dll|exe|addon\w*)$') { continue }
    $hits = Find-UserPath $i.Source
    if (-not $hits) { continue }
    $shown = ($hits | Select-Object -First 3 | ForEach-Object { '"' + $_ + '"' }) -join ', '
    if ($i.Dest -like 'OptiScaler\*') {
        # third-party library (the classification above lets nothing else in under OptiScaler\): a vendor build path, not this machine's
        $warnings.Add("$($i.Dest) : contains a user profile path $shown. It is a third-party library, so it is packed; the maintainer should check that the path is the vendor's.")
    } else {
        $userPathBinaries.Add("$($i.Dest) contains $shown")
    }
}
if ($userPathBinaries.Count -gt 0) {
    Write-Host '[release] a packed binary holds a user profile path (the Windows user name of the build machine); nothing was written:' -ForegroundColor Red
    $userPathBinaries | ForEach-Object { Write-Host "  $_" }
    Write-Host '  Rebuild so that the path is not embedded (the forwarder links with /PDBALTPATH:%_PDB%), then package again.'
    exit 5
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
    'nvngx_dlssnr.dll        NVIDIA DLSS Neural Rendering model, needed for [DlssNr] Enabled. Obtain it from NVIDIA (the DLSS SDK or the NVIDIA app/driver distribution). The ini comment in [DlssNr] names the version and, for RTX 20/30/40, the community compatibility runtime it expects. Put it beside OptiScaler, or one folder per version in OptiScaler\NR.<version>\ (for example NR.310.9) to switch between versions in the Neural Rendering card.'
    'nvngx_dlssg.dll         NVIDIA DLSS frame generation runtime, needed for DLSS-G output.'
    'sl.interposer.dll, sl.common.dll, sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll   NVIDIA Streamline plugins, placed in OptiScaler\streamline\, needed for DLSS-G output. Use the versions that match nvngx_dlssg.dll.'
    'OptiScaler\dlssg_sm86\  Community companion DLSS-G runtime, only for the experimental RTX 30 path ([DLSSG] AmpereMfgUnlock).'
    'ReShade 6.8.0           Your own ReShade install, only if you want presets applied to the HUD-less image ([HudLess] ReShadePresets).'
)

# -IncludeSymbols packs the .pdb files under their own names: the uninstall text lists them (names taken from the include list)
$symbolNames = @($include | Where-Object { $_.Dest -match '\.pdb$' -and $_.Dest -notmatch '[\\/]' } | ForEach-Object { $_.Dest })
$uninstallSymbols = if ($symbolNames.Count) { "    plus the symbol files this archive was made with: $($symbolNames -join ', ')`n" } else { '' }

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
     (OptiScaler.log is then written next to winmm.dll). The log contains the game folder path, and it also holds the file paths and
     hashes of the NVIDIA runtimes it finds. Before you post it publicly, remove or mask every path that contains your Windows user name.

SETTINGS (OptiScaler.ini, all of them can also be changed in the menu; restart the game after changing most of them)
  (the combination used in the maintainer's tested setup; in the menu or the ini)
  DLSS upscaling on the DirectX 11 game:   [Upscalers] Dx11Upscaler=dlss_12
  Neural Rendering (needs nvngx_dlssnr.dll): [DlssNr] Enabled=true
  Frame generation (needs DLSS-G files):   [FrameGen] Enabled=true, FGInput=upscaler, FGOutput=dlssg
  HDR10 output (Windows HDR on first):     [HDR] FfxivHDR=true      ([HDR] Mode=1 = RenoDX scene HDR)
  HUD-less frame generation (opt-in):      [HudLess] Mode=recomposition   (needs DLSS-G output; use it with the DLSS-G preset Auto or B; ui_free is experimental)
  DLSS-G render preset (opt-in):           [DLSSG] RenderPreset=0..3   (0 Auto, 1 A, 2 B, 3 C; changeable in the menu while the game runs)
  Neural Rendering model version:          [DlssNr] Runtime=auto|legacy|NR.<version>
  Defaults: Neural Rendering, HDR, HUD-less, the DLSS-G unlock and the quality override are all OFF until you switch them on.
  Two things are ON by default in FFXIV: the automatic mipmap bias ([Mipmap] MipmapBiasAuto) and the quality change without the 500 ms pause
  ([NgxRelease] SkipReleaseSleep). Set them to false to turn them off.
  Menu look:                               [Menu] Style=ember       (flat charcoal menu; set [Menu] AccentColorR/G/B for the accent, default is blue)

BRING YOUR OWN (not included: not redistributable from this repository)
$(($byo | ForEach-Object { "  $_" }) -join "`n")

KNOWN LIMITS
  * Only the settings marked "tested" in the project README were played in game, on one RTX 4090. Everything else is untested.
  * HUD-less is opt-in and off by default; recomposition in SDR with DLSS-G 2x-4x was tested, HDR was not. ui_free is experimental (in one scene its
    UI mask fell back for most frames).
  * DLSS-G Preset A and C turn NVIDIA's UI recomposition off (Preset C also turns FG Lite on), so with them HUD-less recomposition is not requested
    (the log and the menu say so). Use preset Auto or B with recomposition.
  * Window mode ([Window] Mode=on), the automatic multiplier ([DLSSG] AutoMFG=true), "Run NR before DLSS" in the character
    preview and the game jitter correction are off by default. Window mode needs [XeFG] ForceBorderless=true and ReShade.
    The jitter correction is experimental and made no visible difference at the Quality mode in the maintainer's test. The source-level jitter fix
    ([FFXIV] JitterSource) is experimental, off by default and supports one game executable build.
  * DLSS presets L and M cost more fps than K (heavier models); K is the default.
  * libxell.dll (Intel XeLL) is not part of this archive.
  * 5x and 6x multi frame generation on RTX 40 are experimental; one GPU hang happened at 6x. Use 4x or lower.
  * The FFXIV-specific features (native quality, nameplates, lighting, HDR, HUD-less) are tied to one game build. After a game
    patch they switch themselves off (see the log) until the fork is updated; the rest keeps working.

UNINSTALL
  Close the game and delete only what this archive installed, in the folder that contains ffxiv_dx11.exe:
    winmm.dll, nvngx.dll_dlssnr.dll, OptiScaler.ini, INSTALL.txt, NOTICE.txt, SHA256SUMS.txt, and the Licenses folder
$uninstallSymbols    plus OptiScaler.log, if you turned LogToFile on (the game writes it next to winmm.dll)
  and these files inside the OptiScaler folder (a folder left empty by this can be removed too):
$(($include | Where-Object { $_.Dest -like 'OptiScaler\*' } | ForEach-Object { "    $($_.Dest)" }) -join "`n")
  Do NOT delete the OptiScaler folder as a whole. Leave your own NVIDIA files alone: nvngx_dlssg.dll, the streamline folder, the
  NR.<version> folders, dlssg_sm86 and any nvngx_dlssnr.dll you placed there are yours (see BRING YOUR OWN). Then copy back whatever
  you saved in step 2.
"@

$notice = @"
OptiScaler-FFXIV $tag - notices
=====================================================================

This archive contains OptiScaler-FFXIV, a fork of OptiScaler (https://github.com/optiscaler/OptiScaler), licensed GPL-3.0.
The licence text is Licenses\OptiScaler-FFXIV_LICENSE_GPL-3.0.txt. The corresponding source code is at $srcUrl (tag $tag).

Credits (the table by area is in Licenses\ATTRIBUTION.md):
  * The OptiScaler team.
  * SirenBrink (https://github.com/SirenBrink/OptiScaler_DLSSNR_Multipass_MFG_FFXIV, GPL-3.0): a large part of this archive's
    code comes from this community fork: the FFXIV integration, OptiHDR and scene HDR, HDR PNG capture, XeFG/XeMFG, frame
    markers and pacing. Its v6.0 source also contains the NR "Linear light" mode, the HDR PNG option "Include OptiScaler menu"
    and the character preview, which this archive has too. The DLSS-G render preset, NR model picker, bind-time mip bias and
    source-level jitter fix follow the design of its v6.0 source, with code written for this repository.
  * Dagherbou (DLSS Neural Rendering base), y4my4my4m (multipass, MFG unlock), wilsjo2 (pre-SR, native path, MFG unlock hardening),
    byunjoe (HUD-less frame generation), and janblade, Citrus333, Ainquisition, MatheusGViana, ShyVortex, BeliyG3, stegalloway,
    mavismmg and dashdogy, whose work that community fork adapts.
  * RenoDX (clshortfuse): the NR colour composition (Licenses\RenoDX_ATTRIBUTION.txt) and the HDR shaders behind scene HDR
    (Licenses\RenoDX_FFXIV_NOTICE.md). Licence: Licenses\RenoDX_LICENSE.txt.
  * ReShade API headers (BSD-3-Clause OR MIT), used unmodified for the built-in add-on. Licenses\ReShade_ATTRIBUTION.txt.

Third-party binaries in OptiScaler\ and their licences:
$(($include | Where-Object { $_.Dest -like 'OptiScaler\*' } | ForEach-Object { $n = Split-Path $_.Dest -Leaf; $tp = $thirdParty | Where-Object { $n -match $_.Pattern } | Select-Object -First 1; "  {0,-48} Licenses\{1}{2}" -f $_.Dest, $tp.Licence, $(if ($tp.Names) { '' } else { '  (that licence file does not name this library; the same Intel SDK licence is assumed to cover it)' }) }) -join "`n")

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
