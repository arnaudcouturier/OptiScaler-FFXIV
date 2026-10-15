# Shared helpers of the runner scripts that slice production source text into a test (X class, see U6 section 5).
# The slices are located by exact signature / marker string; when production code moves or is renamed the runner fails
# with "Production ... changed" instead of silently testing a stale copy. Dot-source this file.

$script:NrDeferredSr = 'OptiScaler\ext\nr\presr\DlssNr_DeferredSr.inl'
$script:NrDx12 = 'OptiScaler\ext\nr\pass\DlssNr_Dx12.cpp'
$script:NrNative = 'OptiScaler\ext\nr\native\DlssNrNative.cpp'

function Read-Source([string]$Repo, [string]$Relative) {
    $path = Join-Path $Repo $Relative
    if (-not (Test-Path $path)) { throw "Production source moved: $Relative" }
    return [IO.File]::ReadAllText($path)
}

# Text from the first occurrence of $Start (searching from $Offset) up to, not including, the next $End after it
function Read-Fragment([string]$Source, [string]$Start, [string]$End, [int]$Offset = 0) {
    $a = $Source.IndexOf($Start, $Offset)
    if ($a -lt 0) { throw "Production fragment changed: $Start" }
    $b = $Source.IndexOf($End, $a)
    if ($b -lt 0) { throw "Production fragment changed: $End" }
    return $Source.Substring($a, $b - $a)
}

# A whole function body starting at the exact $Signature, through its matching closing brace
function Read-Function([string]$Source, [string]$Signature) {
    $start = $Source.IndexOf($Signature)
    if ($start -lt 0) { throw "Missing production function: $Signature" }
    $open = $Source.IndexOf('{', $start)
    $depth = 1
    $end = $open + 1
    while ($depth -gt 0 -and $end -lt $Source.Length) {
        if ($Source[$end] -eq '{') { $depth++ }
        if ($Source[$end] -eq '}') { $depth-- }
        $end++
    }
    if ($depth -ne 0) { throw "Incomplete production function: $Signature" }
    return $Source.Substring($start, $end - $start)
}

function Invoke-Cl([string]$Description, [string[]]$ClArgs) {
    & cl.exe @ClArgs
    if ($LASTEXITCODE -ne 0) { throw "$Description compilation failed." }
}
