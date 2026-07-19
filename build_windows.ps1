# build_windows.ps1
# Build Socrates for Windows (Snapdragon X Elite ARM64 or x64)
# Usage: .\build_windows.ps1                    (CPU only)
#        .\build_windows.ps1 -WithQNN            (with Qualcomm NPU)
#        .\build_windows.ps1 -Arch x64           (x64 instead of ARM64)

param(
    [switch]$WithQNN,
    [string]$Arch = "ARM64"
)

Write-Host "=== Socrates Windows Build ===" -ForegroundColor Cyan
Write-Host "Architecture: $Arch" -ForegroundColor Cyan

$buildDir = "build/windows-$($Arch.ToLower())-release"

# Configure
$cmakeArgs = @(
    "-B", $buildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_SYSTEM_NAME=Windows",
    "-DSOCRATES_ENABLE_TESTS=OFF",
    "-DSOCRATES_ENABLE_TOOLS=ON",
    "-DSOCRATES_ENABLE_LLAMA=ON",
    "-DSOCRATES_ENABLE_MLX=OFF",
    "-DSOCRATES_WARNINGS_AS_ERRORS=OFF"
)

if ($Arch -eq "ARM64") {
    $cmakeArgs += "-DCMAKE_GENERATOR_PLATFORM=ARM64"
    $cmakeArgs += "-DSOCRATES_ENABLE_LITERT=ON"
}

if ($WithQNN) {
    Write-Host "QNN NPU: ENABLED" -ForegroundColor Green
    $cmakeArgs += "-DSOCRATES_ENABLE_QNN=ON"
    if ($env:QNN_SDK_ROOT) {
        $cmakeArgs += "-DQNN_SDK_ROOT=$env:QNN_SDK_ROOT"
    }
} else {
    Write-Host "QNN NPU: disabled (CPU fallback only)" -ForegroundColor Yellow
    $cmakeArgs += "-DSOCRATES_ENABLE_QNN=OFF"
}

Write-Host ""
Write-Host "Configuring..." -ForegroundColor Cyan
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configure failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Building..." -ForegroundColor Cyan
& cmake --build $buildDir --parallel $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== Build Complete ===" -ForegroundColor Green
$exe = Join-Path $buildDir "tools/socrates-worker.exe"
if (Test-Path $exe) {
    Write-Host "Worker: $exe" -ForegroundColor Green
    Write-Host "Run: .\$exe --no-discovery" -ForegroundColor Cyan
} else {
    Write-Host "Worker binary not found at expected path." -ForegroundColor Yellow
}
