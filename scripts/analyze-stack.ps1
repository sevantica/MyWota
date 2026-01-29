# PowerShell script to analyze stack usage from ELF file
param(
    [Parameter(Mandatory=$true)]
    [string]$ElfFile
)

Write-Host "=== Stack Usage Analysis ===" -ForegroundColor Green

if (-not (Test-Path $ElfFile)) {
    Write-Host "ERROR: ELF file not found: $ElfFile" -ForegroundColor Red
    exit 1
}

$objdump = "${env:USERPROFILE}/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-objdump.exe"
$nm = "${env:USERPROFILE}/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-nm.exe"
$size = "${env:USERPROFILE}/.pico-sdk/toolchain/14_2_Rel1/bin/arm-none-eabi-size.exe"

Write-Host "`n--- Section Sizes ---" -ForegroundColor Yellow
& $size -A $ElfFile

Write-Host "`n--- Stack Symbols ---" -ForegroundColor Yellow
& $nm -n $ElfFile | Select-String "stack"

Write-Host "`n--- Task Stack Definitions ---" -ForegroundColor Yellow
& $nm -n $ElfFile | Select-String -i "task.*stack"

Write-Host "`n--- FreeRTOS Task Information ---" -ForegroundColor Yellow
& $nm -n $ElfFile | Select-String -i "task.*handle"

Write-Host "`n--- Memory Layout ---" -ForegroundColor Yellow
& $objdump -h $ElfFile | Select-String -A 20 "Sections:"

Write-Host "`n=== Analysis Complete ===" -ForegroundColor Green
Write-Host "Check for any stack overflows or unusually large stack allocations" -ForegroundColor Cyan