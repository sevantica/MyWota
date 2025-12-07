$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectDir = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectDir "build"
$TargetName = "UI_PICO_PORT"
$Uf2File = Join-Path $BuildDir "$TargetName.uf2"

Write-Host "Looking for firmware at: $Uf2File"

if (-not (Test-Path $Uf2File)) {
    Write-Error "Firmware file not found: $Uf2File"
    Write-Error "Please build the project first."
    exit 1
}

# Try to find picotool
$PicotoolPath = "picotool" # Default to PATH
# Check common locations if not in path
$PicoSdkPath = "$env:USERPROFILE\.pico-sdk\picotool\2.2.0\picotool\picotool.exe"
if (Test-Path $PicoSdkPath) {
    $PicotoolPath = $PicoSdkPath
}

Write-Host "Using picotool: $PicotoolPath"

# Attempt to force reboot into BOOTSEL mode
Write-Host "Attempting to force device into BOOTSEL mode..."
& $PicotoolPath reboot -f -u
if ($LASTEXITCODE -eq 0) {
    Write-Host "Device reboot command sent. Waiting for enumeration..."
    Start-Sleep -Seconds 3
} else {
    Write-Host "Could not force reboot (device might already be in BOOTSEL mode or not found via USB serial)."
}

Write-Host "Flashing $Uf2File..."
# -x means execute (reboot) after load
& $PicotoolPath load -x "$Uf2File"

if ($LASTEXITCODE -eq 0) {
    Write-Host "Flash successful!"
} else {
    Write-Error "Flash failed. Make sure the device is in BOOTSEL mode (hold BOOTSEL while plugging in)."
    exit $LASTEXITCODE
}
