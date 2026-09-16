[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$freeRdpSource = Join-Path $repositoryRoot 'FreeRDP'
$buildDirectory = Join-Path $repositoryRoot 'build/freerdp'
$installDirectory = Join-Path $repositoryRoot 'build/freerdp-install'
$vcpkgInstalledDirectory = Join-Path $repositoryRoot 'build/vcpkg-installed'

if (-not (Test-Path -LiteralPath (Join-Path $freeRdpSource 'CMakeLists.txt'))) {
    throw 'FreeRDP submodule is missing. Run: git submodule update --init --recursive'
}

$cmakeCandidates = @()
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmakeCandidates += $cmakeCommand.Source
}
$cmakeCandidates += @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
)
$cmake = $cmakeCandidates |
    Where-Object { $_ -and (Test-Path -LiteralPath $_) } |
    Select-Object -First 1

if (-not $cmake) {
    throw 'CMake was not found. Install the Visual Studio CMake tools or add cmake to PATH.'
}

$vcpkgCandidates = @()
if ($VcpkgRoot) {
    $vcpkgCandidates += $VcpkgRoot
}
$vcpkgCandidates += @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\vcpkg'
)
$resolvedVcpkgRoot = $vcpkgCandidates |
    Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'scripts/buildsystems/vcpkg.cmake')) } |
    Select-Object -First 1

if (-not $resolvedVcpkgRoot) {
    throw 'vcpkg was not found. Set VCPKG_ROOT to a vcpkg installation directory.'
}

$vcpkgToolchain = Join-Path $resolvedVcpkgRoot 'scripts/buildsystems/vcpkg.cmake'
$configureArguments = @(
    '-S', $freeRdpSource,
    '-B', $buildDirectory,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
    "-DVCPKG_MANIFEST_DIR=$repositoryRoot",
    "-DVCPKG_INSTALLED_DIR=$vcpkgInstalledDirectory",
    "-DCMAKE_INSTALL_PREFIX=$installDirectory",
    '-DBUILD_SHARED_LIBS=ON',
    '-DWITH_SMARTCARD_EMULATE=OFF',
    '-DWITH_FFMPEG=OFF',
    '-DWITH_SWSCALE=OFF',
    '-DWITH_CHANNELS=OFF',
    '-DWITH_CLIENT=OFF',
    '-DWITH_CLIENT_COMMON=ON',
    '-DWITH_CLIENT_SDL=OFF',
    '-DWITH_SERVER=ON',
    '-DWITH_SHADOW=OFF',
    '-DWITH_PROXY=OFF',
    '-DWITH_PLATFORM_SERVER=OFF',
    '-DWITH_SAMPLE=OFF'
)

Write-Host "Configuring FreeRDP in $buildDirectory"
& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw "FreeRDP configure failed with exit code $LASTEXITCODE."
}

Write-Host "Building and installing FreeRDP ($Configuration)"
& $cmake --build $buildDirectory --config $Configuration --target install --parallel
if ($LASTEXITCODE -ne 0) {
    throw "FreeRDP build failed with exit code $LASTEXITCODE."
}

$vcpkgRuntimeDirectory = Join-Path $vcpkgInstalledDirectory 'x64-windows/bin'
if ($Configuration -eq 'Debug') {
    $vcpkgRuntimeDirectory = Join-Path $vcpkgInstalledDirectory 'x64-windows/debug/bin'
}

$runtimeDependencies = Get-ChildItem -LiteralPath $vcpkgRuntimeDirectory -Filter '*.dll' -File
if (-not $runtimeDependencies) {
    throw "No vcpkg runtime dependencies were found under $vcpkgRuntimeDirectory."
}

$freeRdpRuntimeDirectory = Join-Path $installDirectory 'bin'
New-Item -ItemType Directory -Force -Path $freeRdpRuntimeDirectory | Out-Null
Copy-Item -LiteralPath $runtimeDependencies.FullName -Destination $freeRdpRuntimeDirectory -Force

$requiredFiles = @(
    'include/freerdp3/freerdp/freerdp.h',
    'include/winpr3/winpr/winpr.h',
    'lib/freerdp3.lib',
    'lib/freerdp-client3.lib',
    'lib/freerdp-server3.lib',
    'lib/winpr3.lib',
    'bin/freerdp3.dll',
    'bin/winpr3.dll',
    'bin/libcrypto-3-x64.dll',
    'bin/libssl-3-x64.dll'
)

foreach ($relativePath in $requiredFiles) {
    $installedFile = Join-Path $installDirectory $relativePath
    if (-not (Test-Path -LiteralPath $installedFile)) {
        throw "FreeRDP install is incomplete. Missing: $installedFile"
    }
}

Write-Host "FreeRDP was installed successfully at $installDirectory"
