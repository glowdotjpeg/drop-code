[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$NuGetExe,

    [string]$WindowsAppSdkVersion = "2.3.1",

    [string]$CppWinRtVersion = "2.0.250303.1"
)

$ErrorActionPreference = "Stop"

$NuGetVersion = "7.9.0"
$NuGetSha256 = "992D70CAC5B06C38EFEC91806CABA64CDCC07E6D963A0959DBBBAF264D33B800"

New-Item -ItemType Directory -Force -Path $PackageDirectory | Out-Null
$nugetParent = Split-Path -Parent $NuGetExe
New-Item -ItemType Directory -Force -Path $nugetParent | Out-Null

if (-not (Test-Path -LiteralPath $NuGetExe)) {
    Invoke-WebRequest `
        -Uri "https://dist.nuget.org/win-x86-commandline/v$NuGetVersion/nuget.exe" `
        -OutFile $NuGetExe `
        -UseBasicParsing
}

$sha256 = [System.Security.Cryptography.SHA256]::Create()
$stream = [System.IO.File]::OpenRead($NuGetExe)
try {
    $actualHash = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '')
}
finally {
    $stream.Dispose()
    $sha256.Dispose()
}
if ($actualHash -ne $NuGetSha256) {
    throw "NuGet checksum verification failed for $NuGetExe."
}

$source = "https://api.nuget.org/v3/index.json"

& $NuGetExe install Microsoft.WindowsAppSDK `
    -Version $WindowsAppSdkVersion `
    -OutputDirectory $PackageDirectory `
    -Source $source `
    -NonInteractive `
    -ForceEnglishOutput
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft.WindowsAppSDK restore failed with exit code $LASTEXITCODE."
}

& $NuGetExe install Microsoft.Windows.CppWinRT `
    -Version $CppWinRtVersion `
    -OutputDirectory $PackageDirectory `
    -Source $source `
    -NonInteractive `
    -ForceEnglishOutput
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft.Windows.CppWinRT restore failed with exit code $LASTEXITCODE."
}
