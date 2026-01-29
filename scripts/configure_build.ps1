# Configure Build Script for MyWota
# This script sets up the proper environment and configures CMake for building

# Set Pico SDK environment variables
$env:PICO_SDK_PATH = "C:/Users/bchir/.pico-sdk/sdk/2.2.0"
$env:PICO_TOOLCHAIN_PATH = "C:/Users/bchir/.pico-sdk/toolchain/14_2_Rel1"

# Clean and create build directory
if (Test-Path "$PSScriptRoot/../build") {
    Remove-Item -Recurse -Force "$PSScriptRoot/../build"
    Write-Host "Build directory cleaned"
}
New-Item -ItemType Directory -Path "$PSScriptRoot/../build" | Out-Null
Write-Host "Build directory created"

# Navigate to build directory and run CMake
Set-Location "$PSScriptRoot/../build"
Write-Host "Configuring with CMake..."
& "C:/Users/bchir/.pico-sdk/cmake/v3.31.5/bin/cmake.exe" -G Ninja -DSKIP_PICO_PROMPT=1 -DPICO_USE_PREBUILT_PICOTOOL=1 ..

if ($LASTEXITCODE -eq 0) {
    Write-Host "Configuration successful!"
} else {
    Write-Host "Configuration failed with exit code: $LASTEXITCODE"
    exit $LASTEXITCODE
}
