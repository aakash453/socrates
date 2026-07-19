# setup_windows.ps1
# One-time setup for building Socrates on Windows (Snapdragon X Elite / x64)
# Run: .\setup_windows.ps1

Write-Host "=== Socrates Windows Setup ===" -ForegroundColor Cyan

# Check for CMake
if (!(Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "Installing CMake..." -ForegroundColor Yellow
    winget install Kitware.CMake
}
Write-Host "CMake: $(cmake --version | Select-Object -First 1)" -ForegroundColor Green

# Check for Ninja
if (!(Get-Command ninja -ErrorAction SilentlyContinue)) {
    Write-Host "Installing Ninja..." -ForegroundColor Yellow
    winget install Ninja-build.Ninja
}
Write-Host "Ninja: $(ninja --version)" -ForegroundColor Green

# Check for Visual Studio Build Tools
$vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2>$null
if ($vsPath) {
    Write-Host "Visual Studio: $vsPath" -ForegroundColor Green
} else {
    Write-Host "WARNING: Visual Studio not found. Install Build Tools for ARM64:" -ForegroundColor Yellow
    Write-Host "  winget install Microsoft.VisualStudio.2022.BuildTools --override '--add Microsoft.VisualStudio.Component.VC.Tools.ARM64'"
}

# Check for Qualcomm QNN SDK (optional, for NPU)
if ($env:QNN_SDK_ROOT) {
    Write-Host "QNN SDK: $env:QNN_SDK_ROOT" -ForegroundColor Green
} else {
    Write-Host "QNN SDK not found (NPU disabled)." -ForegroundColor Yellow
    Write-Host "  Download: https://developer.qualcomm.com/software/qualcomm-neural-processing-sdk" -ForegroundColor Yellow
    Write-Host "  Then set: `$env:QNN_SDK_ROOT = 'C:\Qualcomm\QNN\<version>'" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Setup complete. Run: .\build_windows.ps1" -ForegroundColor Cyan
Write-Host "  For NPU:  .\build_windows.ps1 -WithQNN" -ForegroundColor Cyan
