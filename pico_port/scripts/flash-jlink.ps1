# PowerShell script for J-Link flashing
param(
    [string]$BuildDir = "build",
    [string]$ProjectName = "UI_PICO_PORT"
)

Write-Host "J-Link Flash Script Starting..."

# Convert ELF to binary
$elfFile = "$BuildDir\$ProjectName.elf"
$binFile = "$BuildDir\$ProjectName.bin"

if (-not (Test-Path $elfFile)) {
    Write-Error "ELF file not found: $elfFile"
    exit 1
}

Write-Host "Converting ELF to binary..."
& "$env:USERPROFILE/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-objcopy.exe" -O binary $elfFile $binFile

if (-not (Test-Path $binFile)) {
    Write-Error "Failed to create binary file: $binFile"
    exit 1
}

# Flash using J-Link
$jlinkPath = "C:\Program Files\SEGGER\JLink_V862\JLink.exe"
$jlinkScript = "jlink_binary_flash.jlink"

if (-not (Test-Path $jlinkPath)) {
    Write-Error "J-Link executable not found: $jlinkPath"
    exit 1
}

if (-not (Test-Path $jlinkScript)) {
    Write-Error "J-Link script not found: $jlinkScript"
    exit 1
}

Write-Host "Flashing firmware via J-Link..."
& $jlinkPath -device RP2040_M0_0 -if SWD -speed 4000 -autoconnect 1 -CommandFile $jlinkScript

if ($LASTEXITCODE -eq 0) {
    Write-Host "Firmware flashed successfully!" -ForegroundColor Green
} else {
    Write-Error "Flashing failed with exit code: $LASTEXITCODE"
    exit $LASTEXITCODE
}