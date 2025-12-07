# Smart J-Link server starter - only starts if not already running
Write-Host "Checking J-Link GDB Server status..."

# Check if J-Link server is already running and port is available
$existingProcess = Get-Process -Name "JLinkGDBServerCL" -ErrorAction SilentlyContinue
$portInUse = Get-NetTCPConnection -LocalPort 2331 -ErrorAction SilentlyContinue

if ($existingProcess -and $portInUse) {
    Write-Host "J-Link GDB Server is already running on port 2331 (PID: $($existingProcess.Id))"
    Write-Host "Reusing existing server connection"
    exit 0
}

# If process exists but port isn't available, clean up
if ($existingProcess -and -not $portInUse) {
    Write-Host "Found stale J-Link process, cleaning up..."
    $existingProcess | Stop-Process -Force
    Start-Sleep -Seconds 1
}

# Start new J-Link GDB Server
Write-Host "Starting new J-Link GDB Server..."
$jlinkPath = "C:\Program Files\SEGGER\JLink_V862\JLinkGDBServerCL.exe"

# Test if the executable exists
if (-not (Test-Path $jlinkPath)) {
    Write-Error "J-Link executable not found at: $jlinkPath"
    exit 1
}

# Start J-Link GDB Server in a new window with verify enabled
$processStartInfo = New-Object System.Diagnostics.ProcessStartInfo
$processStartInfo.FileName = $jlinkPath
$processStartInfo.Arguments = "-if SWD -device RP2040_M0_0 -speed 4000 -port 2331 -localhostonly -vd -strict"
$processStartInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Minimized
$processStartInfo.UseShellExecute = $true

$process = [System.Diagnostics.Process]::Start($processStartInfo)

# Wait for the server to start
Start-Sleep -Seconds 3

# Verify the server started properly
$portInUse = Get-NetTCPConnection -LocalPort 2331 -ErrorAction SilentlyContinue
if ($portInUse) {
    Write-Host "J-Link GDB Server started successfully on port 2331"
    Write-Host "Server ready for debugging connections"
} else {
    Write-Warning "J-Link GDB Server may not have started properly"
    Write-Host "Please check if J-Link hardware is connected"
    exit 1
}
