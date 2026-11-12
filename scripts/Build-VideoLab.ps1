#Requires -Version 7.0
# Builds the standalone frame generation video lab (gui/enhancement/videolab) and places it
# next to a staged Chiaki build, where it finds the worker, the NVIDIA runtimes and the Qt DLLs.
# See docs/diy/video-lab.md. Nothing here touches the GUI, worker or release builds.
param(
    [string]$MsysRoot,
    [string]$BuildDirectory,
    [string]$StageDirectory
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$MsysRoot) { $MsysRoot = Join-Path (Split-Path -Parent $root) 'tools/msys64' }
if (!$BuildDirectory) { $BuildDirectory = Join-Path $root 'build/zh-videolab' }
if (!$StageDirectory) {
    # Prefer the runnable local build: it also carries runtimes/.
    $StageDirectory = Join-Path $root 'release/ngx'
    if (!(Test-Path -LiteralPath (Join-Path $StageDirectory 'enhancement/nvngx.dll'))) {
        $StageDirectory = Join-Path $root 'build/zh-stage'
    }
}
$bash = Join-Path $MsysRoot 'usr/bin/bash.exe'
if (!(Test-Path -LiteralPath $bash)) {
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
    $env:VIDEOLAB_BUILD = $BuildDirectory
    $build = @'
set -euo pipefail
build="$(cygpath -u "$VIDEOLAB_BUILD")"
cmake -S gui/enhancement/videolab -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel 6
ctest --test-dir "$build" --output-on-failure
'@
    & $bash -lc $build
    if ($LASTEXITCODE) { throw 'Video lab build or self-test failed.' }
    $exe = Join-Path $BuildDirectory 'enhancement-video-lab.exe'
    if (Test-Path -LiteralPath (Join-Path $StageDirectory 'enhancement/nvngx.dll')) {
        Copy-Item -LiteralPath $exe -Destination $StageDirectory -Force
        Write-Host "Video lab staged: $(Join-Path $StageDirectory 'enhancement-video-lab.exe')"
    } else {
        Write-Host "Video lab built: $exe"
        Write-Host "No staged worker in $StageDirectory. Run it with --stage <directory holding enhancement/nvngx.dll and runtimes/>."
    }
} finally {
    $env:PATH = $savedPath
    $env:MSYSTEM = $savedMsystem
    $env:CHERE_INVOKING = $savedChere
    Remove-Item Env:VIDEOLAB_BUILD -ErrorAction SilentlyContinue
    Pop-Location
}
