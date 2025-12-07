/*
 * @attention
 * Copyright (c) Sevantica 2025.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file Progress_Bar_Example.c
 * @brief Example demonstrating the real-time totalRemainingBar progress updates
 * @details Shows how the progress bar displays the percentage of remaining balance
 *          versus the last top-up amount, updating in real-time during operations
 */

/*Includes ----------------------------------------------------------*/
#include "MIFARE_Dispenser_Integration.h"
#include "LCD_Display_Driver.h"
#include "USB_Logging.h"
#include "FreeRTOS.h"
#include "task.h"

/*Function Implementations ------------------------------------------*/

/**
 * @brief Example demonstrating real-time progress bar updates
 * @details This example shows how the totalRemainingBar updates automatically
 *          during different operations:
 *          - Card initialization: 100% (full top-up)
 *          - Water dispensing: Decreases in real-time
 *          - Card top-up: Resets to 100% with new reference amount
 */
void ProgressBar_RealTimeExample(void)
{
    USB_Log_Printf("=== Real-Time Progress Bar Example ===\r\n");
    
    USB_Log_Printf("\r\n🎯 How the Progress Bar Works:\r\n");
    USB_Log_Printf("   • Shows percentage: (Current Balance / Last Top-up) × 100%%\r\n");
    USB_Log_Printf("   • Updates automatically during card operations\r\n");
    USB_Log_Printf("   • Resets to 100%% after each top-up\r\n");
    
    USB_Log_Printf("\r\n📊 Real-Time Update Scenarios:\r\n");
    
    USB_Log_Printf("\r\n1. 🆕 NEW CARD INITIALIZATION:\r\n");
    USB_Log_Printf("   → Initialize card with 5000mL\r\n");
    USB_Log_Printf("   → Progress bar shows: 100%% (5000/5000)\r\n");
    USB_Log_Printf("   → Balance display: '5.0L'\r\n");
    
    USB_Log_Printf("\r\n2. 💧 WATER DISPENSING (Real-time updates):\r\n");
    USB_Log_Printf("   → Dispense 500mL water\r\n");
    USB_Log_Printf("   → Progress bar updates to: 90%% (4500/5000)\r\n");
    USB_Log_Printf("   → Balance display: '4.5L'\r\n");
    USB_Log_Printf("   → Dispense another 1000mL\r\n");
    USB_Log_Printf("   → Progress bar updates to: 70%% (3500/5000)\r\n");
    USB_Log_Printf("   → Balance display: '3.5L'\r\n");
    
    USB_Log_Printf("\r\n3. 💰 CARD TOP-UP (Progress bar reset):\r\n");
    USB_Log_Printf("   → Add 2000mL to card\r\n");
    USB_Log_Printf("   → New balance: 5500mL\r\n");
    USB_Log_Printf("   → Progress bar resets to: 100%% (5500/2000)\r\n");
    USB_Log_Printf("   → Note: Now calculated against 2000mL top-up amount\r\n");
    USB_Log_Printf("   → Balance display: '5.5L'\r\n");
    
    USB_Log_Printf("\r\n4. 🔄 CONTINUOUS MONITORING:\r\n");
    USB_Log_Printf("   → Progress bar updates every 100ms during dispensing\r\n");
    USB_Log_Printf("   → Smooth visual feedback for users\r\n");
    USB_Log_Printf("   → Automatic updates when card is re-inserted\r\n");
    
    USB_Log_Printf("\r\n=== Progress Bar Example Complete ===\r\n");
}

/**
 * @brief Manual demonstration of different progress bar percentages
 * @details Tests the progress bar with various balance/top-up combinations
 */
