[CmdletBinding()]
param([switch]$NoVersionBump)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $projectRoot
try {
    $versionFile = Join-Path $projectRoot 'VERSION'
    $previousVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    if ($previousVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION must have major.minor.patch format' }
    if (-not $NoVersionBump) {
        $parts = $previousVersion.Split('.')
        $nextVersion = '{0}.{1}.{2}' -f $parts[0], $parts[1], ([int]$parts[2] + 1)
        Set-Content -LiteralPath $versionFile -Value $nextVersion -Encoding ascii
    }
    & (Join-Path $PSScriptRoot 'Build.ps1')
    & ctest --preset release
    if ($LASTEXITCODE -ne 0) { throw "Validation failed: $LASTEXITCODE" }
    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    $binaryVersion = (Get-Item -LiteralPath (Join-Path $projectRoot 'build\Soundee_artefacts\Release\Soundee.exe')).VersionInfo.ProductVersion
    if ($binaryVersion -ne $version) { throw "Executable version $binaryVersion does not match package version $version" }
    $destination = Join-Path $projectRoot "dist\Soundee-$version"
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'build\Soundee_artefacts\Release\Soundee.exe') -Destination $destination
    Copy-Item -LiteralPath (Join-Path $projectRoot 'build\Soundee_artefacts\Release\SoundeeEndpointSetup.exe') -Destination $destination
    Copy-Item -LiteralPath (Join-Path $projectRoot 'build\Soundee_artefacts\Release\SoundeeDSP.dll') -Destination $destination
    Copy-Item -LiteralPath (Join-Path $projectRoot 'build\Soundee_artefacts\Release\SoundeeBackendProbe.exe') -Destination $destination
    Copy-Item -LiteralPath (Join-Path $projectRoot 'README.txt') -Destination $destination
    foreach ($notice in @('LICENSE', 'DISTRIBUTION.txt', 'AGPL-3.0.txt')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $notice) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\user-guide.txt') -Destination $destination
    Copy-Item -LiteralPath (Join-Path $projectRoot 'build\licenses\JUCE-LICENSE.md') -Destination (Join-Path $destination 'JUCE-LICENSE.md')
    $helperSource = Join-Path $destination 'endpoint-setup-source'
    New-Item -ItemType Directory -Path $helperSource -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\equalizerapo') -Destination $helperSource -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'src\EndpointSetup.cpp') -Destination $helperSource
    Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\endpoint-setup\CMakeLists.txt') -Destination $helperSource
    Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\endpoint-setup\README.txt') -Destination $helperSource
    Compress-Archive -LiteralPath $destination -DestinationPath (Join-Path $projectRoot "dist\Soundee-$version-win64.zip") -Force
    Write-Output "Portable package ready: Soundee-$version-win64.zip"
} catch {
    if (-not $NoVersionBump) { Set-Content -LiteralPath $versionFile -Value $previousVersion -Encoding ascii }
    throw
} finally { Pop-Location }
