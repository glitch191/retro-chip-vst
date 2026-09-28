<#
.SYNOPSIS
    Dot-source this file to get an MSVC x64 developer environment in the current
    PowerShell session, with cmake/ctest/ninja (bundled with Visual Studio when not on
    PATH) and git (GitHub Desktop's bundled copy when git is not on PATH).

.EXAMPLE
    . .\tools\dev-env.ps1
    cmake --preset dsp-only-release
#>

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 or Build Tools." }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio C++ x64 toolset found." }

$env:PATH = (Split-Path $vswhere) + ";" + $env:PATH
$prev = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$dump = cmd.exe /s /c "`"$vsPath\Common7\Tools\VsDevCmd.bat`" -arch=x64 -host_arch=x64 -no_logo && set"
$ErrorActionPreference = $prev
foreach ($line in $dump) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process") }
}

$cmakeDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -and (Test-Path $cmakeDir)) { $env:PATH = "$cmakeDir;$env:PATH" }
if (-not (Get-Command ninja -ErrorAction SilentlyContinue) -and (Test-Path $ninjaDir)) { $env:PATH = "$ninjaDir;$env:PATH" }

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $desktop = Get-ChildItem "$env:LOCALAPPDATA\GitHubDesktop" -Directory -Filter "app-*" -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($desktop) {
        $gitCmd = Join-Path $desktop.FullName "resources\app\git\cmd"
        if (Test-Path $gitCmd) { $env:PATH = "$gitCmd;$env:PATH" }
    }
}

Write-Host ("MSVC dev env ready: " + $vsPath)
