# Test prepare step (tools\test.ps1 `Prepare`): builds the headers of the G-buffer remap shaders the way the product build does
# (ext\ext.props, ExtGenerateShaders): the ExtShader items are read from ReShadeGBuffer.shaders.props, compiled with the tracked
# fxc (-nologo -T <profile> -E <entry> -O3 ...) and turned into headers by tools\bin2h.ps1, into {out}\ext_gen. The tests then
# include the same bytecode the product ships.
param([Parameter(Mandatory)][string]$Repo, [Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'

$props = Join-Path $Repo 'OptiScaler\ext\reshade\ReShadeGBuffer.shaders.props'
$fxc = Join-Path $Repo 'OptiScaler\shaders\shader_tools\fxc.exe'
$bin2h = Join-Path $Repo 'tools\bin2h.ps1'
$common = Join-Path $Repo 'OptiScaler\ext\shaders\common'
$gen = Join-Path $Out 'ext_gen'
New-Item -ItemType Directory -Path $gen -Force | Out-Null

[xml]$xml = Get-Content -Raw -LiteralPath $props
$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace('m', 'http://schemas.microsoft.com/developer/msbuild/2003')
$items = @($xml.SelectNodes('//m:ExtShader', $ns))
if ($items.Count -eq 0) { throw "no ExtShader item in $props" }

function Meta($node, [string]$name, [string]$default) {
    $n = $node.SelectSingleNode("m:$name", $ns)
    if ($n -and $n.InnerText) { return $n.InnerText } else { return $default }
}

foreach ($item in $items) {
    $src = $item.GetAttribute('Include').Replace('$(MSBuildThisFileDirectory)', ((Split-Path -Parent $props) + '\'))
    $compiler = Meta $item 'Compiler' 'fxc'
    if ($compiler -ne 'fxc') { throw "$src : the prepare step only builds fxc shaders" }
    $shaderProfile = Meta $item 'Profile' 'cs_5_0'
    $entry = Meta $item 'Entry' 'CSMain'
    $variable = Meta $item 'Variable' ''
    $header = Meta $item 'Header' ''
    if (-not $variable -or -not $header) { throw "$src : Variable and Header are required" }
    $bin = Join-Path $gen ($header + '.bin')

    $fxcArgs = @('-nologo', '-T', $shaderProfile, '-E', $entry, '-O3')
    foreach ($d in (Meta $item 'Defines' '').Split(';', [StringSplitOptions]::RemoveEmptyEntries)) { $fxcArgs += @('-D', $d) }
    $fxcArgs += @('-I', (Split-Path -Parent $src), '-I', $common)
    $extra = Meta $item 'Args' ''
    if ($extra) { $fxcArgs += $extra.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) }
    $fxcArgs += @($src, '-Fo', $bin)

    $output = & $fxc @fxcArgs 2>&1
    if ($LASTEXITCODE -ne 0) { $output | ForEach-Object { Write-Host $_ }; throw "fxc failed for $src ($entry)" }
    & pwsh -NoProfile -File $bin2h -In $bin -Out (Join-Path $gen $header) -Name $variable
    if ($LASTEXITCODE -ne 0) { throw "bin2h failed for $header" }
}
