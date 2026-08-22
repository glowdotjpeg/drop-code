[CmdletBinding()]
param(
    [ValidateSet("x64", "x86", "arm64")]
    [string]$Architecture = "x64",

    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "Release",

    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version = "0.3.0",

    [switch]$RunTests,

    [string]$BuildDir
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = Join-Path $Root "build"
}

$vswhereCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
    (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
)
$VsWhere = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object { $_ } | Select-Object -First 1
$VsPath = $null
if ($VsWhere) {
    $installations = @(& $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if ($LASTEXITCODE -eq 0 -and $installations.Count -gt 0) {
        $VsPath = $installations[0].Trim()
    }
}
if (-not $VsPath) {
    throw "Visual Studio with the C++ toolchain was not found. Install the Desktop development with C++ workload."
}
if ($VsWhere) {
    $vsInstallerDirectory = Split-Path -Parent $VsWhere
    $env:PATH = "$vsInstallerDirectory;$env:PATH"
}

$Vcvars = Join-Path $VsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path -LiteralPath $Vcvars)) {
    throw "vcvarsall.bat not found at $Vcvars"
}

$CmakePath = Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$NinjaPath = Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path -LiteralPath $CmakePath)) {
    $CmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($CmakeCommand) { $CmakePath = $CmakeCommand.Source }
}
if (-not (Test-Path -LiteralPath $NinjaPath)) {
    $NinjaCommand = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($NinjaCommand) { $NinjaPath = $NinjaCommand.Source }
}
if (-not (Test-Path -LiteralPath $CmakePath)) { throw "cmake.exe was not found." }
if (-not (Test-Path -LiteralPath $NinjaPath)) { throw "ninja.exe was not found." }

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$command = "`"$Vcvars`" $Architecture && `"$CmakePath`" -S `"$Root`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DDROPCODE_VERSION=$Version -DCMAKE_MAKE_PROGRAM=`"$NinjaPath`" && `"$CmakePath`" --build `"$BuildDir`""
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($RunTests) {
    $CtestPath = Join-Path (Split-Path -Parent $CmakePath) "ctest.exe"
    if (-not (Test-Path -LiteralPath $CtestPath)) {
        throw "ctest.exe was not found next to $CmakePath"
    }
    & $CtestPath --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$Exe = Join-Path $BuildDir "DropCode.exe"
Write-Host "Built: $Exe"