void ProgressBar_PercentageDemo(void)
{
    USB_Log_Printf("=== Progress Bar Percentage Demonstration ===\r\n");
    
    struct {
        uint32_t current_balance;
        uint32_t last_topup;
        uint8_t expected_percentage;
        const char* description;
    } test_cases[] = {
        {5000, 5000, 100, "Full card after initialization"},
        {4500, 5000, 90,  "After 500mL dispensed"},
        {3500, 5000, 70,  "After 1500mL total dispensed"},
        {2500, 5000, 50,  "Half remaining from original top-up"},
        {1000, 5000, 20,  "Low balance warning level"},
        {0,    5000, 0,   "Empty card"},
        {2000, 2000, 100, "Full card after 2L top-up"},
        {1500, 2000, 75,  "75% remaining after top-up"},
        {500,  2000, 25,  "25% remaining after top-up"},
        {6000, 2000, 100, "Balance exceeds top-up (capped at 100%)"}
    };
    
    uint8_t num_tests = sizeof(test_cases) / sizeof(test_cases[0]);
    
    for (uint8_t i = 0; i < num_tests; i++) {
        USB_Log_Printf("\r\nTest %d: %s\r\n", i + 1, test_cases[i].description);
        USB_Log_Printf("  Balance: %u mL, Top-up: %u mL\r\n", 
                       test_cases[i].current_balance, test_cases[i].last_topup);
        USB_Log_Printf("  Expected: %u%%, Testing...\r\n", test_cases[i].expected_percentage);
        
        // Manually test the UI update
        ui_update_total_remaining_bar(test_cases[i].current_balance, test_cases[i].last_topup);
        
        // Wait 1.5 seconds for visual verification
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    
    USB_Log_Printf("\r\n=== Percentage Demonstration Complete ===\r\n");
}

/**
 * @brief Simulate a complete dispensing session with progress updates
 * @details Shows real-time updates during a simulated dispensing session
 */
void ProgressBar_SimulateDispensing(void)
{
    USB_Log_Printf("=== Simulated Dispensing Session ===\r\n");
    
    // Simulate initial card with 3000mL balance from 3000mL top-up
    uint32_t balance = 3000;
    uint32_t topup_amount = 3000;
    
    USB_Log_Printf("Starting simulation: Card with %u mL (100%% full)\r\n", balance);
    ui_update_total_remaining_bar(balance, topup_amount);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Simulate dispensing 100mL at a time
    for (uint8_t step = 1; step <= 20; step++) {
        balance -= 100; // Dispense 100mL
        
        uint8_t percentage = (balance * 100) / topup_amount;
        
        USB_Log_Printf("Step %02d: Dispensed 100mL → Balance: %u mL (%u%%)\r\n", 
                       step, balance, percentage);
        
        // Update progress bar in real-time
        ui_update_total_remaining_bar(balance, topup_amount);
        
        // Brief delay to simulate real dispensing time
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Stop at different points to show key percentages
        if (percentage == 75 || percentage == 50 || percentage == 25 || percentage == 10) {
            USB_Log_Printf("  ⚠️  Milestone: %u%% remaining\r\n", percentage);
            vTaskDelay(pdMS_TO_TICKS(1000)); // Pause for emphasis
        }
        
        if (balance <= 100) {
            USB_Log_Printf("  🔴 Low balance warning!\r\n");
            break;
        }
    }
    
    USB_Log_Printf("\r\nSimulation complete. Final balance: %u mL\r\n", balance);
    USB_Log_Printf("=== Dispensing Simulation Complete ===\r\n");
}

/*Integration Notes ----------------------------------------------*/

/*
 * AUTOMATIC PROGRESS BAR INTEGRATION:
 * 
 * The totalRemainingBar is automatically updated by the system at these points:
 * 
 * 1. CARD DETECTION:
 *    - DispenserTask: DISPENSER_IDLE → DISPENSER_CARD_READY
 *    - Calls: UpdateCardUI(balance_ml, last_topup_ml)
 *    - Result: Progress bar shows current percentage
 * 
 * 2. DISPENSING COMPLETION:
 *    - DispenserTask: DISPENSER_COMPLETING state
 *    - Calls: UpdateCardUI(new_balance_ml, last_topup_ml) 
 *    - Result: Progress bar updates to new percentage
 * 
 * 3. CARD TOP-UP:
 *    - MIFARE_Dispenser_TopupCard() function
 *    - Calls: UpdateCardUI(new_balance, topup_amount)
 *    - Result: Progress bar resets to 100% with new reference
 * 
 * 4. NEW CARD INITIALIZATION:
 *    - MIFARE_Dispenser_InitializeNewCustomer() function
 *    - Calls: UpdateCardUI(initial_balance, initial_balance)
 *    - Result: Progress bar shows 100% (full card)
 * 
 * 5. CARD REMOVAL:
 *    - DispenserTask: DISPENSER_CARD_READY → DISPENSER_IDLE
 *    - Calls: UpdateCardUI(0, 0)
 *    - Result: Progress bar shows 0%
 * 
 * REAL-TIME BEHAVIOR:
 * - Updates happen automatically in the background
 * - No manual intervention required
 * - Progress bar smoothly animates between values (LV_ANIM_ON)
 * - Thread-safe LVGL updates with semaphore protection
 * - USB logging shows all percentage changes for debugging
 * 
 * PERCENTAGE CALCULATION:
 * - Formula: (current_balance_ml / last_topup_amount_ml) × 100
 * - Range: 0% to 100% (capped at 100% even if balance exceeds top-up)
 * - Zero handling: Shows 0% when no card present or zero top-up amount
 * - Overflow protection: Prevents display of >100% values
 * 
 * UI THREAD SAFETY:
 * - All updates use LVGL semaphore (lvgl_sem)
 * - Non-blocking semaphore acquisition with timeout
 * - Graceful error handling if UI elements not initialized
 * - Comprehensive logging for debugging
 */