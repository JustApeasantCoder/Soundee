[CmdletBinding()]
param([switch]$NoVersionBump, [string]$CompilerPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $CompilerPath) {
    $compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($compiler) { $CompilerPath = $compiler.Source }
    else { $CompilerPath = Join-Path $projectRoot 'build\tools\innosetup\tools\ISCC.exe' }
}
if (-not (Test-Path -LiteralPath $CompilerPath)) {
    throw 'Inno Setup 6.6 or later is required. Supply -CompilerPath with the path to ISCC.exe.'
}
& (Join-Path $PSScriptRoot 'Package.ps1') -NoVersionBump:$NoVersionBump
$version = (Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
$packageDir = Join-Path $projectRoot "dist\Soundee-$version"
& $CompilerPath "/DAppVersion=$version" "/DPackageDir=$packageDir" "/DOutputDirPath=$(Join-Path $projectRoot 'dist')" (Join-Path $PSScriptRoot 'Soundee.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed: $LASTEXITCODE" }
$artifacts = @("Soundee-$version-win64-setup.exe", "Soundee-$version-win64.zip")
$checksums = foreach ($artifact in $artifacts) {
    $hash = Get-FileHash -LiteralPath (Join-Path $projectRoot "dist\$artifact") -Algorithm SHA256
    '{0}  {1}' -f $hash.Hash.ToLowerInvariant(), $artifact
}
Set-Content -LiteralPath (Join-Path $projectRoot "dist\Soundee-$version-SHA256SUMS.txt") -Value $checksums -Encoding ascii
Write-Output "Installer ready: Soundee-$version-win64-setup.exe"
