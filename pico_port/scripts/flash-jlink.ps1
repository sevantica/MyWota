# PowerShell script for J-Link flashing - MyWota (RP2040)
#
# NOTE: SEGGER's built-in RP2040 flash loader sizes the QSPI part via SFDP. The
# GigaDevice-equivalent (clone) flash on this board returns no valid SFDP, so SEGGER
# fails with "Algo reported a flash size of 0 bytes". This script therefore programs
# through the RP2040 bootrom flash routines (driven from a RAM stub over SWD) via the
# shared Invoke-JLinkBootromFlash.ps1 helper, which never reads SFDP.
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

$SharedFlasher = "C:\Business\Cross Project\VS Code Common\jlink-rp2040-flash\Invoke-JLinkBootromFlash.ps1"
$JLinkPath = "C:\Program Files\SEGGER\JLink_V862\JLink.exe"
$AppRunAddress = "0x10010000"
$AppFlashOffset = 0x10000
$BootloaderFlashOffset = 0x0

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

$elfFile = Join-Path $BuildDir "$ProjectName.elf"
$binFile = Join-Path $BuildDir "$ProjectName.bin"

if (-not (Test-Path $elfFile)) {
    Write-Error "ELF file not found: $elfFile"
    Write-Host "Build the project first." -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $SharedFlasher)) {
    Write-Error "Shared J-Link bootrom flasher not found: $SharedFlasher"
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

$images = @()
if ($WithBootloader) {
    Write-Host "=== J-Link Flash: MyWota (RP2040) [WITH BOOTLOADER, bootrom path] ===" -ForegroundColor Cyan
    if ([string]::IsNullOrWhiteSpace($BootloaderPath)) {
        $BootloaderPath = Get-NewestSharedBootloader
    }
    if (-not (Test-Path $BootloaderPath)) {
        Write-Error "Bootloader binary not found: $BootloaderPath"
        Write-Host "Build the shared RP2040 bootloader first or specify -BootloaderPath." -ForegroundColor Yellow
        exit 1
    }
    $images += @{ Path = $BootloaderPath; Offset = $BootloaderFlashOffset }
} else {
    Write-Host "=== J-Link Flash: MyWota (RP2040) [APPLICATION ONLY AT $AppRunAddress, bootrom path] ===" -ForegroundColor Cyan
    Write-Host "WARNING: This app is linked for the bootloader boundary and will not boot from reset without a bootloader." -ForegroundColor Yellow
}
$images += @{ Path = $binFile; Offset = $AppFlashOffset }

& $SharedFlasher -Images $images -JLinkPath $JLinkPath
exit $LASTEXITCODE