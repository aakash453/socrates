# setup_windows.ps1
# One-time setup for building Socrates on Windows (Snapdragon X Elite / x64)
# Run: .\setup_windows.ps1

Write-Host "=== Socrates Windows Setup ===" -ForegroundColor Cyan

$cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
if (-not $cmakeExe) {
    $cmakeCandidate = "C:\Program Files\CMake\bin\cmake.exe"
    if (Test-Path $cmakeCandidate) {
        $cmakeExe = $cmakeCandidate
    }
}

if ($cmakeExe) {
    Write-Host "CMake: $cmakeExe" -ForegroundColor Green
} else {
    Write-Host "CMake not found. Install with: winget install Kitware.CMake" -ForegroundColor Red
}

$ninjaExe = (Get-Command ninja -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
if ($ninjaExe) {
    Write-Host "Ninja: $ninjaExe" -ForegroundColor Green
} else {
    Write-Host "Ninja not found. This is OK if building with Visual Studio generator." -ForegroundColor Yellow
}

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vsWhere) {
    $vsPath = & $vsWhere -latest -property installationPath 2>$null
}
if ($vsPath) {
    Write-Host "Visual Studio: $vsPath" -ForegroundColor Green
} else {
    Write-Host "Visual Studio Build Tools not found." -ForegroundColor Red
}

$arm64Compiler = $null
if ($vsPath) {
    $arm64Compiler = Get-ChildItem -Path (Join-Path $vsPath 'VC\Tools\MSVC') -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'arm64\\cl\.exe$' } |
        Select-Object -First 1 -ExpandProperty FullName
}

if ($arm64Compiler) {
    Write-Host "ARM64 MSVC: $arm64Compiler" -ForegroundColor Green
} else {
    Write-Host "ARM64 MSVC toolchain not found." -ForegroundColor Red
    Write-Host "  Install/modify Build Tools with:" -ForegroundColor Yellow
    Write-Host "  vs_installer.exe modify --installPath '$vsPath' --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100" -ForegroundColor Yellow
}

if ($env:QNN_SDK_ROOT) {
    $qnnInclude = Join-Path $env:QNN_SDK_ROOT 'include\QNN'
    $qnnLib = Join-Path $env:QNN_SDK_ROOT 'lib\arm64x-windows-msvc\QnnHtp.lib'
    if ((Test-Path $qnnInclude) -and (Test-Path $qnnLib)) {
        Write-Host "QNN SDK: $env:QNN_SDK_ROOT" -ForegroundColor Green
    } else {
        Write-Host "QNN_SDK_ROOT is set but does not look valid for Windows ARM64 QNN." -ForegroundColor Red
    }
} else {
    Write-Host "QNN SDK not set. For NPU builds set QNN_SDK_ROOT." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Setup complete. Run: .\build_windows.ps1" -ForegroundColor Cyan
Write-Host "  For NPU:  .\build_windows.ps1 -WithQNN" -ForegroundColor Cyan
