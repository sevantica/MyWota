param(
    [switch]$WithBootloader,
    [string]$BootloaderPath = "C:\Business\Cross Project\VS Code Common\Pico bootloader\build\bootloader.bin"
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectDir = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectDir "build"
$TargetName = "UI_PICO_PORT"

if ($WithBootloader) {
    Write-Host "=== Flash with Bootloader Mode ===" -ForegroundColor Cyan
    
    # Check bootloader exists
    if (-not (Test-Path $BootloaderPath)) {
        Write-Error "Bootloader not found at: $BootloaderPath"
        Write-Host "Build the bootloader first or specify path with -BootloaderPath" -ForegroundColor Yellow
        exit 1
    }
    
    $AppBin = Join-Path $BuildDir "$TargetName.bin"
    if (-not (Test-Path $AppBin)) {
        Write-Error "App binary not found: $AppBin"
        exit 1
    }
    
    $CombinedUf2 = Join-Path $BuildDir "combined.uf2"
    $BundleScript = Join-Path $ScriptDir "bundle_firmware.py"
    
    Write-Host "Creating combined firmware..."
    Write-Host "  Bootloader: $BootloaderPath"
    Write-Host "  Application: $AppBin"
    
    python $BundleScript --bootloader "$BootloaderPath" --app "$AppBin" --output "$CombinedUf2"
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to create combined firmware"
        exit 1
    }
    
    $Uf2File = $CombinedUf2
    Write-Host "Combined firmware ready: $Uf2File" -ForegroundColor Green
} else {
    $Uf2File = Join-Path $BuildDir "$TargetName.uf2"
    Write-Host "WARNING: App built for bootloader (0x10008000) but flashing WITHOUT bootloader!" -ForegroundColor Yellow
    Write-Host "Use -WithBootloader flag to include bootloader, or device will not boot." -ForegroundColor Yellow
}

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

# Function to find the Pico's COM port
function Find-PicoComPort {
    $ports = Get-CimInstance -ClassName Win32_PnPEntity | Where-Object { 
        $_.Name -match "COM\d+" -and ($_.Name -match "USB Serial|MyWota|Pico|RP2040")
    }
    if ($ports) {
        $portMatch = [regex]::Match($ports[0].Name, "COM(\d+)")
        if ($portMatch.Success) {
            return "COM$($portMatch.Groups[1].Value)"
        }
    }
    # Fallback: try to find any USB serial port
    $allPorts = [System.IO.Ports.SerialPort]::GetPortNames()
    foreach ($port in $allPorts) {
        return $port  # Return first available
    }
    return $null
}

# Try to send bootsel command via serial first
Write-Host "Looking for Pico serial port..."
$ComPort = Find-PicoComPort

if ($ComPort) {
    Write-Host "Found serial port: $ComPort"
    Write-Host "Sending 'bootsel' command to reboot into BOOTSEL mode..."
    try {
        $serial = New-Object System.IO.Ports.SerialPort $ComPort, 115200, None, 8, One
        $serial.ReadTimeout = 500
        $serial.WriteTimeout = 500
        $serial.DtrEnable = $true
        $serial.Open()
        Start-Sleep -Milliseconds 100
        $serial.WriteLine("bootsel")
        Start-Sleep -Milliseconds 200
        $serial.Close()
        Write-Host "Bootsel command sent. Waiting for device to enumerate in BOOTSEL mode..."
        Start-Sleep -Seconds 2
    } catch {
        Write-Host "Could not send bootsel command: $($_.Exception.Message)"
        Write-Host "Will try picotool reboot method..."
    }
} else {
    Write-Host "No serial port found, trying picotool reboot method..."
}

# Attempt to force reboot into BOOTSEL mode using picotool (fallback)
Write-Host "Attempting to force device into BOOTSEL mode via picotool..."
& $PicotoolPath reboot -f -u 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "Device reboot command sent. Waiting for enumeration..."
    Start-Sleep -Seconds 2
}

# Wait a bit more and check if device is in BOOTSEL mode
$retries = 5
$flashSuccess = $false

for ($i = 0; $i -lt $retries; $i++) {
    Write-Host "Flashing $Uf2File (attempt $($i + 1)/$retries)..."
    & $PicotoolPath load -x "$Uf2File" 2>$null
    
    if ($LASTEXITCODE -eq 0) {
        $flashSuccess = $true
        break
    }
    
    if ($i -lt ($retries - 1)) {
        Write-Host "Device not ready, waiting..."
        Start-Sleep -Seconds 1
    }
}

if ($flashSuccess) {
    Write-Host "Flash successful!"
    exit 0
} else {
    Write-Error "Flash failed. Make sure the device is in BOOTSEL mode (hold BOOTSEL while plugging in)."
    exit 1
}
