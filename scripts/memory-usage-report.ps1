# Memory Usage Report Script for RP2040 Project
param(
    [string]$ProjectPath = $PWD
)

Write-Host "=== MEMORY USAGE REPORT ===" -ForegroundColor Cyan
Write-Host ""

$elfPath = Join-Path $ProjectPath "build\UI_PICO_PORT.elf"

if (Test-Path $elfPath) {
    $toolchainPath = "$env:USERPROFILE\.pico-sdk\toolchain\14_2_Rel1\bin"
    $sizeExe = Join-Path $toolchainPath "arm-none-eabi-size.exe"
    
    if (Test-Path $sizeExe) {
        Write-Host "ELF File Size Analysis:" -ForegroundColor Yellow
        & $sizeExe -A -d $elfPath
        Write-Host ""
        
        Write-Host "Memory Sections Summary:" -ForegroundColor Yellow
        & $sizeExe -B -d $elfPath
        Write-Host ""
        
        # Parse the output to get actual numbers for percentage calculation
        $sizeOutput = & $sizeExe -B -d $elfPath | Out-String
        # Look for the line with numbers (not the header)
        if ($sizeOutput -match "\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+[0-9a-fA-F]+\s+") {
            $textSize = [int]$matches[1]
            $dataSize = [int]$matches[2] 
            $bssSize = [int]$matches[3]
            $totalSize = [int]$matches[4]
            
            $flashUsage = $textSize + $dataSize
            $ramUsage = $dataSize + $bssSize
            
            # RP2040 memory limits
            $flashTotal = 2097152  # 2MB
            $ramTotal = 264192     # 258KB
            
            # Convert to kilobytes for display
            $flashUsageKB = [math]::Round($flashUsage / 1024, 2)
            $flashTotalKB = [math]::Round($flashTotal / 1024, 2)
            $ramUsageKB = [math]::Round($ramUsage / 1024, 2)
            $ramTotalKB = [math]::Round($ramTotal / 1024, 2)
            
            $flashPercent = [math]::Round(($flashUsage / $flashTotal) * 100, 2)
            $ramPercent = [math]::Round(($ramUsage / $ramTotal) * 100, 2)
            
            Write-Host "RP2040 Memory Usage:" -ForegroundColor Green
            Write-Host "Flash: $flashUsageKB kB / $flashTotalKB kB ($flashPercent%)" -ForegroundColor White
            Write-Host "RAM:   $ramUsageKB kB / $ramTotalKB kB ($ramPercent%)" -ForegroundColor White
            
            if ($flashPercent -gt 90) {
                Write-Host "WARNING: Flash usage is over 90%!" -ForegroundColor Red
            } elseif ($flashPercent -gt 75) {
                Write-Host "CAUTION: Flash usage is over 75%" -ForegroundColor Yellow
            }
            
            if ($ramPercent -gt 90) {
                Write-Host "WARNING: RAM usage is over 90%!" -ForegroundColor Red
            } elseif ($ramPercent -gt 75) {
                Write-Host "CAUTION: RAM usage is over 75%" -ForegroundColor Yellow
            }
        } else {
            Write-Host "Could not parse memory usage information" -ForegroundColor Yellow
        }
    } else {
        Write-Host "ARM toolchain not found at: $sizeExe" -ForegroundColor Red
    }
} else {
    Write-Host "ELF file not found at: $elfPath" -ForegroundColor Red
    Write-Host "Please build the project first." -ForegroundColor Yellow
}

Write-Host "=========================" -ForegroundColor Cyan
