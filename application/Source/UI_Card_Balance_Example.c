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
 * @file UI_Card_Balance_Example.c
 * @brief Example demonstrating the UI card balance update functionality
 * @details Shows how the card remaining balance is automatically updated
 *          on the UI display when cards are detected, dispensing occurs,
 *          and cards are removed
 */

/*Includes ----------------------------------------------------------*/
#include "MIFARE_Dispenser_Integration.h"
#include "LCD_Display_Driver.h"
#include "USB_Logging.h"
#include "FreeRTOS.h"
#include "task.h"

/*Function Implementations ------------------------------------------*/

/**
 * @brief Example demonstrating automatic UI updates during card operations
 * @details This example shows how the cardRemaining UI element is automatically
 *          updated during different card operations:
 *          - Card detection: Shows current balance
 *          - Dispensing: Updates balance after completion
 *          - Card removal: Clears display
 *          - New card initialization: Shows initial balance
 */
void UI_CardBalance_Example(void)
{
    USB_Log_Printf("=== UI Card Balance Update Example ===\r\n");
    
    // The UI updates happen automatically in the background through the
    // MIFARE_Dispenser_Integration system:
    
    USB_Log_Printf("\r\n1. Place a card on the reader...\r\n");
    USB_Log_Printf("   → UI will automatically show the card balance\r\n");
    USB_Log_Printf("   → Display format: X.XL for >= 1L, XmL for < 1L\r\n");
    
    USB_Log_Printf("\r\n2. Request water dispensing...\r\n");
    USB_Log_Printf("   → UI will update with new balance after dispensing\r\n");
    
    USB_Log_Printf("\r\n3. Remove the card...\r\n"); 
    USB_Log_Printf("   → UI will clear to show '0mL'\r\n");
    
    USB_Log_Printf("\r\n4. Initialize a new customer card...\r\n");
    USB_Log_Printf("   → UI will show the initial balance set during initialization\r\n");
    
    USB_Log_Printf("\r\nExample UI Display Formats:\r\n");
    USB_Log_Printf("  - 5000mL balance → Display: '5.0L'\r\n");
    USB_Log_Printf("  - 1500mL balance → Display: '1.5L'\r\n");
    USB_Log_Printf("  - 800mL balance  → Display: '800mL'\r\n");
    USB_Log_Printf("  - 0mL balance    → Display: '0mL'\r\n");
    
    USB_Log_Printf("\r\n=== UI Card Balance Example Complete ===\r\n");
}

/**
 * @brief Manual UI update for testing purposes
 * @param balance_ml Balance to display in milliliters
 * @details This function allows manual testing of the UI update functionality
 */
void UI_CardBalance_ManualTest(uint32_t balance_ml)
{
    USB_Log_Printf("Manual UI Update Test: Setting balance to %u mL\r\n", balance_ml);
    ui_update_card_remaining_balance(balance_ml);
}

/**
 * @brief Demonstration of different balance display formats
 * @details Shows how different balance amounts are formatted on the display
 */
void UI_CardBalance_FormatDemo(void)
{
    USB_Log_Printf("=== UI Card Balance Format Demonstration ===\r\n");
    
    uint32_t test_balances[] = {
        0,      // 0mL
        250,    // 250mL
        500,    // 500mL  
        1000,   // 1.0L
        1500,   // 1.5L
        2750,   // 2.7L
        5000,   // 5.0L
        10000   // 10.0L
    };
    
    const char* expected_displays[] = {
        "0mL",
        "250mL", 
        "500mL",
        "1.0L",
        "1.5L", 
        "2.7L",
        "5.0L",
        "10.0L"
    };
    
    uint8_t num_tests = sizeof(test_balances) / sizeof(test_balances[0]);
    
    for (uint8_t i = 0; i < num_tests; i++) {
        USB_Log_Printf("Testing %u mL → Expected display: %s\r\n", 
                       test_balances[i], expected_displays[i]);
        
        ui_update_card_remaining_balance(test_balances[i]);
        
        // Wait 2 seconds between updates for visual verification
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
    USB_Log_Printf("=== Format Demonstration Complete ===\r\n");
}

/*Usage Instructions ------------------------------------------------*/

/*
 * HOW THE UI UPDATE SYSTEM WORKS:
 * 
 * The cardRemaining UI element is automatically updated by the 
 * MIFARE_Dispenser_Integration system at these key points:
 * 
 * 1. CARD DETECTED (DISPENSER_IDLE → DISPENSER_CARD_READY):
 *    - Calls: ui_update_card_remaining_balance(MIFARE_GetBalanceML())
 *    - Result: Shows current card balance on UI
 * 
 * 2. DISPENSING COMPLETED (DISPENSER_COMPLETING):
 *    - Calls: ui_update_card_remaining_balance(MIFARE_GetBalanceML()) 
 *    - Result: Shows updated balance after water dispensed
 * 
 * 3. CARD REMOVED (DISPENSER_CARD_READY → DISPENSER_IDLE):
 *    - Calls: ui_update_card_remaining_balance(0)
 *    - Result: Clears display to show '0mL'
 * 
 * 4. NEW CARD INITIALIZED:
 *    - Calls: ui_update_card_remaining_balance(initial_balance_ml)
 *    - Result: Shows the initial balance set during card setup
 * 
 * DISPLAY FORMATS:
 * - Values >= 1000mL: Displayed as "X.XL" (e.g., "2.5L")  
 * - Values < 1000mL:  Displayed as "XmL" (e.g., "750mL")
 * - Zero balance:     Displayed as "0mL"
 * 
 * THREAD SAFETY:
 * - All UI updates use LVGL semaphore protection
 * - Safe to call from any FreeRTOS task
 * - Automatically handles cases where UI elements aren't initialized
 * 
 * INTEGRATION POINTS:
 * - DispenserTask: Monitors card state and triggers updates
 * - MIFARE_Dispenser_InitializeNewCustomer: Updates after card init
 * - MIFARE_Dispenser_UpdateUI: Manual update function for periodic refresh
 * 
 * TESTING:
 * - Use UI_CardBalance_ManualTest() to test specific values  
 * - Use UI_CardBalance_FormatDemo() to see all format examples
 * - Monitor USB logs to see when updates occur
 */