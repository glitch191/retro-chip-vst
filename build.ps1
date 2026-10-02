<#
.SYNOPSIS
    One-command build for retro-chip-vst on Windows x64.

.DESCRIPTION
    Locates Visual Studio 2022 (or Build Tools) with vswhere, imports the x64
    developer environment, then configures, builds and tests with CMake + Ninja.
    CMake and Ninja bundled with Visual Studio are used when they are not on PATH.

.PARAMETER Preset
    CMake preset name (see CMakePresets.json). Default: windows-x64-release.

.PARAMETER NoTests
    Skip running the chipdsp unit tests after the build.

.PARAMETER Install
    Copy the built .vst3 bundle to C:\Program Files\Common Files\VST3 (asks for elevation).
    The windows-x64-release preset also copies it after every build that relinks the
    plugin (RCV_COPY_PLUGIN_AFTER_BUILD), without elevation: that copy needs write access
    to the installed bundle folder and only warns when it fails.

.PARAMETER Clean
    Delete the preset's build directory before configuring.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Preset dsp-only-release
    .\build.ps1 -Install
#>
[CmdletBinding()]
param(
    [string]$Preset = "windows-x64-release",
    [switch]$NoTests,
    [switch]$Install,
    [switch]$Clean,
    [string]$Target = ""
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root

# --- Locate Visual Studio -------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 or the Build Tools with the 'Desktop development with C++' workload."
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with the C++ x64 toolset was found."
}
Write-Host "Visual Studio: $vsPath"

# --- Import the x64 developer environment into this process ----------------------
$vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
# VsDevCmd.bat expects vswhere.exe on PATH.
$env:PATH = (Split-Path $vswhere) + ";" + $env:PATH
$previousEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$envDump = cmd.exe /s /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 -no_logo && set"
$ErrorActionPreference = $previousEap
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

# --- CMake / Ninja fallbacks (bundled with Visual Studio) ---------------------------
$cmakeDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -and (Test-Path $cmakeDir)) { $env:PATH = "$cmakeDir;$env:PATH" }
if (-not (Get-Command ninja -ErrorAction SilentlyContinue) -and (Test-Path $ninjaDir)) { $env:PATH = "$ninjaDir;$env:PATH" }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "cmake not found. Install CMake 3.22+ or the Visual Studio 'C++ CMake tools' component." }
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { throw "ninja not found. Install Ninja or the Visual Studio 'C++ CMake tools' component." }

Write-Host ("CMake : " + (cmake --version | Select-Object -First 1))
Write-Host ("Ninja : " + (ninja --version))

# --- Configure / build / test ----------------------------------------------------
$buildDir = Join-Path $root "build\$Preset"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

if ($Target) {
    cmake --build --preset $Preset --target $Target
} else {
    cmake --build --preset $Preset
}
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

if (-not $NoTests) {
    ctest --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
}

# --- Optional install to the system VST3 folder -------------------------------------
# C:\Program Files\Common Files\VST3 needs administrator rights: when this script is not
# elevated, the copy runs in an elevated PowerShell (one UAC prompt).
if ($Install) {
    $bundle = Get-ChildItem -Path $buildDir -Recurse -Directory -Filter "*.vst3" | Select-Object -First 1
    if (-not $bundle) { throw "No .vst3 bundle found under $buildDir (was the plugin built?)" }
    $dest = Join-Path $env:CommonProgramFiles "VST3"
    $target = Join-Path $dest $bundle.Name
    # The copy stops at the first error, and checks first that no host has the installed
    # plugin loaded: removing a bundle whose DLL is locked deletes the other files and then
    # copies the new bundle inside the old folder.
    $installedDll = Join-Path $target "Contents\x86_64-win\$($bundle.Name)"
    $copy = "`$ErrorActionPreference = 'Stop'; " +
            "if (Test-Path '$installedDll') { [IO.File]::Open('$installedDll', 'Open', 'ReadWrite', 'None').Close() }; " +
            "New-Item -ItemType Directory -Force -Path '$dest' | Out-Null; " +
            "if (Test-Path '$target') { Remove-Item -Recurse -Force '$target' }; " +
            "Copy-Item -Recurse -Path '$($bundle.FullName)' -Destination '$target'"
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
        Invoke-Expression $copy
    } else {
        $p = Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile", "-Command", $copy -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "Install failed (elevated copy exit code $($p.ExitCode))" }
    }
    $builtDll = Join-Path $bundle.FullName "Contents\x86_64-win\$($bundle.Name)"
    if (-not (Test-Path $installedDll) -or (Get-FileHash $installedDll).Hash -ne (Get-FileHash $builtDll).Hash) {
        throw "Install failed: $installedDll is not the built plugin (is a host using it? close it and retry)"
    }
    Write-Host "Installed: $target"
}

Write-Host "Done."
