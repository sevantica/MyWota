param(
    [switch]$WithBootloader,
    [string]$BootloaderPath = ""
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectDir = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectDir "build"
$TargetName = "UI_PICO_PORT"

function Get-PicotoolPath {
    $picoSdkPath = "$env:USERPROFILE\.pico-sdk\picotool\2.2.0\picotool\picotool.exe"
    if (Test-Path $picoSdkPath) {
        return $picoSdkPath
    }

    $picotoolCommand = Get-Command picotool -ErrorAction SilentlyContinue
    if ($picotoolCommand) {
        return $picotoolCommand.Source
    }

    return "picotool"
}

$PicotoolPath = Get-PicotoolPath

function Assert-RP2040Image {
    param(
        [string]$ImagePath,
        [string]$Description
    )

    $info = & $PicotoolPath info -a "$ImagePath" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Could not inspect $Description with picotool: $ImagePath"
        Write-Host $info
        exit 1
    }

    if ($info -notmatch "family ID 'rp2040'" -and $info -notmatch "pico_board:\s+pico") {
        Write-Error "$Description is not an RP2040/Pico image: $ImagePath"
        Write-Host $info
        exit 1
    }
}

function Assert-ConnectedRP2040Bootsel {
    $info = & $PicotoolPath info -a 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Could not inspect connected BOOTSEL device with picotool."
        Write-Host $info
        exit 1
    }

    if ($info -notmatch "type:\s+RP2040") {
        Write-Error "Connected BOOTSEL device is not RP2040. Refusing to flash MyWota firmware."
        Write-Host $info
        exit 1
    }
}

if ($WithBootloader) {
    Write-Host "=== Flash with Bootloader Mode ===" -ForegroundColor Cyan

    if ([string]::IsNullOrWhiteSpace($BootloaderPath)) {
        $BootloaderRoot = "C:\Business\Cross Project\VS Code Common\Pico bootloader"
        $BootloaderCandidates = @(
            (Join-Path $BootloaderRoot "build_agent\bootloader.bin"),
            (Join-Path $BootloaderRoot "build\bootloader.bin")
        ) | Where-Object { Test-Path $_ } | ForEach-Object { Get-Item $_ } | Sort-Object LastWriteTime -Descending

        if ($BootloaderCandidates.Count -gt 0) {
            $BootloaderPath = $BootloaderCandidates[0].FullName
            Write-Host "Selected newest bootloader artifact: $BootloaderPath ($($BootloaderCandidates[0].LastWriteTime))" -ForegroundColor Cyan
        } else {
            $BootloaderPath = Join-Path $BootloaderRoot "build_agent\bootloader.bin"
        }
    }
    
    # Check bootloader exists
    if (-not (Test-Path $BootloaderPath)) {
        Write-Error "Bootloader not found at: $BootloaderPath"
        Write-Host "Build the bootloader first or specify path with -BootloaderPath" -ForegroundColor Yellow
        exit 1
    }
    Assert-RP2040Image -ImagePath $BootloaderPath -Description "Bootloader"
    
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
    Write-Host "WARNING: App built for bootloader (0x10010000) but flashing WITHOUT bootloader!" -ForegroundColor Yellow
    Write-Host "Use -WithBootloader flag to include bootloader, or device will not boot." -ForegroundColor Yellow
}

Write-Host "Looking for firmware at: $Uf2File"

if (-not (Test-Path $Uf2File)) {
    Write-Error "Firmware file not found: $Uf2File"
    Write-Error "Please build the project first."
    exit 1
}

# Try to find picotool
Write-Host "Using picotool: $PicotoolPath"

# Function to find the Pico's COM port
function Find-PicoComPort {
    $ports = Get-CimInstance -ClassName Win32_PnPEntity | Where-Object { 
        $_.Name -match "COM\d+" -and ($_.Name -match "USB Serial|MyWota|Pico|RP2040") -and ($_.Name -notmatch "Bluetooth")
    }
    if ($ports) {
        $portMatch = [regex]::Match($ports[0].Name, "COM(\d+)")
        if ($portMatch.Success) {
            return "COM$($portMatch.Groups[1].Value)"
        }
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
        $serial.WriteLine("sys bootloader")
        Start-Sleep -Milliseconds 200
        $serial.Close()
        Write-Host "Bootsel command sent. Waiting for device to enumerate in BOOTSEL mode..."
        Start-Sleep -Seconds 2
    } catch {
        Write-Host "Could not send bootsel command: $($_.Exception.Message)"
        Write-Host "If the target is already in BOOTSEL, flashing will continue; otherwise put it in BOOTSEL manually."
    }
} else {
    Write-Host "No matching MyWota serial port found. If the target is already in BOOTSEL, flashing will continue; otherwise put it in BOOTSEL manually."
}

# Wait a bit more and check if device is in BOOTSEL mode
$retries = 5
$flashSuccess = $false

for ($i = 0; $i -lt $retries; $i++) {
    Assert-ConnectedRP2040Bootsel

    Write-Host "Flashing $Uf2File (attempt $($i + 1)/$retries)..."
    & $PicotoolPath load --ignore-partitions -v "$Uf2File"
    
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

    Write-Host "Rebooting device out of BOOTSEL mode..."
    & $PicotoolPath reboot -f -a
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Device rebooted to firmware." -ForegroundColor Green
    } else {
        Write-Host "Flash succeeded, but automatic reboot did not complete. Unplug/replug the device if it remains in BOOTSEL." -ForegroundColor Yellow
    }

    exit 0
} else {
    Write-Error "Flash failed. Make sure the device is in BOOTSEL mode (hold BOOTSEL while plugging in)."
    exit 1
}
