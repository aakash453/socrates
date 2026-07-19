# build_windows.ps1
# Build Socrates for Windows (Snapdragon X Elite ARM64 or x64)
# Usage: .\build_windows.ps1                    (CPU only)
#        .\build_windows.ps1 -WithQNN            (with Qualcomm NPU)
#        .\build_windows.ps1 -Arch x64           (x64 instead of ARM64)

param(
    [switch]$WithQNN,
    [string]$Arch = "ARM64",
    [string]$QnnSdkRoot = ""
)

Write-Host "=== Socrates Windows Build ===" -ForegroundColor Cyan
Write-Host "Architecture: $Arch" -ForegroundColor Cyan

$buildDir = "build/windows-$($Arch.ToLower())-release"

$cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
if (-not $cmakeExe) {
    $cmakeCandidate = "C:\Program Files\CMake\bin\cmake.exe"
    if (Test-Path $cmakeCandidate) {
        $cmakeExe = $cmakeCandidate
    }
}

if (-not $cmakeExe) {
    Write-Host "CMake not found. Install CMake or add it to PATH." -ForegroundColor Red
    exit 1
}

$generator = "Visual Studio 18 2026"
$toolchainFile = Join-Path (Resolve-Path ".").Path "build/conan-release/conan_toolchain.cmake"
$conanPrefixPath = Join-Path (Resolve-Path ".").Path "build/conan-release"
$enableLlama = "ON"
if ($Arch -eq "ARM64" -and $WithQNN) {
    $enableLlama = "OFF"
    Write-Host "llama.cpp: disabled for Windows ARM64 QNN build (llama.cpp rejects MSVC on ARM)" -ForegroundColor Yellow
}
$cmakeArgs = @(
    "-B", $buildDir,
    "-G", $generator,
    "-A", $Arch,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_SYSTEM_NAME=Windows",
    "-DSOCRATES_ENABLE_TESTS=OFF",
    "-DSOCRATES_ENABLE_TOOLS=ON",
    "-DSOCRATES_ENABLE_LLAMA=$enableLlama",
    "-DSOCRATES_ENABLE_MLX=OFF",
    "-DSOCRATES_WARNINGS_AS_ERRORS=OFF"
)

if (Test-Path $toolchainFile) {
    $normalizedToolchainFile = $toolchainFile -replace '\\','/'
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$normalizedToolchainFile"
}
if (Test-Path $conanPrefixPath) {
    $normalizedConanPrefixPath = $conanPrefixPath -replace '\\','/'
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$normalizedConanPrefixPath"
}

if ($Arch -eq "ARM64") {
    $cmakeArgs += "-DSOCRATES_ENABLE_LITERT=ON"
}

if ($WithQNN) {
    Write-Host "QNN NPU: ENABLED" -ForegroundColor Green
    $cmakeArgs += "-DSOCRATES_ENABLE_QNN=ON"
    if ($QnnSdkRoot) {
        $normalizedQnnSdkRoot = $QnnSdkRoot -replace '\\','/'
        $cmakeArgs += "-DQNN_SDK_ROOT=$normalizedQnnSdkRoot"
    } elseif ($env:QNN_SDK_ROOT) {
        $normalizedQnnSdkRoot = $env:QNN_SDK_ROOT -replace '\\','/'
        $cmakeArgs += "-DQNN_SDK_ROOT=$normalizedQnnSdkRoot"
    }
} else {
    Write-Host "QNN NPU: disabled (CPU fallback only)" -ForegroundColor Yellow
    $cmakeArgs += "-DSOCRATES_ENABLE_QNN=OFF"
}

Write-Host ""
Write-Host "Using CMake: $cmakeExe" -ForegroundColor Cyan
Write-Host "Generator: $generator" -ForegroundColor Cyan
Write-Host "Configuring..." -ForegroundColor Cyan
& $cmakeExe @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configure failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Building..." -ForegroundColor Cyan
& $cmakeExe --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== Build Complete ===" -ForegroundColor Green
$exe = Join-Path $buildDir "tools/Release/socrates-worker.exe"
if (-not (Test-Path $exe)) {
    $exe = Join-Path $buildDir "tools/socrates-worker.exe"
}
if (Test-Path $exe) {
    Write-Host "Worker: $exe" -ForegroundColor Green
    Write-Host "Run: .\$exe --no-discovery" -ForegroundColor Cyan
} else {
    Write-Host "Worker binary not found at expected path." -ForegroundColor Yellow
}
