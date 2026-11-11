#Requires -Version 7.0
param(
    [string]$MsysRoot,
    [string]$NgxSdkPath
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$MsysRoot) { $MsysRoot = Join-Path (Split-Path -Parent $root) 'tools/msys64' }
if (!$NgxSdkPath) { $NgxSdkPath = Join-Path (Split-Path -Parent $root) 'dependencies/nvidia-sdk' }
$bash = Join-Path $MsysRoot 'usr/bin/bash.exe'
$cmake = Join-Path $MsysRoot 'clang64/bin/cmake.exe'
if (!(Test-Path -LiteralPath $bash) -or !(Test-Path -LiteralPath $cmake)) {
    throw 'Prepare the MSYS2 CLANG64 dependencies first; see docs/diy/windows-zh.md.'
}
Push-Location $root
$savedPath = $env:PATH
$savedMsystem = $env:MSYSTEM
$savedChere = $env:CHERE_INVOKING
try {
    $env:PATH = (Join-Path $MsysRoot 'clang64/bin') + ';' + $env:PATH
    $env:MSYSTEM = 'CLANG64'
    $env:CHERE_INVOKING = '1'
    $build = @'
set -euo pipefail
cmake -S . -B build/zh-gui -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCHIAKI_ENABLE_CLI=OFF \
    -DCHIAKI_ENABLE_TESTS=ON -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF
cmake --build build/zh-gui --parallel 6
ctest --test-dir build/zh-gui --output-on-failure
cmake -S gui/enhancement/tests -B build/zh-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/zh-tests --parallel 6
ctest --test-dir build/zh-tests --output-on-failure
mkdir -p build/zh-stage
cp /clang64/bin/vulkan-1.dll build/zh-stage/
bash scripts/deploy-windows-msys2.sh build/zh-stage build/zh-gui/gui/chiaki.exe \
    "$PWD/build/zh-gui/third-party/cpp-steam-tools" /clang64 gui/src/qml
'@
    & $bash -lc $build
    if ($LASTEXITCODE) { throw 'GUI build, validation or deployment failed.' }
    & "$PSScriptRoot/Build-EnhancementWorker.ps1" -NgxSdkPath $NgxSdkPath `
        -BuildDirectory (Join-Path $root 'build/zh-worker') -CMake $cmake
    if (!$?) { throw 'Enhancement worker build failed.' }
    New-Item -ItemType Directory -Path build/zh-stage/enhancement -Force | Out-Null
    Copy-Item build/zh-worker/Release/nvngx.dll build/zh-stage/enhancement/nvngx.dll -Force
    Write-Host 'Localized build staged in build/zh-stage. Runtime staging and release publication are separate.'
} finally {
    $env:PATH = $savedPath
    $env:MSYSTEM = $savedMsystem
    $env:CHERE_INVOKING = $savedChere
    Pop-Location
}
