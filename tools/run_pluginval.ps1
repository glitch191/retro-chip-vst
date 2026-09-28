<#
.SYNOPSIS
    Validate the built VST3 with pluginval (Tracktion), strictness level 5 by default.

.DESCRIPTION
    Uses tools\bin\pluginval.exe when present, otherwise downloads the official release
    archive from https://github.com/Tracktion/pluginval/releases (v1.0.4, Windows x64).

.PARAMETER Plugin
    Path to the .vst3 bundle. Default: the bundle found under build\windows-x64-release.

.PARAMETER Strictness
    pluginval strictness level 1..10. Default 5.

.EXAMPLE
    .\tools\run_pluginval.ps1
    .\tools\run_pluginval.ps1 -Strictness 10 -Plugin "build\windows-x64-release\plugin\RetroChip_artefacts\Release\VST3\Retro Chip.vst3"
#>
[CmdletBinding()]
param(
    [string]$Plugin = "",
    [int]$Strictness = 5,
    [int]$TimeoutMs = 120000,
    [switch]$SkipGuiTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$binDir = Join-Path $PSScriptRoot "bin"
$exe = Join-Path $binDir "pluginval.exe"

if (-not (Test-Path $exe)) {
    New-Item -ItemType Directory -Force $binDir | Out-Null
    $zip = Join-Path $binDir "pluginval_Windows.zip"
    Write-Host "Downloading pluginval v1.0.4 (Tracktion, official release)..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip" -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $binDir -Force
    Remove-Item $zip
}

if (-not $Plugin) {
    $bundle = Get-ChildItem -Path (Join-Path $root "build\windows-x64-release") -Recurse -Directory -Filter "*.vst3" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $bundle) { throw "No .vst3 found under build\windows-x64-release. Build first (.\build.ps1)." }
    $Plugin = $bundle.FullName
}

$outDir = Join-Path $root "build-reports\pluginval"
New-Item -ItemType Directory -Force $outDir | Out-Null

$args = @("--strictness-level", $Strictness, "--validate-in-process", "--timeout-ms", $TimeoutMs, "--output-dir", $outDir, "--validate", $Plugin)
if ($SkipGuiTests) { $args = @("--skip-gui-tests") + $args }

Write-Host "pluginval $($args -join ' ')"
& $exe @args
$code = $LASTEXITCODE
Write-Host "pluginval exit code: $code (0 = all tests passed)"
exit $code
