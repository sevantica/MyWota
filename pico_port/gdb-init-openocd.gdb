# GDB commands for Pico Debug via OpenOCD
# This script works around cortex-debug limitations

# Connect to OpenOCD
target remote localhost:3333

# Configure GDB
set confirm off
set print pretty on
set pagination off
set mem inaccessible-by-default off

# Reset and halt target
monitor reset init
monitor halt

# Load firmware
load

# Set breakpoints
break main
break vApplicationMallocFailedHook
break vApplicationStackOverflowHook  
break HardFault_Handler_C

# Display test mode debug help
printf "\n=== Test Mode Debug Commands ===\n"
printf "p test_mode_data.balance - Check balance\n"
printf "p test_mode_data.dispensing_active - Check if dispensing\n"
printf "p test_mode_data.flow_rate_ml_per_min - Check flow rate\n"
printf "call MIFARE_Dispenser_GetTestModeStatus() - Get status\n"
printf "==============================\n\n"

# Continue to main
continue