# PowerShell script for J-Link flashing - MyWota (RP2040)
param(
    [string]$BuildDir = "",
    [string]$ProjectName = "UI_PICO_PORT",
    [switch]$WithBootloader,
    [string]$BootloaderPath = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectDir = Split-Path -Parent $ScriptDir

if ($BuildDir -eq "") {
    $BuildDir = Join-Path $ProjectDir "build"
}

function Get-NewestSharedBootloader {
    $BootloaderRoot = "C:\Business\Cross Project\VS Code Common\Pico bootloader"
    $BootloaderCandidates = @(
        (Join-Path $BootloaderRoot "build_agent\bootloader.bin"),
        (Join-Path $BootloaderRoot "build\bootloader.bin")
    ) | Where-Object { Test-Path $_ } | ForEach-Object { Get-Item $_ } | Sort-Object LastWriteTime -Descending

    if ($BootloaderCandidates.Count -gt 0) {
        Write-Host "Selected newest bootloader artifact: $($BootloaderCandidates[0].FullName) ($($BootloaderCandidates[0].LastWriteTime))" -ForegroundColor Cyan
        return $BootloaderCandidates[0].FullName
    }

    return (Join-Path $BootloaderRoot "build\bootloader.bin")
}

$JLinkDevice = "RP2040_M0_0"
$JLinkSpeed = 4000
$JLinkPath = "C:\Program Files\SEGGER\JLink_V862\JLink.exe"
$AppRunAddress = "0x10010000"
$BootloaderRunAddress = "0x10000000"
$AppProgramAddress = "0x12010000"
$BootloaderProgramAddress = "0x12000000"

$elfFile = Join-Path $BuildDir "$ProjectName.elf"
$binFile = Join-Path $BuildDir "$ProjectName.bin"

if ($WithBootloader) {
    Write-Host "=== J-Link Flash: MyWota (RP2040) [WITH BOOTLOADER] ===" -ForegroundColor Cyan
    if ([string]::IsNullOrWhiteSpace($BootloaderPath)) {
        $BootloaderPath = Get-NewestSharedBootloader
    }
    if (-not (Test-Path $BootloaderPath)) {
        Write-Error "Bootloader binary not found: $BootloaderPath"
        Write-Host "Build the shared RP2040 bootloader first or specify -BootloaderPath." -ForegroundColor Yellow
        exit 1
    }
} else {
    Write-Host "=== J-Link Flash: MyWota (RP2040) [APPLICATION ONLY AT $AppRunAddress] ===" -ForegroundColor Cyan
    Write-Host "WARNING: This app is linked for the bootloader boundary and will not boot from reset without a bootloader." -ForegroundColor Yellow
}

if (-not (Test-Path $elfFile)) {
    Write-Error "ELF file not found: $elfFile"
    Write-Host "Build the project first." -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $JLinkPath)) {
    Write-Error "J-Link executable not found: $JLinkPath"
    exit 1
}

Write-Host "Converting ELF to binary..."
$objcopy = Join-Path $env:USERPROFILE ".pico-sdk\toolchain\14_2_Rel1\bin\arm-none-eabi-objcopy.exe"
& $objcopy -O binary $elfFile $binFile
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to convert ELF to binary"
    exit 1
}

$jlinkCmdFile = Join-Path $BuildDir "jlink_flash_cmd.jlink"

if ($WithBootloader) {
    Write-Host "Preparing J-Link commands for bootloader + app..."
    @"
r
h
loadbin "$BootloaderPath" $BootloaderProgramAddress
loadbin "$binFile" $AppProgramAddress
r
go
q
"@ | Set-Content -Path $jlinkCmdFile -Encoding ASCII
} else {
    Write-Host "Preparing J-Link commands for app only..."
    @"
r
h
loadbin "$binFile" $AppProgramAddress
r
go
q
"@ | Set-Content -Path $jlinkCmdFile -Encoding ASCII
}

Write-Host "Flashing via J-Link (SWD)..."
Write-Host "  Device: $JLinkDevice"
if ($WithBootloader) {
    Write-Host "  Bootloader: $BootloaderPath @ QSPI $BootloaderProgramAddress (runs at $BootloaderRunAddress)"
}
Write-Host "  Application: $binFile @ QSPI $AppProgramAddress (runs at $AppRunAddress)"

$jlinkOutput = & $JLinkPath -device $JLinkDevice -if SWD -speed $JLinkSpeed -autoconnect 1 -CommandFile $jlinkCmdFile 2>&1
$jlinkExitCode = $LASTEXITCODE
$jlinkOutput | ForEach-Object { Write-Host $_ }
$jlinkOutputText = $jlinkOutput | Out-String

if ($jlinkExitCode -ne 0 -or $jlinkOutputText -match "(?i)(\*\*\*\*\*\* Error|ERROR:|Unspecified error|Could not connect|Failed to power up DAP|VTref=0\.000V)") {
    if ($jlinkOutputText -match "SEGGER_OPEN_GetFlashInfo\(\): Algo reported a flash size of 0 bytes") {
        Write-Host "J-Link connected to the RP2040 core, but SEGGER's RP2040 QSPI flash loader could not detect the external flash." -ForegroundColor Yellow
        Write-Host "Flash this board with picotool/BOOTSEL, then use the 'MyWota - J-Link Debug' launch config to debug the already-flashed app." -ForegroundColor Yellow
    }
    Write-Error "Flashing failed. J-Link reported an error."
    if ($jlinkExitCode -ne 0) {
        exit $jlinkExitCode
    }
    exit 1
}

Write-Host "Firmware flashed successfully!" -ForegroundColor Green