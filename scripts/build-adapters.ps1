[CmdletBinding()]
param(
    [string]$OutputRoot = '',
    [switch]$Test
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (!$OutputRoot) { $OutputRoot = Join-Path $repoRoot 'build/standalone' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$dependencies = Join-Path $OutputRoot 'dependencies'

function Invoke-Tool([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE." }
}

function Get-Dependency([string]$Url, [string]$ArchiveName, [string]$Sha256, [string]$DirectoryName) {
    $archive = Join-Path $dependencies $ArchiveName
    if (!(Test-Path -LiteralPath $archive -PathType Leaf)) {
        Invoke-WebRequest -Uri $Url -OutFile $archive -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Sha256) {
        throw "SHA-256 mismatch for $ArchiveName. Remove the cached archive and retry."
    }
    Invoke-Tool 'tar.exe' @('-xf', $archive, '-C', $dependencies)
    return Join-Path $dependencies $DirectoryName
}

$null = New-Item -ItemType Directory -Force -Path $dependencies
$detoursCommit = '1dbd4ea281d73d478aa8d09a8b83fbd265f715f7'
$detours = Get-Dependency "https://codeload.github.com/microsoft/Detours/tar.gz/$detoursCommit" "detours-$detoursCommit.tar.gz" `
    '38d435d2750b4e3af57f56c77af52bc058123c5e7dd734945b7680ec52e5c91c' "Detours-$detoursCommit"
$sdlVersion = '3.4.20'
$sdl = Get-Dependency "https://static.crates.io/crates/sdl3-src/sdl3-src-$sdlVersion.crate" "sdl3-src-$sdlVersion.crate" `
    'dc5181b85760f18170dbc15b27b00d5aaaa003058bb14333ec7dd280665ef3a4' "sdl3-src-$sdlVersion"
$vulkanHeaders = Join-Path $sdl 'SDL/src/video/khronos'
$install = Join-Path $OutputRoot 'package'
$source = Join-Path $repoRoot 'src'
$testFlag = if ($Test) { 'ON' } else { 'OFF' }
$builds = @(
    @{ Name = 'x64'; Platform = 'x64' }
    @{ Name = 'x86'; Platform = 'Win32' }
)

foreach ($build in $builds) {
    $directory = Join-Path $OutputRoot $build.Name
    $arguments = @('-S', $source, '-B', $directory, '-G', 'Visual Studio 17 2022',
        '-A', $build.Platform, "-DDETOURS_SOURCE_DIR=$detours", "-DBUILD_TESTING=$testFlag")
    if ($build.Name -eq 'x64') { $arguments += "-DVULKAN_LAYER_INCLUDE_DIR=$vulkanHeaders" }
    Invoke-Tool 'cmake' $arguments
    Invoke-Tool 'cmake' @('--build', $directory, '--config', 'Release', '--parallel', '4')
    Invoke-Tool 'cmake' @('--install', $directory, '--config', 'Release', '--prefix', $install)
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE'), (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md') -Destination $install
Copy-Item -LiteralPath (Join-Path $detours 'LICENSE.md') -Destination (Join-Path $install 'LICENSE-Detours.md')

if ($Test) {
    foreach ($build in $builds) {
        Invoke-Tool 'ctest' @('--test-dir', (Join-Path $OutputRoot $build.Name), '-C', 'Release', '--output-on-failure')
    }
}
Write-Output "Windows helper package: $install"
