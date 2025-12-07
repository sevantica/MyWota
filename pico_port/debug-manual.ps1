# Manual GDB Debug Script for Pico Debug Probe
# This bypasses cortex-debug limitations

Write-Host "=== Pico Debug Probe - Manual GDB Session ===" -ForegroundColor Cyan
Write-Host ""

# Check if OpenOCD is running
$openocd = Get-Process *openocd* -ErrorAction SilentlyContinue
if (-not $openocd) {
    Write-Host "Starting OpenOCD..." -ForegroundColor Yellow
    Start-Process -FilePath "$env:USERPROFILE/.pico-sdk/openocd/0.12.0+dev/openocd.exe" `
        -ArgumentList "-s", "$env:USERPROFILE/.pico-sdk/openocd/0.12.0+dev/scripts", `
                      "-f", "interface/cmsis-dap.cfg", `
                      "-f", "target/rp2040.cfg" `
        -NoNewWindow -PassThru
    Start-Sleep -Seconds 3
}

# Check if build exists
if (-not (Test-Path "build/UI_PICO_PORT.elf")) {
    Write-Host "Error: build/UI_PICO_PORT.elf not found!" -ForegroundColor Red
    Write-Host "Run 'Build' task first." -ForegroundColor Red
    exit 1
}

Write-Host "OpenOCD is running (PID: $($openocd.Id))" -ForegroundColor Green
Write-Host "Launching GDB with OpenOCD connection..." -ForegroundColor Yellow
Write-Host ""

# Launch GDB with the init script
& "$env:USERPROFILE/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-gdb.exe" `
    -x "gdb-init-openocd.gdb" `
    "build/UI_PICO_PORT.elf"