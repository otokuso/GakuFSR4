param(
    [Parameter(Mandatory = $true)] [string] $GameRoot,
    [switch] $FrameGeneration
)

$pluginRoot = Join-Path $GameRoot 'GakuLoader/plugins'
$configRoot = Join-Path $GameRoot 'GakuLoader/Configs'
$runtimeRoot = Join-Path $GameRoot 'GakuLoader/PluginData/gakufsr4'

$required = @(
    (Join-Path $pluginRoot 'GakuFSR4.dll'),
    (Join-Path $configRoot 'gakufsr4.ini'),
    (Join-Path $runtimeRoot 'amd_fidelityfx_loader_dx12.dll'),
    (Join-Path $runtimeRoot 'amd_fidelityfx_upscaler_dx12.dll')
)
if ($FrameGeneration) {
    $required += (Join-Path $runtimeRoot 'amd_fidelityfx_framegeneration_dx12.dll')
}

$missing = @($required | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
if ($missing.Count -gt 0) {
    $missing | ForEach-Object { Write-Error "Missing deployment file: $_" }
    exit 1
}

Write-Host 'GakuFSR4 deployment paths verified. This does not verify AMD runtime compatibility or GPU support.'
