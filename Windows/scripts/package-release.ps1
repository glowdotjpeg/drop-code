[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$Version = "0.1.0",
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = Join-Path $Root "build"
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $Root "dist"
}

$Exe = Join-Path $BuildDir "DropCode.exe"
$Bootstrap = Join-Path $BuildDir "Microsoft.WindowsAppRuntime.Bootstrap.dll"
if (-not (Test-Path -LiteralPath $Exe)) { throw "DropCode.exe was not found at $Exe" }
if (-not (Test-Path -LiteralPath $Bootstrap)) {
    throw "Microsoft.WindowsAppRuntime.Bootstrap.dll was not found at $Bootstrap"
}

$Name = "DropCode-v$Version-windows-x64"
$Staging = Join-Path $OutputDir $Name
$Archive = Join-Path $OutputDir "$Name.zip"
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if (Test-Path -LiteralPath $Staging) {
    Remove-Item -LiteralPath $Staging -Recurse -Force
}
if (Test-Path -LiteralPath $Archive) {
    Remove-Item -LiteralPath $Archive -Force
}
New-Item -ItemType Directory -Force -Path $Staging | Out-Null

Copy-Item -LiteralPath $Exe -Destination $Staging
Copy-Item -LiteralPath $Bootstrap -Destination $Staging
Copy-Item -LiteralPath (Join-Path $Root "README.md") -Destination (Join-Path $Staging "README.md")
Copy-Item -LiteralPath (Join-Path $Root "third_party\libvterm\LICENSE") `
          -Destination (Join-Path $Staging "libvterm-LICENSE.txt")

Compress-Archive -Path (Join-Path $Staging "*") -DestinationPath $Archive
$Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$Archive.sha256" -Value "$Hash  $([IO.Path]::GetFileName($Archive))"
Remove-Item -LiteralPath $Staging -Recurse -Force
Write-Host $Archive
